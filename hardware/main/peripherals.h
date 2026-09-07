#ifndef PERIPHERALS_H
#define PERIPHERALS_H

#include <stdint.h>
#include <stdbool.h>

// Pin definitions matching SonicPay hardware spec
#define PIN_OLED_SCK   18
#define PIN_OLED_MOSI  23
#define PIN_OLED_RES    4
#define PIN_OLED_DC     2
#define PIN_OLED_CS     5

// RGB LED (Common Anode, Active LOW: 0 = FULL POWER ON, 1 = OFF)
#define PIN_RGB_RED    25
#define PIN_RGB_GREEN  26
#define PIN_RGB_BLUE   27

// Individual Discrete LEDs (Active HIGH: 1 = ON, 0 = OFF)
#define PIN_LED_GREEN  32
#define PIN_LED_RED    33

#define PIN_BUZZER     14

void peripherals_init(void);

// LED feedback
void led_green_on(void);
void led_green_off(void);
void led_red_on(void);
void led_red_off(void);
void leds_off(void);

// Buzzer feedback (LEDC PWM tones)
void buzzer_play_success(void);
void buzzer_play_error(void);

// OLED Display
void oled_show_ready(void);
void oled_show_listening(void);
void oled_show_receiving(int current_symbol, int total_symbols);
void oled_show_success(uint32_t rupees, uint32_t paise, uint16_t nonce);
void oled_show_error(const char *reason);
void oled_show_replay_error(uint16_t nonce);

#endif // PERIPHERALS_H
