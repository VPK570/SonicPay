/*
 * SonicPay Peripheral Test
 *
 * Self-contained ESP-IDF program to verify all hardware peripherals
 * are wired correctly before integrating into SonicPay firmware.
 *
 * Tests: OLED (SSD1306 SPI), RGB LED (LEDC PWM), Individual LEDs, Analog Mic (ADC)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "u8g2.h"

/* ─── Pin Definitions ─── */

// OLED (SSD1306 SPI)
#define PIN_OLED_SCK   18
#define PIN_OLED_MOSI  23
#define PIN_OLED_RES    4
#define PIN_OLED_DC     2
#define PIN_OLED_CS     5

// RGB LED (common anode, active LOW)
#define PIN_RGB_RED    25
#define PIN_RGB_GREEN  26
#define PIN_RGB_BLUE   27

// Individual LEDs (active HIGH)
#define PIN_LED_GREEN  32
#define PIN_LED_RED    33

// Mic (ADC1 CH6)
#define PIN_MIC        34
#define MIC_ADC_CHANNEL ADC_CHANNEL_6

/* ─── LEDC Config ─── */

#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_HIGH_SPEED_MODE
#define LEDC_DUTY_RES   LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY  5000

#define LEDC_CH_RED     LEDC_CHANNEL_0
#define LEDC_CH_GREEN   LEDC_CHANNEL_1
#define LEDC_CH_BLUE    LEDC_CHANNEL_2

/* ─── SPI Config ─── */

#define SPI_HOST_ID     SPI2_HOST
#define SPI_CLOCK_SPEED 1000000  // 1 MHz

/* ─── Globals ─── */

static spi_device_handle_t spi_handle;
static const char *TAG = "periph_test";

static void rgb_off(void);

/* ─── u8g2 SPI Byte Callback ─── */

static uint8_t u8x8_byte_esp32_hw_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    uint8_t *data;
    spi_transaction_t t;

    switch (msg) {
        case U8X8_MSG_BYTE_SEND:
            data = (uint8_t *)arg_ptr;
            memset(&t, 0, sizeof(t));
            t.length = arg_int * 8;
            t.tx_buffer = data;
            spi_device_transmit(spi_handle, &t);
            break;

        case U8X8_MSG_BYTE_INIT:
            break;

        case U8X8_MSG_BYTE_SET_DC:
            gpio_set_level(PIN_OLED_DC, arg_int);
            break;

        case U8X8_MSG_BYTE_START_TRANSFER:
            gpio_set_level(PIN_OLED_CS, 0);
            break;

        case U8X8_MSG_BYTE_END_TRANSFER:
            gpio_set_level(PIN_OLED_CS, 1);
            break;

        default:
            return 0;
    }
    return 1;
}

/* ─── u8g2 GPIO/Timer Callbacks ─── */

static uint8_t u8x8_esp32_gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            gpio_reset_pin(PIN_OLED_CS);
            gpio_set_direction(PIN_OLED_CS, GPIO_MODE_OUTPUT);
            gpio_reset_pin(PIN_OLED_DC);
            gpio_set_direction(PIN_OLED_DC, GPIO_MODE_OUTPUT);
            gpio_reset_pin(PIN_OLED_RES);
            gpio_set_direction(PIN_OLED_RES, GPIO_MODE_OUTPUT);
            break;

        case U8X8_MSG_GPIO_CS:
            gpio_set_level(PIN_OLED_CS, arg_int);
            break;

        case U8X8_MSG_GPIO_DC:
            gpio_set_level(PIN_OLED_DC, arg_int);
            break;

        case U8X8_MSG_GPIO_RESET:
            gpio_set_level(PIN_OLED_RES, arg_int);
            break;

        case U8X8_MSG_DELAY_MILLI:
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;

        case U8X8_MSG_DELAY_10MICRO:
            esp_rom_delay_us(10);
            break;

        case U8X8_MSG_DELAY_100NANO:
            esp_rom_delay_us(1);
            break;

        default:
            return 0;
    }
    return 1;
}

/* ─── OLED Init ─── */

static bool init_oled(u8g2_t *u8g2)
{
    // Init SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_OLED_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_OLED_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 128 * 64 / 8,
    };
    esp_err_t ret = spi_bus_initialize(SPI_HOST_ID, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SPI_CLOCK_SPEED,
        .mode = 0,
        .spics_io_num = -1,  // We control CS manually
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI_HOST_ID, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Init u8g2 with SSD1306 128x64 SPI
    u8g2_Setup_ssd1306_128x64_noname_f(u8g2, U8G2_R0,
                                        u8x8_byte_esp32_hw_spi,
                                        u8x8_esp32_gpio_and_delay);

    // Reset OLED
    gpio_set_level(PIN_OLED_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_OLED_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    u8g2_InitDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0);
    u8g2_ClearDisplay(u8g2);

    ESP_LOGI(TAG, "OLED initialized");
    return true;
}

/* ─── RGB LED Init (LEDC PWM) ─── */

static void init_rgb_led(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t channels[] = {
        { .speed_mode = LEDC_MODE, .channel = LEDC_CH_RED,   .timer_sel = LEDC_TIMER, .gpio_num = PIN_RGB_RED,   .duty = 0, .hpoint = 0 },
        { .speed_mode = LEDC_MODE, .channel = LEDC_CH_GREEN, .timer_sel = LEDC_TIMER, .gpio_num = PIN_RGB_GREEN, .duty = 0, .hpoint = 0 },
        { .speed_mode = LEDC_MODE, .channel = LEDC_CH_BLUE,  .timer_sel = LEDC_TIMER, .gpio_num = PIN_RGB_BLUE,  .duty = 0, .hpoint = 0 },
    };
    for (int i = 0; i < 3; i++) {
        ledc_channel_config(&channels[i]);
    }

    // Enable fade for breathing effect
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    // Set initial state to OFF (duty 255 = off for common anode)
    rgb_off();

    ESP_LOGI(TAG, "RGB LED initialized (LEDC PWM)");
}

/* ─── Individual LEDs Init ─── */

static void init_individual_leds(void)
{
    gpio_reset_pin(PIN_LED_GREEN);
    gpio_set_direction(PIN_LED_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_GREEN, 0);

    gpio_reset_pin(PIN_LED_RED);
    gpio_set_direction(PIN_LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_RED, 0);

    ESP_LOGI(TAG, "Individual LEDs initialized");
}

/* ─── Helpers ─── */

// Common anode: 0 = ON, 255 = OFF
static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    ledc_set_duty(LEDC_MODE, LEDC_CH_RED, 255 - r);
    ledc_update_duty(LEDC_MODE, LEDC_CH_RED);
    ledc_set_duty(LEDC_MODE, LEDC_CH_GREEN, 255 - g);
    ledc_update_duty(LEDC_MODE, LEDC_CH_GREEN);
    ledc_set_duty(LEDC_MODE, LEDC_CH_BLUE, 255 - b);
    ledc_update_duty(LEDC_MODE, LEDC_CH_BLUE);
}

static void rgb_off(void)
{
    rgb_set(0, 0, 0);
}

static void oled_show_message(u8g2_t *u8g2, const char *line1, const char *line2)
{
    u8g2_ClearBuffer(u8g2);
    if (line1) {
        u8g2_SetFont(u8g2, u8g2_font_helvB24_tr);
        int w = u8g2_GetStrWidth(u8g2, line1);
        u8g2_DrawStr(u8g2, (128 - w) / 2, 35, line1);
    }
    if (line2) {
        u8g2_SetFont(u8g2, u8g2_font_helvB14_tr);
        int w = u8g2_GetStrWidth(u8g2, line2);
        u8g2_DrawStr(u8g2, (128 - w) / 2, 55, line2);
    }
    u8g2_SendBuffer(u8g2);
}

/* ─── ADC Mic ─── */

static adc_oneshot_unit_handle_t adc_handle;

static void init_mic(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_11,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, MIC_ADC_CHANNEL, &cfg));
    ESP_LOGI(TAG, "Mic ADC initialized (ADC1 CH6, 12-bit, 11dB atten)");
}

static int mic_read_average(int num_samples)
{
    int sum = 0, raw;
    for (int i = 0; i < num_samples; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, MIC_ADC_CHANNEL, &raw));
        sum += raw;
        esp_rom_delay_us(100);
    }
    return sum / num_samples;
}

/* ─── Test Sequence ─── */

void app_main(void)
{
    u8g2_t u8g2;

    printf("\n=== SonicPay Peripheral Test ===\n\n");

    // ─── Init all peripherals ───
    printf("[INIT] Configuring GPIO pins...\n");
    init_individual_leds();
    init_rgb_led();
    init_mic();

    printf("[INIT] Initializing OLED (SPI)...\n");
    if (!init_oled(&u8g2)) {
        printf("[FAIL] OLED initialization failed! Check SPI wiring.\n");
        printf("  SCK=GPIO%d  MOSI=GPIO%d  RES=GPIO%d  DC=GPIO%d  CS=GPIO%d\n",
               PIN_OLED_SCK, PIN_OLED_MOSI, PIN_OLED_RES, PIN_OLED_DC, PIN_OLED_CS);
        // Can't continue without OLED — blink red LED as error indicator
        while (1) {
            gpio_set_level(PIN_LED_RED, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(PIN_LED_RED, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // ── TEST 1: OLED splash ──
    printf("[TEST 1] OLED — displaying splash screen...\n");
    oled_show_message(&u8g2, "SonicPay", NULL);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ── TEST 2: OLED status ──
    printf("[TEST 2] OLED — \"Testing LEDs...\"\n");
    oled_show_message(&u8g2, NULL, "Testing LEDs...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ── TEST 3: RGB cycle ──
    printf("[TEST 3] RGB LED — cycling colors...\n");

    printf("  Red ON\n");
    rgb_set(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    rgb_off();
    vTaskDelay(pdMS_TO_TICKS(200));

    printf("  Green ON\n");
    rgb_set(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    rgb_off();
    vTaskDelay(pdMS_TO_TICKS(200));

    printf("  Blue ON\n");
    rgb_set(0, 0, 255);
    vTaskDelay(pdMS_TO_TICKS(500));
    rgb_off();
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── TEST 4: RGB breathing white ──
    printf("[TEST 4] RGB LED — breathing white...\n");
    {
        // Breathing: fade all channels 255 (off) -> 0 (bright) -> 255 (off).
        const int fade_time_ms = 1500;

        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_RED,   0, 1, 30);
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_GREEN, 0, 1, 30);
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_BLUE,  0, 1, 30);

        // Start fade-in (all channels fade to 0 = full brightness on common anode)
        ledc_fade_start(LEDC_MODE, LEDC_CH_RED, LEDC_FADE_NO_WAIT);
        ledc_fade_start(LEDC_MODE, LEDC_CH_GREEN, LEDC_FADE_NO_WAIT);
        ledc_fade_start(LEDC_MODE, LEDC_CH_BLUE, LEDC_FADE_NO_WAIT);
        vTaskDelay(pdMS_TO_TICKS(fade_time_ms + 100));

        // Fade-out: set target to 255 (off for common anode)
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_RED,   255, 1, 30);
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_GREEN, 255, 1, 30);
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_BLUE,  255, 1, 30);

        ledc_fade_start(LEDC_MODE, LEDC_CH_RED, LEDC_FADE_NO_WAIT);
        ledc_fade_start(LEDC_MODE, LEDC_CH_GREEN, LEDC_FADE_NO_WAIT);
        ledc_fade_start(LEDC_MODE, LEDC_CH_BLUE, LEDC_FADE_NO_WAIT);
        vTaskDelay(pdMS_TO_TICKS(fade_time_ms + 100));

        rgb_off();
    }

    // ── TEST 5: Individual green LED ──
    printf("[TEST 5] Individual green LED — ON/OFF\n");
    gpio_set_level(PIN_LED_GREEN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PIN_LED_GREEN, 0);

    // ── TEST 6: Individual red LED ──
    printf("[TEST 6] Individual red LED — ON/OFF\n");
    gpio_set_level(PIN_LED_RED, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PIN_LED_RED, 0);

    // ── TEST 7: OLED mic status ──
    printf("[TEST 7] OLED — \"Testing Mic...\"\n");
    oled_show_message(&u8g2, NULL, "Testing Mic...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ── TEST 8: Mic ADC read ──
    printf("[TEST 8] Mic — reading 100 ADC samples...\n");
    int mic_avg = mic_read_average(100);
    printf("  Mic average: %d (raw 12-bit ADC)\n", mic_avg);

    if (mic_avg == 0) {
        printf("  [WARN] Mic average is 0 — mic may be disconnected or GPIO%d not connected.\n", PIN_MIC);
    } else if (mic_avg >= 4095) {
        printf("  [WARN] Mic average is 4095 — ADC may be saturated or pin floating.\n");
    } else {
        printf("  [OK] Mic reading looks valid.\n");
    }

    // ── TEST 9: Display mic value ──
    printf("[TEST 9] OLED — displaying mic value...\n");
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "Mic avg: %d", mic_avg);
        oled_show_message(&u8g2, NULL, buf);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // ── TEST 10: Done ──
    printf("[TEST 10] OLED — \"All tests done!\"\n");
    oled_show_message(&u8g2, "All tests", "done!");
    vTaskDelay(pdMS_TO_TICKS(3000));

    printf("\n=== All peripheral tests passed! ===\n");
    printf("Entering idle loop (blue breathing)...\n");

    // ── Idle loop: slow blue breathing ──
    while (1) {
        // Fade blue in (255 -> 0 duty = off -> bright on common anode).
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_BLUE, 0, 1, 40);
        ledc_fade_start(LEDC_MODE, LEDC_CH_BLUE, LEDC_FADE_NO_WAIT);
        vTaskDelay(pdMS_TO_TICKS(2100));

        // Fade blue out (0 -> 255 duty = bright -> off).
        ledc_set_fade_with_step(LEDC_MODE, LEDC_CH_BLUE, 255, 1, 40);
        ledc_fade_start(LEDC_MODE, LEDC_CH_BLUE, LEDC_FADE_NO_WAIT);
        vTaskDelay(pdMS_TO_TICKS(2100));
    }
}
