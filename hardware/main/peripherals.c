#include "peripherals.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1306.h"
#include "ledger.h"

#define TAG "PERIPHERALS"

#define BUZZER_TIMER       LEDC_TIMER_1
#define BUZZER_MODE        LEDC_LOW_SPEED_MODE
#define BUZZER_CHANNEL     LEDC_CHANNEL_3
#define BUZZER_DUTY_RES    LEDC_TIMER_10_BIT

void peripherals_init(void) {
    // 1. Initialize RGB LEDs (25, 26, 27) & Discrete LEDs (32, 33)
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << PIN_RGB_RED) | (1ULL << PIN_RGB_GREEN) | (1ULL << PIN_RGB_BLUE) |
                        (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&led_cfg);

    // Set drive strength to MAXIMUM (GPIO_DRIVE_CAP_3 ~40mA) for max brightness
    gpio_set_drive_capability(PIN_RGB_RED, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_RGB_GREEN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_RGB_BLUE, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_LED_GREEN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_LED_RED, GPIO_DRIVE_CAP_3);

    leds_off();

    // 2. Initialize Buzzer LEDC PWM
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .speed_mode = BUZZER_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .gpio_num = PIN_BUZZER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_cfg);

    // 3. Initialize OLED
    ssd1306_init();

    // Self-test LED pulse (Full power RGB + Discrete LEDs)
    led_green_on();
    vTaskDelay(pdMS_TO_TICKS(150));
    leds_off();
    led_red_on();
    vTaskDelay(pdMS_TO_TICKS(150));
    leds_off();

    ESP_LOGI(TAG, "Peripherals & OLED self-test complete");
}

// ── LED Helpers ───────────────────────────────────────────────────
// Common Anode RGB: 0 = 100% MAXIMUM POWER ON, 1 = OFF
// Discrete LEDs: 1 = ON, 0 = OFF

void led_green_on(void) {
    gpio_set_level(PIN_RGB_GREEN, 0); // 100% full power Green RGB
    gpio_set_level(PIN_LED_GREEN, 1); // Discrete Green LED
}

void led_green_off(void) {
    gpio_set_level(PIN_RGB_GREEN, 1);
    gpio_set_level(PIN_LED_GREEN, 0);
}

void led_red_on(void) {
    gpio_set_level(PIN_RGB_RED, 0);   // 100% full power Red RGB
    gpio_set_level(PIN_LED_RED, 1);   // Discrete Red LED
}

void led_red_off(void) {
    gpio_set_level(PIN_RGB_RED, 1);
    gpio_set_level(PIN_LED_RED, 0);
}

void leds_off(void) {
    // RGB LED off (Common Anode -> drive HIGH)
    gpio_set_level(PIN_RGB_RED, 1);
    gpio_set_level(PIN_RGB_GREEN, 1);
    gpio_set_level(PIN_RGB_BLUE, 1);
    // Discrete LEDs off
    gpio_set_level(PIN_LED_GREEN, 0);
    gpio_set_level(PIN_LED_RED, 0);
}

// ── Buzzer Tone Generator ─────────────────────────────────────────
static void tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) {
        ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
        ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }
    ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, freq_hz);
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 512); // 50% duty cycle for 10-bit
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
}

void buzzer_play_success(void) {
    tone(1047, 80);  // C6
    vTaskDelay(pdMS_TO_TICKS(20));
    tone(1318, 80);  // E6
    vTaskDelay(pdMS_TO_TICKS(20));
    tone(1568, 150); // G6
}

void buzzer_play_error(void) {
    tone(300, 150);
    vTaskDelay(pdMS_TO_TICKS(50));
    tone(300, 250);
}

// ── OLED Screen Wrappers ──────────────────────────────────────────
void oled_show_ready(void) {
    ssd1306_show_ready(ledger_get_count());
}

void oled_show_listening(void) {
    ssd1306_show_listening();
}

void oled_show_receiving(int current_symbol, int total_symbols) {
    ssd1306_show_progress(current_symbol, total_symbols);
}

void oled_show_success(uint32_t rupees, uint32_t paise, uint16_t nonce) {
    ssd1306_show_success(rupees, paise, nonce);
}

void oled_show_error(const char *reason) {
    ssd1306_show_error("PAYMENT ERROR", reason);
}

void oled_show_replay_error(uint16_t nonce) {
    ssd1306_show_replay_error(nonce);
}
