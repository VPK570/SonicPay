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

#define TAG         "SonicPay"
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
// 8-FSK: 3 bits/sym → ceil(560/3) = 188 data symbols
// Symbol: 50 ms  |  Guard: 10 ms  |  Sample rate: 36096 Hz
// Samples/symbol: 36096 * 0.05 = 1804
// Samples/guard:  36096 * 0.01 = 360

#define NUM_TONES          9
#define SYMBOL_DURATION_MS 50
#define GUARD_DURATION_MS  10
#define SYMBOL_SAMPLES     (DECODE_RATE * SYMBOL_DURATION_MS / 1000)  // 1804
#define GUARD_SAMPLES      (DECODE_RATE * GUARD_DURATION_MS  / 1000)  // 360

#define PREAMBLE_SYMS       3
#define POSTAMBLE_SYMS      2
#define NUM_DATA_SYMS       188   // ceil(560 / 3)
#define MAX_PAYLOAD_BYTES   128   // 70 bytes needed; headroom for debug

#define PREAMBLE_SNR_THRESHOLD  5.0f   // F0 is lower-freq; separate from data threshold
#define DATA_SNR_THRESHOLD      50.0f  // Real tones: SNR 85–2584; ambient noise: SNR 3–107
#define IDLE_TIMEOUT_MS         3000   // 188 syms × 60ms = 11.28s; timeout must be >> that

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

    ESP_LOGI(TAG, "Hardware ready — 8-FSK 2050–3600 Hz, 50ms symbols, 36096 Hz ADC");
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
// Returns 0–8 (index into TARGET_FREQS) or -1 if no clear tone.
static int detectToneFull(int16_t *buf, int len, float *out_maxPower, float *out_snr) {
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

    if (maxPower < 1e6f) return -1;
    if (snr < DATA_SNR_THRESHOLD) return -1;
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

// Push 3 bits from an 8-FSK symbol (MSB first).
// symbolVal: 0–7 (maps to bits 000–111).
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
// Packet layout (70 bytes):
//   [0–1]   amount_paise  : uint16 big-endian (divide by 100 → rupees)
//   [2–3]   nonce         : uint16 big-endian
//   [4–67]  Ed25519 sig   : 64 bytes (verified in Piece 4 with Monocypher)
//   [68–69] CRC-16/CCITT  : uint16 big-endian, over bytes 0–67
static void processPayload(void) {
    ESP_LOGI(TAG, "=== Transmission complete: %d bits / %d bytes ===", payloadBits, payloadBytes);

    // Dump raw hex for debugging
    printf("RAW BYTES: ");
    for (int k = 0; k < payloadBytes && k < MAX_PAYLOAD_BYTES; k++) {
        printf("%02X ", payloadBuf[k]);
    }
    printf("\n");

    if (payloadBytes < 70) {
        ESP_LOGW(TAG, "Short packet (%d / 70 bytes) — ignoring", payloadBytes);
        return;
    }

    uint16_t amountPaise = ((uint16_t)payloadBuf[0] << 8) | payloadBuf[1];
    uint16_t nonce       = ((uint16_t)payloadBuf[2] << 8) | payloadBuf[3];
    // bytes [4..67] = Ed25519 signature (64 bytes) — verified in Piece 4
    uint16_t rxCrc       = ((uint16_t)payloadBuf[68] << 8) | payloadBuf[69];
    uint16_t calCrc      = crc16_ccitt(payloadBuf, 68);  // CRC over first 68 bytes

    // ── CRC check ──────────────────────────────────────────────────
    if (rxCrc != calCrc) {
        ESP_LOGW(TAG, "CRC mismatch (rx=0x%04X calc=0x%04X) — packet discarded", rxCrc, calCrc);
        return;
    }

    // ── Ed25519 signature verify ────────────────────────────────────
    // TODO(Piece 4): call crypto_eddsa_check() with Monocypher here.
    // For now, log the signature bytes so we can confirm they arrive correctly.
    ESP_LOGI(TAG, "Signature (first 8 bytes): "
             "%02X%02X%02X%02X %02X%02X%02X%02X ...",
             payloadBuf[4],  payloadBuf[5],  payloadBuf[6],  payloadBuf[7],
             payloadBuf[8],  payloadBuf[9],  payloadBuf[10], payloadBuf[11]);

    // Amount is stored as integer paise; divide by 100 for display.
    uint32_t rupees = amountPaise / 100;
    uint32_t paise  = amountPaise % 100;

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  ✅ CRC16 VALID (sig verify pending Piece 4)    ");
    ESP_LOGI(TAG, "  💰 AMOUNT : ₹%"PRIu32".%02"PRIu32"  (%u paise)  ", rupees, paise, amountPaise);
    ESP_LOGI(TAG, "  🔢 NONCE  : #%u                                ", nonce);
    ESP_LOGI(TAG, "=================================================");
}

// ── Demod task ────────────────────────────────────────────────────
static void demodTask(void *arg) {
    DemodState state             = STATE_IDLE;
    int        consecutiveF0     = 0;
    int        postCount         = 0;
    int        dataCount         = 0;
    int        silenceCount      = 0;
    int64_t    lastSymbolTimeMs  = 0;
    int64_t    preambleLockTimeMs = 0;

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

        // Idle timeout: if we haven't seen a symbol in a long time, flush.
        // Timeout must exceed full transmission: 193 slots × 60ms ≈ 11.6s → use 3000ms
        // (this only fires if we're in mid-decode and the phone drops out)
        if (state != STATE_IDLE && (nowMs - lastSymbolTimeMs) > IDLE_TIMEOUT_MS) {
            if (dataCount > 0) {
                ESP_LOGW(TAG, "Timeout mid-decode (%d symbols) — processing partial packet", dataCount);
                processPayload();
            } else {
                ESP_LOGW(TAG, "Timeout — flushing to IDLE");
            }
            state             = STATE_IDLE;
            consecutiveF0     = 0;
            postCount         = 0;
            dataCount         = 0;
            silenceCount      = 0;
            skipGuardSamples  = 0;
            sampleIdx         = 0;
        }

        adc_digi_output_data_t *p     = (adc_digi_output_data_t *)rx_buf;
        uint32_t                count = ret_num / sizeof(adc_digi_output_data_t);

        for (uint32_t i = 0; i < count; i++) {
            if (p[i].type1.channel != MIC_CHANNEL) continue;

            // Skip guard-gap samples (10ms silence between symbols)
            if (skipGuardSamples > 0) {
                skipGuardSamples--;
                continue;
            }

            // Accumulate into symbol buffer (zero-centred)
            symbolBuf[sampleIdx++] = (int16_t)((int)p[i].type1.data - 2048);

            if (sampleIdx < SYMBOL_SAMPLES) continue;  // not full yet

            // Full symbol window captured — reset for next slot
            sampleIdx        = 0;
            lastSymbolTimeMs = esp_timer_get_time() / 1000;
            skipGuardSamples = GUARD_SAMPLES;

            // Work on a local copy so the main buffer is free immediately
            static int16_t localBuf[SYMBOL_SAMPLES];
            memcpy(localBuf, symbolBuf, sizeof(localBuf));

            float rawPower = 0, rawSnr = 0;
            int   tone     = detectToneFull(localBuf, SYMBOL_SAMPLES, &rawPower, &rawSnr);

            switch (state) {

                // ────────────────── IDLE / PREAMBLE ───────────────────
                case STATE_IDLE:
                case STATE_PREAMBLE:
                    if (tone == 0 && rawSnr >= PREAMBLE_SNR_THRESHOLD) {
                        consecutiveF0++;
                        ESP_LOGI(TAG, "Preamble F0 hit %d/%d (SNR=%.1f, pwr=%.0f)",
                                 consecutiveF0, PREAMBLE_SYMS, rawSnr, rawPower);
                        if (consecutiveF0 >= PREAMBLE_SYMS) {
                            ESP_LOGI(TAG, "🔒 Preamble locked — decoding 188 × 8-FSK symbols");
                            state              = STATE_DATA;
                            consecutiveF0      = 0;
                            dataCount          = 0;
                            postCount          = 0;
                            silenceCount       = 0;
                            preambleLockTimeMs = lastSymbolTimeMs;
                            resetPayload();
                        } else {
                            state = STATE_PREAMBLE;
                        }
                    } else if (tone == -1) {
                        // brief silence / ambient noise — stay put, don't reset
                    } else {
                        // Wrong tone while waiting for preamble
                        consecutiveF0 = 0;
                        state         = STATE_IDLE;
                    }
                    break;

                // ────────────────── DATA ───────────────────────────────
                case STATE_DATA:
                    if (tone >= 1 && tone <= 8) {
                        silenceCount = 0;
                        int symVal   = tone - 1;   // 0–7 maps to F1–F8
                        pushBits3(symVal);
                        dataCount++;
                        ESP_LOGI(TAG, "Symbol %3d/%d +%lldms: 8-FSK val=%d bits=%d%d%d (F%d=%.0fHz snr=%.1f)",
                                 dataCount, NUM_DATA_SYMS,
                                 (long long)(lastSymbolTimeMs - preambleLockTimeMs),
                                 symVal,
                                 (symVal >> 2) & 1, (symVal >> 1) & 1, symVal & 1,
                                 tone, TARGET_FREQS[tone], rawSnr);
                        if (dataCount >= NUM_DATA_SYMS) {
                            ESP_LOGI(TAG, "All %d symbols received — waiting for postamble", NUM_DATA_SYMS);
                            state     = STATE_POSTAMBLE;
                            postCount = 0;
                        }
                    } else if (tone == -1) {
                        // Silent slot — can be dropout or end-of-transmission
                        if (dataCount > 0) {
                            silenceCount++;
                            if (silenceCount >= 3) {
                                ESP_LOGW(TAG, "3 silent slots — flushing mid-decode (%d symbols)", dataCount);
                                processPayload();
                                state            = STATE_IDLE;
                                consecutiveF0    = 0;
                                postCount        = 0;
                                dataCount        = 0;
                                silenceCount     = 0;
                                skipGuardSamples = 0;
                            }
                        }
                    }
                    // tone == 0 (F0 sync) seen during data: unexpected; ignore this slot
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
                            silenceCount     = 0;
                            skipGuardSamples = 0;
                        }
                    } else {
                        // Non-F0 tone (data band) or silence during postamble window.
                        // Do NOT re-enter DATA — this path was consuming ambient noise
                        // as fake late symbols and preventing postamble lock.
                        // Just log and stay here; idle timeout will flush if F0 never arrives.
                        if (tone != -1) {
                            ESP_LOGD(TAG, "Postamble: ignoring non-F0 tone %d (SNR=%.1f)", tone, rawSnr);
                        }
                    }
                    break;
            }
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "=== SonicPay Piece 3: 8-FSK Demodulation (2050–3600 Hz) ===");
    ESP_LOGI(TAG, "Packet: [amount_paise:2][nonce:2][sig:64][crc:2] = 70 bytes");
    ESP_LOGI(TAG, "188 data symbols × 50ms + 10ms guard = ~11.3s TX");
    initHardware();
    xTaskCreate(demodTask, "demod", 16384, NULL, 1, NULL);
}
