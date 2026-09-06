#include <Arduino.h>

#define MIC_PIN     34
#define SAMPLE_RATE 40000
#define BUF_LEN     1024

volatile int16_t samples[BUF_LEN];
volatile int sampleIdx = 0;
volatile bool bufReady = false;

hw_timer_t* timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onTimer() {
    portENTER_CRITICAL_ISR(&timerMux);
    if (sampleIdx < BUF_LEN && !bufReady) {
        samples[sampleIdx++] = (int16_t)(analogRead(MIC_PIN) - 2048);
        if (sampleIdx >= BUF_LEN) {
            bufReady = true;
            sampleIdx = 0;
        }
    }
    portEXIT_CRITICAL_ISR(&timerMux);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== SonicPay Piece 1: ADC capture ===");

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    timer = timerBegin(0, 80, true);
    timerAttachInterrupt(timer, &onTimer, true);
    timerAlarmWrite(timer, 1000000 / SAMPLE_RATE, true);
    timerAlarmEnable(timer);

    Serial.println("Timer started. Sampling at 40kHz...");
}

void loop() {
    if (bufReady) {
        // compute min, max, average
        int32_t sum = 0;
        int16_t minVal =  32767;
        int16_t maxVal = -32768;

        for (int i = 0; i < BUF_LEN; i++) {
            int16_t s = samples[i];
            sum += s;
            if (s < minVal) minVal = s;
            if (s > maxVal) maxVal = s;
        }
        int16_t avg = sum / BUF_LEN;

        // print stats
        Serial.printf("min=%6d  max=%6d  avg=%6d  range=%d\n",
                      minVal, maxVal, avg, maxVal - minVal);

        // print first 16 samples
        Serial.print("samples: ");
        for (int i = 0; i < 16; i++) {
            Serial.printf("%6d ", samples[i]);
        }
        Serial.println();

        // silence warning
        if ((maxVal - minVal) < 50) {
            Serial.println("⚠ Very low range — check mic wiring or gain pot");
        }

        bufReady = false;
    }
}
