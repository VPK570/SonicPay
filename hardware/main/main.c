#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "monocypher.h"
#include "peripherals.h"
#include "ledger.h"

#define TAG         "SonicPay"

// Hardcoded Ed25519 Public Key for SonicPay Terminal Verification
// (Matches the public key generated / used by the phone app)
static const uint8_t MERCHANT_PUBLIC_KEY[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#define MIC_CHANNEL ADC_CHANNEL_6
#define ADC_REQUEST_RATE 44100
#define DECODE_RATE      36096
#define CONV_FRAME_SIZE_BYTES 1024

// ── Frequency & Timing Config ─────────────────────────────────────
// 8-FSK: 9 tones total (F0 = sync, F1–F8 = data)
//
//   F0 = 2050 Hz  — sync only (preamble + postamble, never data)
//   F1 = 2200 Hz  — data value 0  (bits 000)
//   F2 = 2400 Hz  — data value 1  (bits 001)
//   F3 = 2600 Hz  — data value 2  (bits 010)
//   F4 = 2800 Hz  — data value 3  (bits 011)
//   F5 = 3000 Hz  — data value 4  (bits 100)
//   F6 = 3200 Hz  — data value 5  (bits 101)
//   F7 = 3400 Hz  — data value 6  (bits 110)
//   F8 = 3600 Hz  — data value 7  (bits 111)
//
// Packet: [amount_paise:2][nonce:2][signature:64][crc:2] = 70 bytes = 560 bits
// 8-FSK: 3 bits/sym → ceil(560/3) = 187 data symbols
// Symbol: 50 ms  |  Guard: 10 ms  |  Sample rate: 36096 Hz
// Samples/symbol: 36096 * 0.05 = 1804
// Samples/guard:  36096 * 0.01 = 360

#define NUM_TONES          9
#define SYMBOL_DURATION_MS 50
#define GUARD_DURATION_MS  10
#define SYMBOL_SAMPLES     (DECODE_RATE * SYMBOL_DURATION_MS / 1000)  // 1804
#define GUARD_SAMPLES      (DECODE_RATE * GUARD_DURATION_MS  / 1000)  // 360

#define PREAMBLE_SYMS       5     // Require 5 consecutive F0 hits (300ms) to lock preamble
#define POSTAMBLE_SYMS      2
#define NUM_DATA_SYMS       187   // ceil(560 / 3) = 187 symbols for 70 bytes
#define MAX_PAYLOAD_BYTES   128   // 70 bytes needed; headroom for debug

// Dynamic Thresholds
#define PREAMBLE_SNR_THRESHOLD  45.0f   // Above ambient (~43-51 SNR) — requires real phone chirp (80-380+)
#define DATA_SNR_THRESHOLD       6.0f   // Sensitive reception during data decoding
#define MIN_POWER_THRESHOLD     1e6f   // Minimum power gate

// Preamble consecutive-hit timeout guard (500 ms = ~8 symbol slots)
#define PREAMBLE_HIT_TIMEOUT_MS  500

// Idle flush timeout (800 ms for fast error detection without dropping normal payments)
#define IDLE_TIMEOUT_MS  800

static const float TARGET_FREQS[NUM_TONES] = {
    2050,  // Tone 0: 2.050 kHz (Dedicated Sync — preamble + postamble)
    2200,  // Tone 1: 2.200 kHz (Data 000)
    2400,  // Tone 2: 2.400 kHz (Data 001)
    2600,  // Tone 3: 2.600 kHz (Data 010)
    2800,  // Tone 4: 2.800 kHz (Data 011)
    3000,  // Tone 5: 3.000 kHz (Data 100)
    3200,  // Tone 6: 3.200 kHz (Data 101)
    3400,  // Tone 7: 3.400 kHz (Data 110)
    3600,  // Tone 8: 3.600 kHz (Data 111)
};

// ── State machine ─────────────────────────────────────────────────
typedef enum {
    STATE_IDLE,       // waiting for preamble lock
    STATE_PREAMBLE,   // verifying 2050 Hz preamble hits
    STATE_DATA,       // decoding 3-bit 8-FSK symbols
    STATE_POSTAMBLE   // verifying 2050 Hz postamble
} DemodState;

// ── Sample buffer ─────────────────────────────────────────────────
static int16_t symbolBuf[SYMBOL_SAMPLES];
static int     sampleIdx        = 0;
static int     skipGuardSamples = 0;

static adc_continuous_handle_t adc_handle = NULL;

// ── ADC init ──────────────────────────────────────────────────────
static void initHardware(void) {
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 16384,
        .conv_frame_size    = CONV_FRAME_SIZE_BYTES,
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
        .sample_freq_hz = ADC_REQUEST_RATE,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

    ESP_LOGI(TAG, "ADC hardware ready (36096 Hz continuous)");
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

// ── Detect dominant tone ──────────────────────────────────────────
static int detectToneFull(int16_t *buf, int len, float *out_maxPower, float *out_snr, float minPower, float minSnr) {
    float powers[NUM_TONES];
    float maxPower = 0;
    float sumPower = 0;
    int   maxIdx   = 0;

    for (int i = 0; i < NUM_TONES; i++) {
        powers[i] = goertzel(buf, len, TARGET_FREQS[i], DECODE_RATE);
        sumPower += powers[i];
        if (powers[i] > maxPower) {
            maxPower = powers[i];
            maxIdx   = i;
        }
    }

    float avgOther = (sumPower - maxPower) / (NUM_TONES - 1);
    float snr      = (avgOther > 0) ? maxPower / avgOther : 0;

    if (out_maxPower) *out_maxPower = maxPower;
    if (out_snr)      *out_snr      = snr;

    if (maxPower < minPower) return -1;
    if (snr < minSnr) return -1;
    return maxIdx;
}

// ── Payload bit accumulator ───────────────────────────────────────
static uint8_t payloadBuf[MAX_PAYLOAD_BYTES];
static int     payloadBits  = 0;
static int     payloadBytes = 0;

static void resetPayload(void) {
    memset(payloadBuf, 0, sizeof(payloadBuf));
    payloadBits  = 0;
    payloadBytes = 0;
}

static void pushBits3(int symbolVal) {
    for (int bit = 2; bit >= 0; bit--) {
        int b       = (symbolVal >> bit) & 1;
        int byteIdx = payloadBits / 8;
        int bitIdx  = 7 - (payloadBits % 8);
        if (byteIdx < MAX_PAYLOAD_BYTES) {
            payloadBuf[byteIdx] |= (b << bitIdx);
        }
        payloadBits++;
    }
    payloadBytes = (payloadBits + 7) / 8;
}

// ── CRC-16/CCITT ─────────────────────────────────────────────────
static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc & 0xFFFF;
}

// ── Packet processor ─────────────────────────────────────────────
static void processPayload(void) {
    ESP_LOGI(TAG, "=== Transmission complete: %d bits / %d bytes ===", payloadBits, payloadBytes);

    printf("RAW BYTES: ");
    for (int k = 0; k < payloadBytes && k < MAX_PAYLOAD_BYTES; k++) {
        printf("%02X ", payloadBuf[k]);
    }
    printf("\n");

    if (payloadBytes < 70) {
        ESP_LOGW(TAG, "Short packet (%d / 70 bytes) — ignoring", payloadBytes);
        led_red_on();
        buzzer_play_error();
        oled_show_error("Short Packet");
        vTaskDelay(pdMS_TO_TICKS(2000));
        leds_off();
        oled_show_ready();
        return;
    }

    uint16_t amountRupees = ((uint16_t)payloadBuf[0] << 8) | payloadBuf[1];
    uint16_t nonce        = ((uint16_t)payloadBuf[2] << 8) | payloadBuf[3];
    uint16_t rxCrc        = ((uint16_t)payloadBuf[68] << 8) | payloadBuf[69];
    uint16_t calCrc       = crc16_ccitt(payloadBuf, 68);

    if (rxCrc != calCrc) {
        ESP_LOGW(TAG, "CRC mismatch (rx=0x%04X calc=0x%04X) — packet discarded", rxCrc, calCrc);
        led_red_on();
        buzzer_play_error();
        oled_show_error("CRC Error");
        vTaskDelay(pdMS_TO_TICKS(2000));
        leds_off();
        oled_show_ready();
        return;
    }

    // ── Ed25519 signature verify ────────────────────────────────────
    const uint8_t *msg = payloadBuf;        // first 4 bytes
    const uint8_t *sig = payloadBuf + 4;    // 64-byte signature
    
    int sigStatus = crypto_ed25519_check(sig, MERCHANT_PUBLIC_KEY, msg, 4);

    uint32_t rupees = amountRupees;
    uint32_t paise  = 0;

    if (sigStatus != 0) {
        ESP_LOGW(TAG, "❌ SIGNATURE VERIFICATION FAILED!");
        led_red_on();
        buzzer_play_error();
        oled_show_error("Invalid Sig");
        vTaskDelay(pdMS_TO_TICKS(2500));
        leds_off();
        oled_show_ready();
        return;
    }

    // ── Replay attack check (SPIFFS ledger) ─────────────────────────
    if (ledger_contains_nonce(nonce)) {
        ESP_LOGW(TAG, "⚠️ REPLAY ATTACK DETECTED — Nonce #%u already processed", nonce);
        led_red_on();
        buzzer_play_error();
        oled_show_replay_error(nonce);
        vTaskDelay(pdMS_TO_TICKS(3000));
        leds_off();
        oled_show_ready();
        return;
    }

    // ── Record transaction & give Success feedback ──────────────────
    ledger_add_entry(nonce, (uint32_t)amountRupees * 100);

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  ✅ TRANSACTION APPROVED & RECORDED!            ");
    ESP_LOGI(TAG, "  💰 AMOUNT : ₹%"PRIu32".%02"PRIu32"  (%u rupees) ", rupees, paise, amountRupees);
    ESP_LOGI(TAG, "  🔢 NONCE  : #%u                                ", nonce);
    ESP_LOGI(TAG, "=================================================");

    led_green_on();
    oled_show_success(rupees, paise, nonce);
    buzzer_play_success();

    vTaskDelay(pdMS_TO_TICKS(3500));
    leds_off();
    oled_show_ready();
}

// ── Demod task ────────────────────────────────────────────────────
static void demodTask(void *arg) {
    DemodState state              = STATE_IDLE;
    int        consecutiveF0      = 0;
    int        postCount          = 0;
    int        dataCount          = 0;
    int        silenceCount       = 0;
    int64_t    lastSymbolTimeMs   = 0;
    int64_t    preambleLockTimeMs = 0;
    int64_t    lastF0HitMs        = 0;

    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "Demod running — waiting for preamble (F0 = 2050 Hz)...");

    static uint8_t rx_buf[1024];

    while (1) {
        esp_task_wdt_reset();

        uint32_t  ret_num = 0;
        esp_err_t ret     = adc_continuous_read(adc_handle, rx_buf, sizeof(rx_buf), &ret_num, pdMS_TO_TICKS(10));
        if (ret != ESP_OK || ret_num == 0) {
            vTaskDelay(1);
            continue;
        }

        int64_t nowMs = esp_timer_get_time() / 1000;

        if (state != STATE_IDLE && (nowMs - lastSymbolTimeMs) > IDLE_TIMEOUT_MS) {
            if (dataCount > 0) {
                ESP_LOGW(TAG, "Idle timeout — flushing (%d symbols received)", dataCount);
                processPayload();
            } else {
                ESP_LOGW(TAG, "Idle timeout — flushing to IDLE (no data)");
                oled_show_ready();
            }
            state             = STATE_IDLE;
            consecutiveF0     = 0;
            postCount         = 0;
            dataCount         = 0;
            skipGuardSamples  = 0;
            sampleIdx         = 0;
            lastF0HitMs       = 0;
        }

        adc_digi_output_data_t *p     = (adc_digi_output_data_t *)rx_buf;
        uint32_t                count = ret_num / sizeof(adc_digi_output_data_t);

        for (uint32_t i = 0; i < count; i++) {
            if (p[i].type1.channel != MIC_CHANNEL) continue;

            if (skipGuardSamples > 0) {
                skipGuardSamples--;
                continue;
            }

            symbolBuf[sampleIdx++] = (int16_t)((int)p[i].type1.data - 2048);

            if (sampleIdx < SYMBOL_SAMPLES) continue;

            sampleIdx        = 0;
            lastSymbolTimeMs = esp_timer_get_time() / 1000;

            // Only skip guard gaps AFTER preamble lock is established!
            // In IDLE/PREAMBLE, skipping 10ms gaps phase-shifts the window and drops preamble hits.
            if (state == STATE_DATA || state == STATE_POSTAMBLE) {
                skipGuardSamples = GUARD_SAMPLES;
            } else {
                skipGuardSamples = 0;
            }

            static int16_t localBuf[SYMBOL_SAMPLES];
            memcpy(localBuf, symbolBuf, sizeof(localBuf));

            float rawPower = 0, rawSnr = 0;
            // Two-Stage Thresholds:
            // - IDLE/PREAMBLE: Power 1e6, SNR 30.0 -> Locks reliably on phone speaker at 15cm
            // - DATA: Power 1e4, SNR 6.0 -> Sensitive reception for all 187 data symbols
            float minPwr = (state == STATE_IDLE || state == STATE_PREAMBLE) ? 1e6f : 1e4f;
            float minSnr = (state == STATE_IDLE || state == STATE_PREAMBLE) ? 30.0f : 6.0f;

            int tone = detectToneFull(localBuf, SYMBOL_SAMPLES, &rawPower, &rawSnr, minPwr, minSnr);

            switch (state) {

                // ────────────────── IDLE / PREAMBLE ───────────────────
                case STATE_IDLE:
                case STATE_PREAMBLE:
                    if (tone == 0 && rawSnr >= PREAMBLE_SNR_THRESHOLD) {
                        if (consecutiveF0 > 0 &&
                            (lastSymbolTimeMs - lastF0HitMs) > PREAMBLE_HIT_TIMEOUT_MS) {
                            ESP_LOGW(TAG, "Preamble gap too large (%lld ms) — resetting counter",
                                     (long long)(lastSymbolTimeMs - lastF0HitMs));
                            consecutiveF0 = 0;
                        }
                        consecutiveF0++;
                        lastF0HitMs = lastSymbolTimeMs;

                        ESP_LOGI(TAG, "Preamble F0 hit %d/%d (SNR=%.1f, pwr=%.0f)",
                                 consecutiveF0, PREAMBLE_SYMS, rawSnr, rawPower);

                        if (consecutiveF0 >= PREAMBLE_SYMS) {
                            ESP_LOGI(TAG, "🔒 Preamble locked — decoding 187 × 8-FSK symbols");
                            state              = STATE_DATA;
                            consecutiveF0      = 0;
                            dataCount          = 0;
                            postCount          = 0;
                            preambleLockTimeMs = lastSymbolTimeMs;
                            lastF0HitMs        = 0;
                            resetPayload();
                            oled_show_listening();
                        } else {
                            state = STATE_PREAMBLE;
                        }
                    } else if (tone == -1) {
                        // Silence / dropout
                    } else {
                        consecutiveF0 = 0;
                        lastF0HitMs   = 0;
                        state         = STATE_IDLE;
                    }
                    break;

                // ────────────────── DATA ───────────────────────────────
                case STATE_DATA:
                    if (tone >= 1 && tone <= 8) {
                        silenceCount = 0;
                        int symVal = tone - 1;
                        pushBits3(symVal);
                        dataCount++;
                        ESP_LOGI(TAG, "Symbol %3d/%d +%lldms: 8-FSK val=%d bits=%d%d%d (F%d=%.0fHz snr=%.1f)",
                                 dataCount, NUM_DATA_SYMS,
                                 (long long)(lastSymbolTimeMs - preambleLockTimeMs),
                                 symVal,
                                 (symVal >> 2) & 1, (symVal >> 1) & 1, symVal & 1,
                                 tone, TARGET_FREQS[tone], rawSnr);

                        if (dataCount % 20 == 0 || dataCount == NUM_DATA_SYMS) {
                            oled_show_receiving(dataCount, NUM_DATA_SYMS);
                        }

                        if (dataCount >= NUM_DATA_SYMS) {
                            ESP_LOGI(TAG, "All %d symbols received — waiting for postamble", NUM_DATA_SYMS);
                            state     = STATE_POSTAMBLE;
                            postCount = 0;
                        }
                    } else if (tone == -1) {
                        silenceCount++;
                        // 8 consecutive silent/corrupted symbol slots (~480ms) during DATA -> fast fail!
                        if (silenceCount >= 8) {
                            ESP_LOGW(TAG, "⚡ Signal Dropout — 8 consecutive silent symbol slots (~480ms) during DATA");
                            led_red_on();
                            buzzer_play_error();
                            oled_show_error("Signal Lost");
                            vTaskDelay(pdMS_TO_TICKS(1500));
                            leds_off();
                            oled_show_ready();

                            state            = STATE_IDLE;
                            consecutiveF0    = 0;
                            postCount        = 0;
                            dataCount        = 0;
                            silenceCount     = 0;
                            skipGuardSamples = 0;
                            lastF0HitMs      = 0;
                        }
                    }
                    break;

                // ────────────────── POSTAMBLE ──────────────────────────
                case STATE_POSTAMBLE:
                    if (tone == 0 && rawSnr >= PREAMBLE_SNR_THRESHOLD) {
                        postCount++;
                        ESP_LOGI(TAG, "Postamble F0 hit %d/%d (SNR=%.1f)", postCount, POSTAMBLE_SYMS, rawSnr);
                        if (postCount >= POSTAMBLE_SYMS) {
                            processPayload();
                            state            = STATE_IDLE;
                            consecutiveF0    = 0;
                            postCount        = 0;
                            dataCount        = 0;
                            skipGuardSamples = 0;
                            lastF0HitMs      = 0;
                        }
                    } else {
                        if (tone != -1) {
                            ESP_LOGD(TAG, "Postamble: ignoring tone %d (SNR=%.1f)", tone, rawSnr);
                        }
                    }
                    break;
            }
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "=== SonicPay Full Terminal (8-FSK + Crypto + SPIFFS + OLED/LED/Buzzer) ===");
    
    // Init hardware peripherals & SPIFFS ledger
    peripherals_init();
    ledger_init();
    initHardware();

    xTaskCreate(demodTask, "demod", 16384, NULL, 1, NULL);
}
