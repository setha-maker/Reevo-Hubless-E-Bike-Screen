// ----------------------------------------------------------------------------
//  pins.h — pin assignments for the display, touch, and UART header.
//
//  This file is a thin wrapper around displays/display_config.h, which is the
//  user-facing file you edit when adapting to a different CYD board. Don't
//  hand-edit values here — change them in display_config.h.
// ----------------------------------------------------------------------------
#pragma once

#include "displays/display_config.h"

// ----- LCD (SPI) -----
constexpr int PIN_LCD_MOSI = DCFG_LCD_MOSI;
constexpr int PIN_LCD_MISO = DCFG_LCD_MISO;
constexpr int PIN_LCD_SCLK = DCFG_LCD_SCLK;
constexpr int PIN_LCD_CS   = DCFG_LCD_CS;
constexpr int PIN_LCD_DC   = DCFG_LCD_DC;
constexpr int PIN_LCD_RST  = DCFG_LCD_RST;
constexpr int PIN_LCD_BL   = DCFG_LCD_BL;

// ----- Touch -----
constexpr int PIN_TOUCH_SDA = DCFG_TOUCH_SDA;
constexpr int PIN_TOUCH_SCL = DCFG_TOUCH_SCL;
constexpr int PIN_TOUCH_INT = DCFG_TOUCH_INT;
constexpr int PIN_TOUCH_RST = DCFG_TOUCH_RST;

// ----- External UART connector (4-pin: RXD, TXD, GND, 5V) -----
constexpr int PIN_UART_TX = DCFG_UART_TX;
constexpr int PIN_UART_RX = DCFG_UART_RX;
