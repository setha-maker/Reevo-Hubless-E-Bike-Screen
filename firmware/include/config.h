// ----------------------------------------------------------------------------
//  config.h — every tunable for the Reevo dashboard in one place.
//  If you find yourself changing a magic number in some .cpp file, move it
//  here instead. Pins are in pins.h; this file is for behaviour.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

// ----- Display -----
// Panel is natively 240(W) x 320(H) portrait. We run the dashboard in
// landscape with the board's USB-C connector on the LEFT and the UART
// header on the TOP — that's LovyanGFX rotation 3 (portrait rotated 90°
// counter-clockwise).
//
// LovyanGFX's FT5x06/FT6336U driver doesn't auto-transform touch coords
// when the display is rotated. We fix that ourselves: see touch_transform()
// in main.cpp.
#include "displays/display_config.h"

constexpr int   PANEL_NATIVE_WIDTH  = DCFG_PANEL_NATIVE_W;
constexpr int   PANEL_NATIVE_HEIGHT = DCFG_PANEL_NATIVE_H;
constexpr int   DISPLAY_ROTATION    = DCFG_ROTATION;
constexpr int   DISPLAY_WIDTH       = DCFG_DISPLAY_W;
constexpr int   DISPLAY_HEIGHT      = DCFG_DISPLAY_H;

// ----- BLE -----
constexpr const char* BIKE_NAME_PREFIX  = "REEVO";   // suffix is per-bike
constexpr uint32_t    BIKE_PAIR_PASSKEY = 111111;    // RN4870 default, only
                                                     // valid on never-paired
                                                     // bikes — see README
constexpr int         BLE_SCAN_DURATION_S = 8;
constexpr int         BLE_RECONNECT_BACKOFF_MIN_MS = 1500;
constexpr int         BLE_RECONNECT_BACKOFF_MAX_MS = 12000;

// ISSC Transparent UART (Reevo's BLE service)
#define REEVO_SVC_UUID    "49535343-fe7d-4ae5-8fa9-9fafd205e455"
#define REEVO_NOTIFY_UUID "49535343-1e4d-4bd9-ba61-23c647249616"
#define REEVO_WRITE_UUID  "49535343-8841-43f4-a8d4-ecbe34729bb3"
#define REEVO_FLOW_UUID   "49535343-4c8a-39b3-2f49-511cff073b7e"

// NVS keys for persistent state across reboots
#define NVS_NAMESPACE     "reevo"
#define NVS_KEY_PAIRED    "paired"
#define NVS_KEY_BIKE_ADDR "bike_addr"

// ----- UART tap (Diagnostics) -----
// Wired to the bike's BLE-module debug pads. CoolTerm on the same pads
// at this baud shows readable ASCII. If you see only '.....' garbage on
// the Diagnostics page, the bike is on a different baud — try 9600 next.
constexpr int UART_TAP_BAUD = 115200;

// ----- App behaviour -----
// Speed display = (wheel_pulse / MAX_WHEEL_PULSE) × top_speed.
// Picking a top_speed picks how fast the bar reads "pinned" — empirical
// max wheel_pulse observed in the field was ~30.
constexpr int   TOP_SPEED_MIN     = 1;
constexpr int   TOP_SPEED_MAX     = 20;
constexpr int   TOP_SPEED_DEFAULT = 15;
constexpr float MAX_WHEEL_PULSE  = 30.0f;

// Sleep / wake state machine.
// "Activity" = any BLE notification, any touch, or explicit wake. The Reevo
// does NOT disconnect BLE when it sleeps and we haven't decoded a sleep-
// indicator notification yet, so we lean on the bike's incidental traffic
// (battery, kickstand, etc.) to keep the screen alive. Default 60s gives
// even a quiet bike plenty of breathing room.
constexpr int   SLEEP_TIMEOUT_DEFAULT_S = 60;
constexpr int   SLEEP_TIMEOUT_MIN_S     = 20;
constexpr int   SLEEP_TIMEOUT_MAX_S     = 300;

// Brightness 1..10 mapped to backlight 25..255.
constexpr int   BRIGHTNESS_DEFAULT = 8;
constexpr int   BRIGHTNESS_MIN     = 1;
constexpr int   BRIGHTNESS_MAX     = 10;
inline int brightness_to_pwm(int b) { return b * 25 + 5; }

// ----- Colors (RGB565 helpers) -----
#define RGB(r,g,b) (((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) & 0xF8) >> 3)
namespace Color {
    constexpr uint16_t BG       = RGB( 15,  15,  20);
    constexpr uint16_t PANEL    = RGB( 28,  28,  36);
    constexpr uint16_t FG       = RGB(240, 240, 240);
    constexpr uint16_t DIM      = RGB(110, 110, 120);
    constexpr uint16_t MUTED    = RGB( 60,  60,  70);
    constexpr uint16_t ACCENT   = RGB( 60, 200, 255);
    constexpr uint16_t WARN     = RGB(255,  80,  80);
    constexpr uint16_t OK       = RGB( 60, 220, 100);
    constexpr uint16_t SIGNAL   = RGB(255, 165,   0);
    constexpr uint16_t REEVO_BLUE = RGB(43, 87, 220);   // matches the logo
    constexpr uint16_t SEG_YELLOW = RGB(255, 224,  30);  // 7-seg / calculator yellow
}
#undef RGB
