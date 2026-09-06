#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#define TAG         "SonicPay"
#define MIC_CHANNEL ADC_CHANNEL_6
#define SAMPLE_RATE 40000

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
#define SYMBOL_BUF_LEN SYMBOL_SAMPLES
static int16_t symbolBuf[SYMBOL_BUF_LEN];
static int sampleIdx = 0;

static adc_continuous_handle_t adc_handle = NULL;

// ── ADC init ──────────────────────────────────────────────────────
static void initHardware(void) {
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 16384,
        .conv_frame_size    = 1024,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &adc_handle));

    adc_digi_pattern_config_t adc_pattern[1] = {0};
    adc_pattern[0].atten     = ADC_ATTEN_DB_11;
    adc_pattern[0].channel   = MIC_CHANNEL;
    adc_pattern[0].unit      = ADC_UNIT_1;
    adc_pattern[0].bit_width = ADC_BITWIDTH_12;

    adc_continuous_config_t dig_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = adc_pattern,
        .sample_freq_hz = SAMPLE_RATE,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

    ESP_LOGI(TAG, "Hardware ready (Continuous DMA mode)");
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

    if (maxPower < 5e6) return -1;
    if (snr < 5.0f)     return -1;
    return maxIdx;
}

// ── Bit accumulator ───────────────────────────────────────────────
static uint8_t  payloadBuf[MAX_PAYLOAD_BYTES];
static int      payloadBits  = 0;
static int      payloadBytes = 0;

static void resetPayload(void) {
    memset(payloadBuf, 0, sizeof(payloadBuf));
    payloadBits  = 0;
    payloadBytes = 0;
}

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
    int        dataCount   = 0;
    int        diagCount   = 0;

    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "Demod running — waiting for preamble...");
    ESP_LOGI(TAG, "[DIAG] Will print raw power/SNR every ~2s while idle");

    uint8_t rx_buf[1024];

    while (1) {
        esp_task_wdt_reset();

        uint32_t ret_num = 0;
        esp_err_t ret = adc_continuous_read(adc_handle, rx_buf, sizeof(rx_buf), &ret_num, pdMS_TO_TICKS(10));
        if (ret != ESP_OK || ret_num == 0) {
            vTaskDelay(1);
            continue;
        }

        adc_digi_output_data_t *p = (adc_digi_output_data_t *)rx_buf;
        uint32_t count = ret_num / sizeof(adc_digi_output_data_t);

        for (uint32_t i = 0; i < count; i++) {
            if (p[i].type1.channel == MIC_CHANNEL) {
                symbolBuf[sampleIdx++] = (int16_t)((int)p[i].type1.data - 2048);
            }

            if (sampleIdx < SYMBOL_BUF_LEN) {
                continue;
            }

            // Symbol window complete
            sampleIdx = 0;

            int16_t localBuf[SYMBOL_BUF_LEN];
            memcpy(localBuf, symbolBuf, sizeof(localBuf));

            float rawPower = 0, rawSnr = 0;
            int tone = detectToneFull(localBuf, SYMBOL_BUF_LEN, &rawPower, &rawSnr);

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
                            ESP_LOGI(TAG, "Preamble locked (%d F0s) — awaiting first data symbol", f0Count);
                            state     = STATE_DATA;
                            dataCount = 0;
                            resetPayload();
                        } else {
                            ESP_LOGI(TAG, "Preamble F0 count: %d / %d", f0Count, PREAMBLE_SYMS);
                        }
                    } else if (tone == -1) {
                        // acoustic dropout
                    } else {
                        ESP_LOGW(TAG, "Preamble broken — back to idle");
                        f0Count = 0;
                        state   = STATE_IDLE;
                    }
                    break;

                case STATE_DATA:
                    if (tone == 0) {
                        if (dataCount > 0) {
                            postCount = 1;
                            state     = STATE_POSTAMBLE;
                        }
                    } else if (tone == -1) {
                        // silence mid-data
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
                            for (int k = 0; k < payloadBytes; k++) {
                                printf("%02X ", payloadBuf[k]);
                            }
                            printf("\n");
                            printf("RAW STRING: %.*s\n", payloadBytes, (char*)payloadBuf);
                            state     = STATE_IDLE;
                            f0Count   = 0;
                            postCount = 0;
                            dataCount = 0;
                        }
                    } else if (tone != -1) {
                        pushBits(0);
                        pushBits(tone);
                        dataCount += 2;
                        state = STATE_DATA;
                    }
                    break;
            }
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "=== SonicPay Piece 3: Preamble + Demodulation ===");
    initHardware();
    xTaskCreate(demodTask, "demod", 8192, NULL, 1, NULL);
}
