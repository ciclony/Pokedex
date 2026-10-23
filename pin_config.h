#pragma once

// ESP32-2432S028 ("Cheap Yellow Display") pins.
// Source: github.com/witnessmenow/ESP32-Cheap-Yellow-Display (PINS.md)

// The two-USB version (micro-USB + USB-C) has an ST7789; the single micro-USB
// one an ILI9341. Comment this out for the ILI9341.
#define CYD_ST7789

// Display (HSPI)
#define LCD_SCLK 14
#define LCD_MOSI 13
#define LCD_MISO 12
#define LCD_CS 15
#define LCD_DC 2
#define LCD_BL 21
#define LCD_W 320
#define LCD_H 240

// XPT2046 touch (own bus, bit-banged)
#define TP_CLK 25
#define TP_MOSI 32
#define TP_MISO 39
#define TP_CS 33
#define TP_INT 36
// raw 12-bit readings at the panel edges
#define TP_X_MIN 200
#define TP_X_MAX 3700
#define TP_Y_MIN 240
#define TP_Y_MAX 3800

// microSD (VSPI)
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5
