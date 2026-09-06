#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#define TAG         "SonicPay"
#define MIC_CHANNEL ADC1_CHANNEL_6
#define SAMPLE_RATE 40000
#define BUF_LEN     1024

// ── FSK config ────────────────────────────────────────────────────
#define NUM_TONES       8
#define SYMBOL_SAMPLES  3200   // 80ms * 40000 = 3200 samples per symbol
#define PREAMBLE_MS     240    // 3 symbols × 80ms
#define POSTAMBLE_MS    160    // 2 symbols × 80ms
#define PREAMBLE_SYMS   3
#define POSTAMBLE_SYMS  2
#define MAX_PAYLOAD_BYTES 512

static const float TARGET_FREQS[NUM_TONES] = {
    12000, 12400, 12800, 13200,
    13600, 14000, 14400, 14800
};

// ── State machine ─────────────────────────────────────────────────
typedef enum {
    STATE_IDLE,        // waiting for preamble
    STATE_PREAMBLE,    // counting F0 symbols
    STATE_DATA,        // decoding symbols
    STATE_POSTAMBLE    // counting F0 postamble
} DemodState;

// ── Sample buffer ─────────────────────────────────────────────────
// We collect samples into a symbol-sized window
#define SYMBOL_BUF_LEN SYMBOL_SAMPLES
static int16_t symbolBuf[SYMBOL_BUF_LEN];
static volatile int  sampleIdx = 0;
static volatile bool symbolReady = false;

static gptimer_handle_t timer = NULL;

// ── Timer ISR ─────────────────────────────────────────────────────
static bool IRAM_ATTR onTimer(gptimer_handle_t t,
                               const gptimer_alarm_event_data_t *edata,
                               void *user_ctx) {
    if (sampleIdx < SYMBOL_BUF_LEN && !symbolReady) {
        int raw = adc1_get_raw(MIC_CHANNEL);
        symbolBuf[sampleIdx++] = (int16_t)(raw - 2048);
        if (sampleIdx >= SYMBOL_BUF_LEN) {
            symbolReady = true;
            sampleIdx   = 0;
        }
    }
    return false;
}

// ── ADC + timer init ──────────────────────────────────────────────
static void initHardware(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(MIC_CHANNEL, ADC_ATTEN_DB_11);

    gptimer_config_t cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &timer));

    gptimer_alarm_config_t alarm = {
        .alarm_count                = 1000000 / SAMPLE_RATE,
        .reload_count               = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm));

    gptimer_event_callbacks_t cbs = { .on_alarm = onTimer };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    ESP_ERROR_CHECK(gptimer_start(timer));

    ESP_LOGI(TAG, "Hardware ready");
}

// ── Goertzel ──────────────────────────────────────────────────────
static float goertzel(int16_t *buf, int len, float freq, float sr) {
    float k     = (float)len * freq / sr;
    float omega = 2.0f * M_PI * k / (float)len;
    float coeff = 2.0f * cosf(omega);
    float s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < len; i++) {
        s0 = (float)buf[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// ── Detect dominant tone index 0-7, -1 = silence ──────────────────
// Returns detected tone index (0-7) or -1 for silence.
// Also fills out_maxPower and out_snr for diagnostics if non-NULL.
static int detectToneFull(int16_t *buf, int len, float *out_maxPower, float *out_snr) {
    float powers[NUM_TONES];
    float maxPower = 0;
    float sumPower = 0;
    int   maxIdx   = 0;

    for (int i = 0; i < NUM_TONES; i++) {
        powers[i] = goertzel(buf, len, TARGET_FREQS[i], SAMPLE_RATE);
        sumPower += powers[i];
        if (powers[i] > maxPower) {
            maxPower = powers[i];
            maxIdx   = i;
        }
    }

    float avgOther = (sumPower - maxPower) / (NUM_TONES - 1);
    float snr = (avgOther > 0) ? maxPower / avgOther : 0;

    if (out_maxPower) *out_maxPower = maxPower;
    if (out_snr)      *out_snr      = snr;

    // Real signal: 28M-50M power. Ambient noise: 50K-3M. Gate at 5M.
    if (maxPower < 5e6) return -1;
    // SNR must be clear — real tones show 8-30x, noise is 2-4x
    if (snr < 5.0f)     return -1;
    return maxIdx;
}

static int detectTone(int16_t *buf, int len) {
    return detectToneFull(buf, len, NULL, NULL);
}

// ── Bit accumulator ───────────────────────────────────────────────
static uint8_t  payloadBuf[MAX_PAYLOAD_BYTES];
static int      payloadBits  = 0;   // total bits accumulated
static int      payloadBytes = 0;

static void resetPayload(void) {
    memset(payloadBuf, 0, sizeof(payloadBuf));
    payloadBits  = 0;
    payloadBytes = 0;
}

// Push 3 bits (one FSK symbol) into payloadBuf
static void pushBits(int symbolIdx) {
    for (int bit = 2; bit >= 0; bit--) {
        int b        = (symbolIdx >> bit) & 1;
        int byteIdx  = payloadBits / 8;
        int bitIdx   = 7 - (payloadBits % 8);
        if (byteIdx < MAX_PAYLOAD_BYTES) {
            payloadBuf[byteIdx] |= (b << bitIdx);
        }
        payloadBits++;
    }
    payloadBytes = (payloadBits + 7) / 8;
}

// ── Demod task ────────────────────────────────────────────────────
static void demodTask(void *arg) {
    DemodState state       = STATE_IDLE;
    int        f0Count     = 0;
    int        postCount   = 0;
    int        dataCount   = 0;  // non-F0 data symbols received — gate for postamble
    int        diagCount   = 0;  // counts symbols processed for periodic diagnostics

    esp_task_wdt_add(NULL); // Register demodTask with Task WDT
    ESP_LOGI(TAG, "Demod running — waiting for preamble...");
    ESP_LOGI(TAG, "[DIAG] Will print raw power/SNR every ~2s while idle");

    while (1) {
        esp_task_wdt_reset(); // Feed watchdog timer

        if (!symbolReady) {
            vTaskDelay(1);
            continue;
        }

        // copy symbol buffer safely
        int16_t localBuf[SYMBOL_BUF_LEN];
        memcpy(localBuf, symbolBuf, sizeof(localBuf));
        symbolReady = false;

        float rawPower = 0, rawSnr = 0;
        int tone = detectToneFull(localBuf, SYMBOL_BUF_LEN, &rawPower, &rawSnr);

        // Diagnostics: every 50 symbols (~2s) while idle, print raw signal levels
        if (state == STATE_IDLE) {
            diagCount++;
            if (diagCount >= 50) {
                diagCount = 0;
                ESP_LOGI(TAG, "[DIAG] maxPower=%.0f snr=%.2f tone=%d", rawPower, rawSnr, tone);
            }
        }

        switch (state) {

            case STATE_IDLE:
                if (tone == 0) {
                    f0Count = 1;
                    state   = STATE_PREAMBLE;
                    ESP_LOGI(TAG, "Preamble starting...");
                }
                break;

            case STATE_PREAMBLE:
                if (tone == 0) {
                    f0Count++;
                    if (f0Count >= PREAMBLE_SYMS) {
                        // Consume any remaining preamble F0 symbols by staying in PREAMBLE
                        // until we see a non-F0, non-silence tone — that's the first data symbol
                        ESP_LOGI(TAG, "Preamble locked (%d F0s) — awaiting first data symbol", f0Count);
                        state     = STATE_DATA;
                        dataCount = 0;
                        resetPayload();
                    } else {
                        ESP_LOGI(TAG, "Preamble F0 count: %d / %d", f0Count, PREAMBLE_SYMS);
                    }
                } else if (tone == -1) {
                    // momentary acoustic dropout — hold count
                } else {
                    // false start (different frequency)
                    ESP_LOGW(TAG, "Preamble broken — back to idle");
                    f0Count = 0;
                    state   = STATE_IDLE;
                }
                break;

            case STATE_DATA:
                if (tone == 0) {
                    // Only treat F0 as postamble if we've seen at least 1 real data symbol.
                    // This prevents the preamble tail (extra F0 symbols) from firing postamble.
                    if (dataCount > 0) {
                        postCount = 1;
                        state     = STATE_POSTAMBLE;
                    }
                    // else: still flushing preamble tail — ignore this F0
                } else if (tone == -1) {
                    // silence mid-data — skip symbol
                } else {
                    pushBits(tone);
                    dataCount++;
                }
                break;

            case STATE_POSTAMBLE:
                if (tone == 0) {
                    postCount++;
                    if (postCount >= POSTAMBLE_SYMS) {
                        ESP_LOGI(TAG, "=== Transmission complete ===");
                        ESP_LOGI(TAG, "Total bits: %d | bytes: %d", payloadBits, payloadBytes);
                        printf("RAW BYTES (hex): ");
                        for (int i = 0; i < payloadBytes; i++) {
                            printf("%02X ", payloadBuf[i]);
                        }
                        printf("\n");
                        printf("RAW STRING: %.*s\n", payloadBytes, (char*)payloadBuf);
                        state     = STATE_IDLE;
                        f0Count   = 0;
                        postCount = 0;
                        dataCount = 0;
                    }
                } else if (tone != -1) {
                    // was a data symbol not postamble
                    pushBits(0);       // the F0 we held back was actually data
                    pushBits(tone);
                    dataCount += 2;
                    state = STATE_DATA;
                }
                break;
        }
        vTaskDelay(1); // Yield to FreeRTOS IDLE task
    }
}

// ── Entry point ───────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "=== SonicPay Piece 3: Preamble + Demodulation ===");
    initHardware();
    xTaskCreate(demodTask, "demod", 8192, NULL, 1, NULL);
}
