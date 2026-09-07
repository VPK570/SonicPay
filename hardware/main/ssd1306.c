#include "ssd1306.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define TAG "OLED_UI"
#define SPI_HOST_ID SPI2_HOST

static spi_device_handle_t spi_handle;
static u8g2_t u8g2;

static uint8_t u8x8_byte_esp32_hw_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
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

static uint8_t u8x8_esp32_gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
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

bool ssd1306_init(void) {
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
        .clock_speed_hz = 1000000, // 1 MHz
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI_HOST_ID, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return false;
    }

    u8g2_Setup_ssd1306_128x64_noname_f(&u8g2, U8G2_R0, u8x8_byte_esp32_hw_spi, u8x8_esp32_gpio_and_delay);

    gpio_set_level(PIN_OLED_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_OLED_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearDisplay(&u8g2);

    ESP_LOGI(TAG, "SSD1306 OLED UI initialized");
    return true;
}

// ── UI Drawing Helpers ────────────────────────────────────────────
void ssd1306_draw_header(const char *title) {
    u8g2_DrawBox(&u8g2, 0, 0, 128, 14);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    int width = u8g2_GetStrWidth(&u8g2, title);
    u8g2_DrawStr(&u8g2, (128 - width) / 2, 11, title);
    u8g2_SetDrawColor(&u8g2, 1);
}

void ssd1306_show_ready(size_t ledger_count) {
    u8g2_ClearBuffer(&u8g2);
    ssd1306_draw_header("SonicPay Terminal");

    u8g2_SetFont(&u8g2, u8g2_font_7x14B_tf);
    u8g2_DrawStr(&u8g2, 38, 34, "READY");

    char sub[32];
    snprintf(sub, sizeof(sub), "Ledger: %zu txns", ledger_count);
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 26, 56, sub);

    u8g2_SendBuffer(&u8g2);
}

void ssd1306_show_listening(void) {
    u8g2_ClearBuffer(&u8g2);
    ssd1306_draw_header("SonicPay Terminal");

    u8g2_SetFont(&u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(&u8g2, 16, 32, "[Preamble Lock]");
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 20, 52, "Receiving audio...");

    u8g2_SendBuffer(&u8g2);
}

void ssd1306_show_progress(int current, int total) {
    u8g2_ClearBuffer(&u8g2);
    ssd1306_draw_header("Receiving Payment");

    char pctStr[16];
    int pct = (current * 100) / (total > 0 ? total : 1);
    snprintf(pctStr, sizeof(pctStr), "%d%%", pct);

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 4, 30, "Acoustic modem");
    u8g2_DrawStr(&u8g2, 100, 30, pctStr);

    // Progress bar frame & fill box
    u8g2_DrawFrame(&u8g2, 4, 36, 120, 14);
    int barW = (116 * current) / (total > 0 ? total : 1);
    if (barW > 116) barW = 116;
    if (barW > 0) u8g2_DrawBox(&u8g2, 6, 38, barW, 10);

    u8g2_SendBuffer(&u8g2);
}

void ssd1306_show_success(uint32_t rupees, uint32_t paise, uint16_t nonce) {
    u8g2_ClearBuffer(&u8g2);
    ssd1306_draw_header("PAYMENT APPROVED");

    char amtStr[32], nonceStr[32];
    snprintf(amtStr, sizeof(amtStr), "RS %lu.%02lu", (unsigned long)rupees, (unsigned long)paise);
    snprintf(nonceStr, sizeof(nonceStr), "Nonce: #%u", nonce);

    u8g2_SetFont(&u8g2, u8g2_font_7x14B_tf);
    int w = u8g2_GetStrWidth(&u8g2, amtStr);
    u8g2_DrawStr(&u8g2, (128 - w) / 2, 36, amtStr);

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    w = u8g2_GetStrWidth(&u8g2, nonceStr);
    u8g2_DrawStr(&u8g2, (128 - w) / 2, 54, nonceStr);

    u8g2_SendBuffer(&u8g2);
}

void ssd1306_show_replay_error(uint16_t nonce) {
    u8g2_ClearBuffer(&u8g2);
    ssd1306_draw_header("REPLAY ATTACK!");

    u8g2_SetFont(&u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(&u8g2, 10, 32, "Duplicate Nonce");

    char nStr[32];
    snprintf(nStr, sizeof(nStr), "#%u in ledger!", nonce);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 24, 52, nStr);

    u8g2_SendBuffer(&u8g2);
}

void ssd1306_show_error(const char *title, const char *reason) {
    u8g2_ClearBuffer(&u8g2);
    ssd1306_draw_header(title ? title : "PAYMENT ERROR");

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    if (reason) {
        int w = u8g2_GetStrWidth(&u8g2, reason);
        u8g2_DrawStr(&u8g2, (128 - w) / 2, 36, reason);
    }
    u8g2_DrawStr(&u8g2, 26, 54, "Transaction cancelled");

    u8g2_SendBuffer(&u8g2);
}
