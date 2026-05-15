// ----------------------------------------------------------------------------
//  display_config.h — display & touch hardware profile.
//
//  The default values here target the Freenove FNK0104B ESP32-S3 board:
//    https://amzn.to/4dJlGMW
//
//  If you bought a different "CYD" (cheap yellow display) board with a
//  different panel driver, different pinout, or different touch controller,
//  edit the values below. Common alternative profiles are included as
//  commented-out blocks at the bottom. Uncomment exactly one profile
//  (delete the Freenove block first) and you should be good to recompile.
//
//  After changing anything here:
//      cd firmware
//      pio run -e reevo -t upload
// ----------------------------------------------------------------------------
#pragma once

// ===== Panel driver selection ================================================
// Uncomment exactly one to match the chip on the back of your TFT.
#define DISPLAY_PANEL_ILI9341
// #define DISPLAY_PANEL_ST7789
// #define DISPLAY_PANEL_ILI9488
// #define DISPLAY_PANEL_GC9A01    // round 1.28" CYDs

// ===== Touch controller selection ============================================
// Uncomment exactly one. FT5x06 driver handles the FT6x36 family (FT6236, etc).
#define DISPLAY_TOUCH_FT5X06
// #define DISPLAY_TOUCH_XPT2046   // resistive boards
// #define DISPLAY_TOUCH_GT911

// ===== Panel pinout (SPI) ====================================================
constexpr int DCFG_LCD_MOSI = 11;
constexpr int DCFG_LCD_MISO = 13;
constexpr int DCFG_LCD_SCLK = 12;
constexpr int DCFG_LCD_CS   = 10;
constexpr int DCFG_LCD_DC   = 46;
constexpr int DCFG_LCD_RST  = -1;   // -1 if tied to the board's system reset
constexpr int DCFG_LCD_BL   = 45;   // backlight
constexpr int DCFG_SPI_HZ   = 40000000;
constexpr bool DCFG_BL_ACTIVE_HIGH = true;

// ===== Panel orientation =====================================================
// The bike screen runs in landscape with USB-C on the left and the UART
// header on top. Native panel is portrait 240x320 → rotation 1 swaps it to
// 320x240 landscape. If your panel reads upside-down or mirrored, try other
// rotation values (0..3).
constexpr int DCFG_PANEL_NATIVE_W  = 240;
constexpr int DCFG_PANEL_NATIVE_H  = 320;
constexpr int DCFG_DISPLAY_W       = 320;        // after rotation
constexpr int DCFG_DISPLAY_H       = 240;        // after rotation
constexpr int DCFG_ROTATION        = 1;
constexpr bool DCFG_INVERT_COLORS  = true;       // most ILI9341 IPS modules
constexpr bool DCFG_BGR_ORDER      = true;       // false = RGB
constexpr int DCFG_X_OFFSET        = 0;
constexpr int DCFG_Y_OFFSET        = 0;

// ===== Touch =================================================================
constexpr int DCFG_TOUCH_SDA = 16;
constexpr int DCFG_TOUCH_SCL = 15;
constexpr int DCFG_TOUCH_INT = 17;
constexpr int DCFG_TOUCH_RST = 18;
constexpr int DCFG_TOUCH_I2C_ADDR = 0x38;        // FT6336U default
constexpr int DCFG_TOUCH_I2C_HZ   = 400000;

// ===== UART header (4-pin: 5V, GND, TX, RX) ==================================
// Used for the Diagnostics page's raw byte tap. Also see README on powering
// the dashboard from the bike's 5V UART line.
constexpr int DCFG_UART_TX = 43;
constexpr int DCFG_UART_RX = 44;

// ===========================================================================
// === Alternative profiles (uncomment one to use; delete the default first) ==
// ===========================================================================
//
// ----- Sunton CYD ESP32-2432S028R (ILI9341, XPT2046 resistive touch) -----
// constexpr int DCFG_LCD_MOSI = 13;
// constexpr int DCFG_LCD_MISO = 12;
// constexpr int DCFG_LCD_SCLK = 14;
// constexpr int DCFG_LCD_CS   = 15;
// constexpr int DCFG_LCD_DC   =  2;
// constexpr int DCFG_LCD_RST  = -1;
// constexpr int DCFG_LCD_BL   = 21;
// constexpr int DCFG_TOUCH_SDA = 33;   // XPT2046 actually uses SPI; this
// constexpr int DCFG_TOUCH_SCL = 25;   // CYD model is resistive — XPT2046
// constexpr int DCFG_TOUCH_INT = 36;   // requires extra integration work,
// constexpr int DCFG_TOUCH_RST = 32;   // not currently supported by this
//                                       // firmware as-is.
//
// ----- Sunton CYD ESP32-2432S028C (capacitive variant, GT911 touch) -----
// (Same display pins as the R variant. GT911 not currently supported here.)
//
// ----- Generic ST7789 240x320 CYD -----
// // Define DISPLAY_PANEL_ST7789 above instead of ILI9341.
// // Pins vary by exact board — consult its schematic.
// // Note: most ST7789 boards use rgb_order = false (RGB, not BGR).
