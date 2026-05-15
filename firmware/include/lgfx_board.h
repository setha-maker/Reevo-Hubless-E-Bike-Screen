// ----------------------------------------------------------------------------
//  lgfx_board.h — LovyanGFX config for the Freenove FNK0104B board.
//
//  Display:  ILI9341 (240x320 IPS) over SPI, HSPI/SPI3_HOST.
//  Touch:    FT6336U (FocalTech FT6x36 family) over I²C, 0x38.
//  Backlight pin 45, active-HIGH.
//
//  Reference: Freenove's official TFT_eSPI_Setups/FNK0104B_2.8_240x320_ILI9341.h
//  https://github.com/Freenove/Freenove_ESP32_S3_Display
// ----------------------------------------------------------------------------
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pins.h"
#include "config.h"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
    lgfx::Touch_FT5x06  _touch;   // FT5x06 driver also handles FT6x36 family

public:
    LGFX() {
        {   // SPI bus — HSPI / SPI3_HOST per Freenove's USE_HSPI_PORT
            auto cfg = _bus.config();
            cfg.spi_host    = SPI3_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = DCFG_SPI_HZ;
            cfg.freq_read   = DCFG_SPI_HZ / 2;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCLK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = PIN_LCD_MISO;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {   // Panel — dimensions MUST be the native portrait size of the
            // physical display (240×320). LovyanGFX swaps width/height
            // itself when you call setRotation() with a 90/270° rotation.
            auto cfg = _panel.config();
            cfg.pin_cs           = PIN_LCD_CS;
            cfg.pin_rst          = PIN_LCD_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = PANEL_NATIVE_WIDTH;
            cfg.panel_height     = PANEL_NATIVE_HEIGHT;
            cfg.offset_x         = DCFG_X_OFFSET;
            cfg.offset_y         = DCFG_Y_OFFSET;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = DCFG_INVERT_COLORS;
            cfg.rgb_order        = !DCFG_BGR_ORDER;   // LovyanGFX: false = BGR
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }
        {   // Backlight on GPIO 45 (PWM)
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;     // active-HIGH on this board
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        {   // Capacitive touch — FT6336U @ 0x38 on I²C bus 0.
            // Bounds are panel-native, same as the panel config above.
            auto cfg = _touch.config();
            cfg.x_min      = 0;
            cfg.x_max      = PANEL_NATIVE_WIDTH  - 1;
            cfg.y_min      = 0;
            cfg.y_max      = PANEL_NATIVE_HEIGHT - 1;
            cfg.pin_int    = PIN_TOUCH_INT;
            cfg.pin_rst    = PIN_TOUCH_RST;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port   = 0;
            cfg.i2c_addr   = 0x38;       // FT6x36 family default
            cfg.pin_sda    = PIN_TOUCH_SDA;
            cfg.pin_scl    = PIN_TOUCH_SCL;
            cfg.freq       = 400000;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};
