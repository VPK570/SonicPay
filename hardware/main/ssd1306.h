#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "u8g2.h"

#define PIN_OLED_SCK   18
#define PIN_OLED_MOSI  23
#define PIN_OLED_RES    4
#define PIN_OLED_DC     2
#define PIN_OLED_CS     5

bool ssd1306_init(void);

// Polished UI screen functions using u8g2 graphics
void ssd1306_draw_header(const char *title);
void ssd1306_show_ready(size_t ledger_count);
void ssd1306_show_listening(void);
void ssd1306_show_progress(int current, int total);
void ssd1306_show_success(uint32_t rupees, uint32_t paise, uint16_t nonce);
void ssd1306_show_replay_error(uint16_t nonce);
void ssd1306_show_error(const char *title, const char *reason);

#endif // SSD1306_H
