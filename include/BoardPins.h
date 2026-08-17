#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.75C official schematic/example mapping.
namespace BoardPins {
constexpr int LCD_SDIO0 = 4;
constexpr int LCD_SDIO1 = 5;
constexpr int LCD_SDIO2 = 6;
constexpr int LCD_SDIO3 = 7;
constexpr int LCD_SCLK = 38;
constexpr int LCD_RESET = 2;
constexpr int LCD_CS = 12;
constexpr int LCD_WIDTH = 466;
constexpr int LCD_HEIGHT = 466;

constexpr int I2C_SDA = 15;
constexpr int I2C_SCL = 14;
constexpr int TOUCH_INT = 11;
constexpr int TOUCH_RESET = 2;  // Shared with LCD_RESET on this board.

}  // namespace BoardPins
