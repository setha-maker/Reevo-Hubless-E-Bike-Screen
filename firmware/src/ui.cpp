// ----------------------------------------------------------------------------
//  ui.cpp — Reevo Dashboard UI.
//
//  Screen stack: MAIN / NUMPAD / SETTINGS / SETTINGS_BT / SETTINGS_DISPLAY /
//                SETTINGS_SPEED / SETTINGS_BIKE / SETTINGS_DIAG / SETTINGS_CMD.
//  All drawing is to a PSRAM-backed sprite, pushed in a single DMA blit.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <string.h>

#include "ui.h"
#include "state.h"
#include "ble.h"
#include "config.h"
#include "power.h"
#include "uart_tap.h"
#include "cmd_ap.h"
#include "pas_timeout.h"
#include "secrets.h"
#include "theme.h"
#include "assets.h"
#include "user_config.h"

namespace {

enum class Screen : uint8_t {
    MAIN, NUMPAD, LOCK_CONFIRM,
    SETTINGS, SETTINGS_BT, SETTINGS_DISPLAY, SETTINGS_SPEED,
    SETTINGS_BIKE, SETTINGS_DIAG, SETTINGS_CMD,
};
Screen g_screen = Screen::MAIN;

LGFX*       g_tft       = nullptr;
LGFX_Sprite g_buf;
bool        g_sprite_ok = false;

void set_screen(Screen s) {
    // Leaving the Command Prompt page tears down the AP — we don't want
    // a Wi-Fi access point to keep broadcasting once the user has moved on.
    if (g_screen == Screen::SETTINGS_CMD && s != Screen::SETTINGS_CMD) {
        if (cmd_ap::active()) cmd_ap::stop();
    }
    g_screen = s;
}

// ---------------------------------------------------------------------------
//  RGB565 linear interpolation for color cross-fade
// ---------------------------------------------------------------------------
uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = (int)(ar + (br - ar) * t);
    int g = (int)(ag + (bg - ag) * t);
    int bl= (int)(ab + (bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ---------------------------------------------------------------------------
//  Drawing primitives
// ---------------------------------------------------------------------------

void draw_battery_icon(LGFX_Sprite& s, int x, int y, int pct) {
    constexpr int w = 30, h = 14;
    s.drawRect(x, y, w, h, Color::FG);
    s.fillRect(x + w, y + 4, 2, h - 8, Color::FG);
    int fill_w = (pct * (w - 4)) / 100;
    uint16_t c = pct >= 50 ? Color::OK
               : pct >= 20 ? Color::SEG_YELLOW
                           : Color::WARN;
    if (fill_w > 0) s.fillRect(x + 2, y + 2, fill_w, h - 4, c);
}

void draw_gear_icon(LGFX_Sprite& s, int cx, int cy, int size,
                    uint16_t color, uint16_t /*bg*/) {
    // Minimalist gear: thin double-ring with six small dot-teeth around
    // the outside and a single dot at the center. Reads as a gear without
    // the chunky-blob feel of filled teeth.
    int r_out  = size / 2;
    int r_ring = r_out - 2;
    for (int i = 0; i < 6; i++) {
        float a = i * (M_PI / 3.0f);
        int tx = cx + (int)(r_out * cosf(a));
        int ty = cy + (int)(r_out * sinf(a));
        s.fillCircle(tx, ty, 2, color);
    }
    s.drawCircle(cx, cy, r_ring,     color);
    s.drawCircle(cx, cy, r_ring - 1, color);
    s.fillCircle(cx, cy, 2, color);
}

void draw_headlight_sun(LGFX_Sprite& s, int cx, int cy, int r, bool on) {
    // Eight evenly-spaced rays + solid disc.
    uint16_t c = on ? Color::SEG_YELLOW : Color::DIM;
    int ray_in  = r + 3;
    int ray_out = r + (on ? 9 : 5);
    for (int i = 0; i < 8; i++) {
        float a = i * (M_PI / 4.0f);
        int x1 = cx + (int)(ray_in  * cosf(a));
        int y1 = cy + (int)(ray_in  * sinf(a));
        int x2 = cx + (int)(ray_out * cosf(a));
        int y2 = cy + (int)(ray_out * sinf(a));
        s.drawLine(x1, y1, x2, y2, c);
    }
    s.fillCircle(cx, cy, r, c);
}

// Flag glyph: vertical pole + triangular pennant. Used for the trip-reset
// button, which now lives under the trip odometer in the top-left.
void draw_flag_icon(LGFX_Sprite& s, int cx, int cy, uint16_t color) {
    constexpr int H = 18;        // flag height
    int top = cy - H / 2;
    // Pole (2px wide for visibility)
    s.drawFastVLine(cx - 6,     top, H + 2, color);
    s.drawFastVLine(cx - 6 + 1, top, H + 2, color);
    // Pennant — triangular flag to the right of the pole
    s.fillTriangle(cx - 4, top,
                   cx + 8, top + 5,
                   cx - 4, top + 10, color);
}

// Badge-light glyph: a big bold "B" in the same blue as the APK
// headlight-off icon. Tri-state coloring lives at the call site.
void draw_badge_icon(LGFX_Sprite& s, int cx, int cy, uint16_t color) {
    s.setFont(&fonts::FreeSansBold24pt7b);
    s.setTextDatum(middle_center);
    s.setTextColor(color, Color::BG);
    s.drawString("B", cx, cy + 1);   // +1 nudges optical center
}

// ---------------------------------------------------------------------------
//  Vector 7-segment digits — clean rectangles at any size, no pixelation
//  from font scaling. Used for the speedometer.
// ---------------------------------------------------------------------------
//
//   __a__
//  |     |
//  f     b
//  |__g__|
//  |     |
//  e     c
//  |__d__|
//
// Segment bits: a=0x01, b=0x02, c=0x04, d=0x08, e=0x10, f=0x20, g=0x40.
constexpr uint8_t SEG_DIGITS[10] = {
    0x3F, // 0: a b c d e f
    0x06, // 1: b c
    0x5B, // 2: a b d e g
    0x4F, // 3: a b c d g
    0x66, // 4: b c f g
    0x6D, // 5: a c d f g
    0x7D, // 6: a c d e f g
    0x07, // 7: a b c
    0x7F, // 8: all
    0x6F, // 9: a b c d f g
};

// One hexagonal segment, oriented horizontally. (x, y) = top-left corner
// of the bounding box; `len` is total length (tip-to-tip), `t` is thickness.
//
//     /----------\
//    <            >
//     \----------/
void draw_hex_horizontal(LGFX_Sprite& s, int x, int y, int len, int t,
                         uint16_t color) {
    int t2 = t / 2;
    // body — middle rectangle
    s.fillRect(x + t2, y, len - t, t, color);
    // left tip
    s.fillTriangle(x,       y + t2,
                   x + t2,  y,
                   x + t2,  y + t - 1, color);
    // right tip
    s.fillTriangle(x + len - 1,        y + t2,
                   x + len - 1 - t2,   y,
                   x + len - 1 - t2,   y + t - 1, color);
}

// Hexagonal segment oriented vertically. (x, y) = top-left corner;
// `t` is thickness (width), `len` is total length (tip-to-tip).
void draw_hex_vertical(LGFX_Sprite& s, int x, int y, int t, int len,
                       uint16_t color) {
    int t2 = t / 2;
    s.fillRect(x, y + t2, t, len - t, color);
    s.fillTriangle(x,           y + t2,
                   x + t2,      y,
                   x + t - 1,   y + t2, color);
    s.fillTriangle(x,           y + len - 1 - t2,
                   x + t2,      y + len - 1,
                   x + t - 1,   y + len - 1 - t2, color);
}

void draw_7seg_digit(LGFX_Sprite& s, int x, int y, int w, int h,
                     int d, uint16_t color) {
    if (d < 0 || d > 9) return;
    uint8_t segs   = SEG_DIGITS[d];
    int     t      = h / 9;                // segment thickness
    int     y_mid  = (h - t) / 2;          // y of middle horizontal
    int     y_bot  = h - t;                // y of bottom horizontal
    int     hlen   = w - 2 * t;            // horizontal segment length (caps included)
    int     vlen   = (h - t) / 2 - t + 1;  // vertical segment length (caps included)

    // a — top horizontal
    if (segs & 0x01) draw_hex_horizontal(s, x + t,         y,            hlen, t, color);
    // g — middle horizontal
    if (segs & 0x40) draw_hex_horizontal(s, x + t,         y + y_mid,    hlen, t, color);
    // d — bottom horizontal
    if (segs & 0x08) draw_hex_horizontal(s, x + t,         y + y_bot,    hlen, t, color);
    // f — top-left vertical
    if (segs & 0x20) draw_hex_vertical  (s, x,             y + t,        t, vlen, color);
    // b — top-right vertical
    if (segs & 0x02) draw_hex_vertical  (s, x + w - t,     y + t,        t, vlen, color);
    // e — bottom-left vertical
    if (segs & 0x10) draw_hex_vertical  (s, x,             y + y_mid + t, t, vlen, color);
    // c — bottom-right vertical
    if (segs & 0x04) draw_hex_vertical  (s, x + w - t,     y + y_mid + t, t, vlen, color);
}

void draw_7seg_number(LGFX_Sprite& s, int cx, int cy,
                      int value, int digit_w, int digit_h, int gap,
                      uint16_t color) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value < 0 ? 0 : value);
    int n = (int)strlen(buf);
    int total_w = n * digit_w + (n - 1) * gap;
    int x0 = cx - total_w / 2;
    int y0 = cy - digit_h / 2;
    for (int i = 0; i < n; i++) {
        draw_7seg_digit(s, x0 + i * (digit_w + gap), y0,
                        digit_w, digit_h, buf[i] - '0', color);
    }
}

// Padlock glyph — sized to fill the 38×38 bitmap footprint.
void draw_lock_icon(LGFX_Sprite& s, int cx, int cy, uint16_t color) {
    constexpr int bw = 26, bh = 20;
    // Drop the body's top edge above cy so the combined shackle+body's
    // visual center sits right on cy. Without this, the bottom-weighted
    // shape made slot 3 look spaced further than the others.
    int by = cy - 4;
    // Shackle (drawn first so the body overlaps its lower half).
    s.drawCircle(cx, by - 4, 9, color);
    s.drawCircle(cx, by - 4, 8, color);
    s.drawCircle(cx, by - 4, 7, color);
    s.drawCircle(cx, by - 4, 6, color);
    // Body covers the bottom half of the shackle ring.
    s.fillRoundRect(cx - bw / 2, by, bw, bh, 3, color);
    // Keyhole — small dot in BG color.
    s.fillCircle(cx, by + bh / 2 - 1, 2, Color::BG);
}

// Transient bottom-of-screen toast. Drawn on top of whatever screen is
// rendering; auto-dismisses after `until_ms`.
char     g_toast_text[64] = "";
uint32_t g_toast_until_ms = 0;
void show_toast(const char* msg, uint32_t duration_ms = 2000) {
    strncpy(g_toast_text, msg, sizeof(g_toast_text) - 1);
    g_toast_text[sizeof(g_toast_text) - 1] = '\0';
    g_toast_until_ms = millis() + duration_ms;
}
void draw_toast(LGFX_Sprite& s) {
    if (g_toast_text[0] == '\0' || millis() > g_toast_until_ms) return;
    s.setFont(&fonts::FreeSansBold9pt7b);
    int tw = s.textWidth(g_toast_text);
    int pad = 14, h = 26;
    int w = tw + pad * 2;
    int x = (DISPLAY_WIDTH - w) / 2;
    // Sits just above the speedometer — there's a clean band between the
    // header (battery/blinkers, ends ~y=44) and the speed digit (top ~y=78).
    int y = 50;
    s.fillRoundRect(x, y, w, h, h / 2, Color::PANEL);
    s.setTextDatum(middle_center);
    s.setTextColor(Color::FG, Color::PANEL);
    s.drawString(g_toast_text, DISPLAY_WIDTH / 2, y + h / 2);
}

void draw_blinker_arrow(LGFX_Sprite& s, int cx, int cy, int w, int h,
                        bool pointing_right, uint16_t color) {
    int hw = w / 2, hh = h / 2;
    if (pointing_right) {
        s.fillTriangle(cx, cy - hh, cx + hw, cy, cx, cy + hh, color);
        s.fillRect(cx - hw, cy - hh/2 - 1, hw, hh + 2, color);
    } else {
        s.fillTriangle(cx, cy - hh, cx - hw, cy, cx, cy + hh, color);
        s.fillRect(cx, cy - hh/2 - 1, hw, hh + 2, color);
    }
}

// generic "button" — rounded rect with centered text
void draw_button(LGFX_Sprite& s, int x, int y, int w, int h,
                 const char* label, uint16_t bg, uint16_t fg, bool enabled = true) {
    if (!enabled) { bg = Color::MUTED; fg = Color::DIM; }
    s.fillRoundRect(x, y, w, h, 6, bg);
    s.setFont(&fonts::FreeSans12pt7b);
    s.setTextDatum(middle_center);
    s.setTextColor(fg, bg);
    s.drawString(label, x + w / 2, y + h / 2);
}

void draw_stepper(LGFX_Sprite& s, int x, int y, int w, int h,
                  const char* value, uint16_t bg, uint16_t fg) {
    // [-] value [+] in width w
    constexpr int btn_w = 36;
    s.fillRoundRect(x, y, btn_w, h, 6, Color::PANEL);
    s.fillRoundRect(x + w - btn_w, y, btn_w, h, 6, Color::PANEL);
    s.setFont(&fonts::FreeSansBold12pt7b);
    s.setTextDatum(middle_center);
    s.setTextColor(Color::FG, Color::PANEL);
    s.drawString("-", x + btn_w / 2, y + h / 2);
    s.drawString("+", x + w - btn_w / 2, y + h / 2);
    // value in middle
    s.fillRoundRect(x + btn_w + 4, y, w - 2 * btn_w - 8, h, 6, bg);
    s.setTextColor(fg, bg);
    s.drawString(value, x + w / 2, y + h / 2);
}

// ---------------------------------------------------------------------------
//  Assist tic-tac (with cross-fade)
// ---------------------------------------------------------------------------
struct AssistFade {
    uint8_t  prev = 0;
    uint8_t  curr = 0;
    uint32_t started_ms = 0;
} g_fade;

constexpr int FADE_MS = 220;

uint16_t color_for_level(int level) {
    switch (level) {
        case 1: return Color::OK;          // ECO green
        case 2: return Color::ACCENT;      // NORMAL blue
        case 3: return Color::SEG_YELLOW;  // SPORT yellow
        case 4: return Color::WARN;        // TURBO red
        default: return Color::MUTED;      // OFF grey
    }
}
const char* label_for_level(int level) {
    switch (level) {
        case 1: return "ECO";
        case 2: return "NORMAL";
        case 3: return "SPORT";
        case 4: return "TURBO";
        default: return "OFF";
    }
}
uint16_t text_color_for_level(int level) {
    return (level == 3) ? Color::BG : Color::FG;
}

void update_fade(uint8_t new_level) {
    if (new_level != g_fade.curr) {
        g_fade.prev = g_fade.curr;
        g_fade.curr = new_level;
        g_fade.started_ms = millis();
    }
}

// ---------------------------------------------------------------------------
//  MAIN dashboard
// ---------------------------------------------------------------------------
namespace MainScreen {

// Top-left: battery / mileage / flag (trip reset).
constexpr int BAT_X = 8,  BAT_Y = 10;             // 30×14 battery glyph
constexpr int TRIP_TEXT_X = 8, TRIP_TEXT_Y = 32;
constexpr int FLAG_CX = 20, FLAG_CY = 64;         // tap-to-reset trip
constexpr int FLAG_HIT_X = 0, FLAG_HIT_Y = 50, FLAG_HIT_W = 50, FLAG_HIT_H = 30;

// Top-center: turn-signal arrows.
constexpr int BLINKER_CY = 22, BLINKER_W = 22, BLINKER_H = 16;
constexpr int BLINKER_LX = 138, BLINKER_RX = 182;

// Right column of icon buttons, descending from the top:
//   0: gear (settings)
//   1: headlight (toggle)
//   2: trip-reset
//   3: lock (opens confirm screen)
constexpr int RIGHT_X       = 296;
constexpr int RIGHT_Y0      = 24;
constexpr int RIGHT_PITCH   = 46;       // more breathing room between buttons
constexpr int RIGHT_HIT_HW  = 24;
constexpr int RIGHT_HIT_HH  = 22;
inline int right_cy(int idx) { return RIGHT_Y0 + idx * RIGHT_PITCH; }

// Speed: 7-seg digit at 2× scale (~96 px tall). Centered between the PAS
// pill above (ends y=84) and the bottom status row (starts y=216).
constexpr int SPEED_CY = 154;

constexpr int ASSIST_BAR_W = 160;
constexpr int ASSIST_BAR_X = (DISPLAY_WIDTH - ASSIST_BAR_W) / 2;
constexpr int ASSIST_BAR_Y = 62;
constexpr int ASSIST_BAR_H = 22;
constexpr int ASSIST_HIT_X = ASSIST_BAR_X - 4;
constexpr int ASSIST_HIT_Y = ASSIST_BAR_Y - 4;
constexpr int ASSIST_HIT_W = ASSIST_BAR_W + 8;
constexpr int ASSIST_HIT_H = ASSIST_BAR_H + 8;

void draw(LGFX_Sprite& s) {
    // Custom theme — only main screen reads from here. Other screens keep
    // the default Color::BG so navigation stays predictable.
    const uint16_t bg          = theme::main_bg();
    const uint16_t speed_color = theme::main_speed();
    s.fillSprite(bg);

    // ---- Top-left: battery + trip mileage ----
    draw_battery_icon(s, BAT_X, BAT_Y, g_state.battery_pct);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d%%", (int)g_state.battery_pct);
    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextDatum(middle_left);
    s.setTextColor(Color::FG, bg);
    s.drawString(buf, BAT_X + 34, BAT_Y + 7);

    snprintf(buf, sizeof(buf), "%.1f mi", (double)g_state.trip_miles());
    s.setTextDatum(top_left);
    s.setTextColor(Color::DIM, bg);
    s.drawString(buf, TRIP_TEXT_X, TRIP_TEXT_Y);

    // Flag (trip reset) directly below the mileage.
    draw_flag_icon(s, FLAG_CX, FLAG_CY, Color::FG);

    // ---- Top-center: blinkers ----
    bool blink_on = (millis() / 500) & 1;
    uint16_t lc = (g_state.left_signal  && blink_on) ? Color::OK : Color::MUTED;
    uint16_t rc = (g_state.right_signal && blink_on) ? Color::OK : Color::MUTED;
    draw_blinker_arrow(s, BLINKER_LX, BLINKER_CY, BLINKER_W, BLINKER_H, false, lc);
    draw_blinker_arrow(s, BLINKER_RX, BLINKER_CY, BLINKER_W, BLINKER_H, true,  rc);

    // ---- Right column: gear / headlight / badge / lock ----
    auto blit = [&](const uint16_t* bmp, int slot) {
        s.pushImage(RIGHT_X - SETTINGS_BITMAP_W / 2,
                    right_cy(slot) - SETTINGS_BITMAP_H / 2,
                    SETTINGS_BITMAP_W, SETTINGS_BITMAP_H, bmp);
    };
    blit(settings_bitmap, 0);
    blit(g_state.headlight_on ? headlight_on_bitmap : headlight_off_bitmap, 1);
    {
        // Bold "B" in the same blue as the headlight-off icon — sampled
        // from front_light_off.png at (36, 142, 253).
        constexpr uint16_t BADGE_BLUE = ((36 >> 3) << 11)
                                      | ((142 >> 2) << 5)
                                      | (253 >> 3);
        draw_badge_icon(s, RIGHT_X, right_cy(2), BADGE_BLUE);
    }
    {
        bool lock_enabled = g_state.kickstand_down;
        draw_lock_icon(s, RIGHT_X, right_cy(3),
                       lock_enabled ? Color::FG : Color::DIM);
    }

    // ---- Big green 7-seg speed (no MPH label) ----
    int speed_i = (int)(g_state.displayed_speed() + 0.5f);
    if (speed_i < 0)   speed_i = 0;
    if (speed_i > 999) speed_i = 999;
    // Vector 7-seg renderer — clean rectangles, no pixel-doubling stairs.
    constexpr int DIGIT_W = 64;
    constexpr int DIGIT_H = 108;
    constexpr int DIGIT_GAP = 10;
    draw_7seg_number(s, DISPLAY_WIDTH / 2, SPEED_CY,
                     speed_i, DIGIT_W, DIGIT_H, DIGIT_GAP, speed_color);

    // ---- Tic-tac assist pill with cross-fade ----
    update_fade(g_state.assist_level);
    uint32_t elapsed = millis() - g_fade.started_ms;
    float t = elapsed >= (uint32_t)FADE_MS ? 1.0f : (float)elapsed / (float)FADE_MS;
    uint16_t bar_bg = lerp565(color_for_level(g_fade.prev),
                              color_for_level(g_fade.curr), t);
    s.fillRoundRect(ASSIST_BAR_X, ASSIST_BAR_Y,
                    ASSIST_BAR_W, ASSIST_BAR_H,
                    ASSIST_BAR_H / 2, bar_bg);
    s.setFont(&fonts::FreeSansBold9pt7b);
    s.setTextDatum(middle_center);
    s.setTextColor(text_color_for_level(g_fade.curr), bar_bg);
    s.drawString(label_for_level(g_fade.curr),
                 ASSIST_BAR_X + ASSIST_BAR_W / 2,
                 ASSIST_BAR_Y + ASSIST_BAR_H / 2);

    // ---- Bottom status line: "Catapult ●  Runaway ●" ----
    {
        constexpr int DOT_R = 5;
        const int  status_y = DISPLAY_HEIGHT - 12;
        s.setFont(&fonts::FreeSans9pt7b);
        s.setTextDatum(middle_left);

        int x = 8;
        s.setTextColor(Color::FG, bg);
        s.drawString("Catapult", x, status_y);
        int cat_dot_x = x + s.textWidth("Catapult") + 8;
        uint16_t cat_c = g_state.kickstand_down ? Color::DIM : Color::WARN;
        s.fillCircle(cat_dot_x, status_y, DOT_R, cat_c);

        int auto_dot_x = DISPLAY_WIDTH - 8;
        uint16_t auto_c = (g_state.assist_level > 0) ? Color::WARN : Color::DIM;
        s.fillCircle(auto_dot_x, status_y, DOT_R, auto_c);
        s.setTextDatum(middle_right);
        s.setTextColor(Color::FG, bg);
        s.drawString("Runaway", auto_dot_x - DOT_R - 6, status_y);

        // Small Reevo wordmark, centered between the two corner labels.
        s.setFont(&fonts::FreeSansBold9pt7b);
        s.setTextDatum(middle_center);
        s.setTextColor(Color::REEVO_BLUE, bg);
        s.drawString("Reevo", DISPLAY_WIDTH / 2, status_y);
    }

    if (g_state.ble != BikeState::BleStatus::Connected) {
        const char* tag = "BLE?";
        switch (g_state.ble) {
            case BikeState::BleStatus::Scanning:     tag = "scan";  break;
            case BikeState::BleStatus::Connecting:   tag = "conn";  break;
            case BikeState::BleStatus::Disconnected: tag = "down";  break;
            case BikeState::BleStatus::Idle:         tag = "idle";  break;
            default: break;
        }
        s.setFont(&fonts::Font0);
        s.setTextDatum(top_center);
        s.setTextColor(Color::WARN, bg);
        s.drawString(tag, DISPLAY_WIDTH / 2, 38);
    }
}

// Returns 0..3 if a right-column button was tapped, else -1.
int hit_right_col(int x, int y) {
    if (x < RIGHT_X - RIGHT_HIT_HW || x >= RIGHT_X + RIGHT_HIT_HW) return -1;
    for (int i = 0; i < 4; i++) {
        int cy = right_cy(i);
        if (y >= cy - RIGHT_HIT_HH && y < cy + RIGHT_HIT_HH) return i;
    }
    return -1;
}

void on_touch(int x, int y) {
    // Flag (trip reset) in the top-left band.
    if (x >= FLAG_HIT_X && x < FLAG_HIT_X + FLAG_HIT_W &&
        y >= FLAG_HIT_Y && y < FLAG_HIT_Y + FLAG_HIT_H) {
        g_state.trip_baseline_odo = g_state.odometer_counter;
        show_toast("Trip reset");
        return;
    }

    int col = hit_right_col(x, y);
    if (col == 0) { set_screen(Screen::SETTINGS); return; }
    if (col == 1) {
        ble_send_command(g_state.headlight_on ? "0:C-1-5@" : "0:C-1-4@");
        return;
    }
    if (col == 2) {
        // Toggle badge light. Unknown defaults to "turn on" first.
        if (g_state.badge_light == BikeState::BadgeLight::On) {
            ble_send_command("0:C-1-17@");
            g_state.badge_light = BikeState::BadgeLight::Off;
        } else {
            ble_send_command("0:C-1-16@");
            g_state.badge_light = BikeState::BadgeLight::On;
        }
        return;
    }
    if (col == 3) {
        if (g_state.kickstand_down) set_screen(Screen::LOCK_CONFIRM);
        else                        show_toast("Deploy Kickstand To Lock");
        return;
    }

    if (x >= ASSIST_HIT_X && x < ASSIST_HIT_X + ASSIST_HIT_W &&
        y >= ASSIST_HIT_Y && y < ASSIST_HIT_Y + ASSIST_HIT_H) {
        if (g_state.assist_level >= 4) ble_send_command("0:C-1-11@");
        else                            ble_send_command("0:C-1-10@");
        return;
    }
}

}  // namespace MainScreen

// ---------------------------------------------------------------------------
//  LOCK CONFIRM — takeover screen reached from either the main lock button
//  or the Bike Controls Engage Lock card.
// ---------------------------------------------------------------------------
namespace LockConfirm {
constexpr int BTN_W = 120, BTN_H = 44, BTN_Y = 170;
constexpr int BTN_GAP = 16;
constexpr int BTN_X0 = (DISPLAY_WIDTH - BTN_W * 2 - BTN_GAP) / 2;

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);

    s.setFont(&fonts::FreeSansBold18pt7b);
    s.setTextDatum(top_center);
    s.setTextColor(Color::FG, Color::BG);
    s.drawString("Lock Reevo?", DISPLAY_WIDTH / 2, 56);

    s.setFont(&fonts::FreeSans12pt7b);
    s.setTextDatum(top_center);
    s.setTextColor(Color::DIM, Color::BG);
    s.drawString("You'll need the secret code.", DISPLAY_WIDTH / 2, 104);

    draw_button(s, BTN_X0,                      BTN_Y, BTN_W, BTN_H,
                "Cancel", Color::PANEL, Color::FG);
    draw_button(s, BTN_X0 + BTN_W + BTN_GAP,    BTN_Y, BTN_W, BTN_H,
                "Lock",   Color::WARN,  Color::FG);
}

void on_touch(int x, int y) {
    if (y < BTN_Y || y >= BTN_Y + BTN_H) return;
    if (x >= BTN_X0 && x < BTN_X0 + BTN_W) {
        set_screen(Screen::MAIN);
    } else if (x >= BTN_X0 + BTN_W + BTN_GAP &&
               x <  BTN_X0 + BTN_W + BTN_GAP + BTN_W) {
        // Lock and immediately put the dashboard to sleep — the rider sees
        // the goodbye image, the screen blanks, and tapping again reveals
        // the numpad (ui_loop forces it whenever kickstand_locked is true).
        ble_send_command("0:C-1-2@");
        pas_timeout::force_to_zero();   // wake up with PAS off, no surprises
        set_screen(Screen::MAIN);
        power::sleep_now();
    }
}

}  // namespace LockConfirm

// ---------------------------------------------------------------------------
//  NUMPAD (unlock) screen
// ---------------------------------------------------------------------------
namespace NumpadScreen {

// Lock-screen state. There is no "back" out of here — the bike must
// confirm `kickstand_locked = false` before ui_loop will switch away.
// Correct PIN fires C-1-3 and we wait for the state echo; wrong PIN
// just silently resets per the user spec.
char     g_code[8]             = "";
uint32_t g_unlock_sent_ms      = 0;
constexpr uint32_t UNLOCK_TIMEOUT_MS = 4000;

// Press-feedback: index of last-tapped key in keys[] and when. While the
// timestamp is recent we render that key with an inverted color so the
// rider sees the tap register.
int      g_pressed_idx         = -1;
uint32_t g_pressed_ms          = 0;
constexpr uint32_t PRESS_FLASH_MS = 140;

void reset() {
    g_code[0] = '\0';
    g_unlock_sent_ms = 0;
}

void update() {
    if (g_unlock_sent_ms &&
        (millis() - g_unlock_sent_ms) > UNLOCK_TIMEOUT_MS) {
        g_unlock_sent_ms = 0;     // gave up waiting; allow retry
    }
}

constexpr int TITLE_Y   = 14;
constexpr int DOT_Y     = 60;
constexpr int DOT_R     = 9;
constexpr int DOT_GAP   = 30;
constexpr int GRID_X0   = 46;
constexpr int GRID_Y0   = 86;
constexpr int BTN_W     = 72;
constexpr int BTN_H     = 32;
constexpr int GAP       = 6;

struct Key { const char* label; int row; int col; };
const Key keys[] = {
    {"1",0,0},{"2",0,1},{"3",0,2},
    {"4",1,0},{"5",1,1},{"6",1,2},
    {"7",2,0},{"8",2,1},{"9",2,2},
    {"DEL",3,0},{"0",3,1},{"OK",3,2},
};

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);

    // Title — REEVO blue, big, friendly.
    s.setFont(&fonts::FreeSansBold18pt7b);
    s.setTextDatum(top_center);
    s.setTextColor(Color::REEVO_BLUE, Color::BG);
    s.drawString("Unlock Reevo", DISPLAY_WIDTH / 2, TITLE_Y);

    // 4 PIN dots — yellow to echo the speedometer accent.
    int n = (int)strlen(g_code);
    int total_span = DOT_GAP * 3;
    int x0 = (DISPLAY_WIDTH - total_span) / 2;
    bool pending = g_unlock_sent_ms != 0;
    for (int i = 0; i < 4; i++) {
        int cx = x0 + i * DOT_GAP;
        if (pending) {
            s.fillCircle(cx, DOT_Y, DOT_R, Color::OK);
        } else if (i < n) {
            s.fillCircle(cx, DOT_Y, DOT_R, Color::SEG_YELLOW);
        } else {
            s.drawCircle(cx, DOT_Y, DOT_R, Color::MUTED);
        }
    }

    // 3x4 grid.
    bool pressed_active = (g_pressed_idx >= 0) &&
                          (millis() - g_pressed_ms < PRESS_FLASH_MS);
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        const Key& k = keys[i];
        int x = GRID_X0 + k.col * (BTN_W + GAP);
        int y = GRID_Y0 + k.row * (BTN_H + GAP);
        uint16_t bg, fg;
        bool is_pressed = pressed_active && g_pressed_idx == i;
        if (is_pressed) {
            // Inverted flash so any key reads as "just tapped" regardless
            // of its normal color.
            bg = Color::FG;
            fg = Color::BG;
        } else if (strcmp(k.label, "OK") == 0)  { bg = Color::ACCENT; fg = Color::BG; }
        else if (strcmp(k.label, "DEL") == 0)   { bg = Color::WARN;   fg = Color::FG; }
        else                                    { bg = Color::PANEL;  fg = Color::FG; }
        draw_button(s, x, y, BTN_W, BTN_H, k.label, bg, fg);
    }
}

void try_submit() {
    if (secrets::is_valid_unlock(g_code)) {
        ble_send_command("0:C-1-3@");
        g_unlock_sent_ms = millis();
        g_code[0] = '\0';
        // stay on numpad; ui_loop's lock detector will leave once the bike
        // confirms kickstand_locked is false.
    } else {
        g_code[0] = '\0';      // silent reset per spec
    }
}

void on_touch(int x, int y) {
    if (g_unlock_sent_ms) return;        // ignore taps while waiting

    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        const Key& k = keys[i];
        int bx = GRID_X0 + k.col * (BTN_W + GAP);
        int by = GRID_Y0 + k.row * (BTN_H + GAP);
        if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) {
            g_pressed_idx = i;
            g_pressed_ms  = millis();
            if (strcmp(k.label, "DEL") == 0) {
                size_t l = strlen(g_code);
                if (l > 0) g_code[l - 1] = '\0';
            } else if (strcmp(k.label, "OK") == 0) {
                try_submit();
            } else if (strlen(g_code) < 4) {
                size_t l = strlen(g_code);
                g_code[l]     = k.label[0];
                g_code[l + 1] = '\0';
            }
            return;
        }
    }
}

}  // namespace NumpadScreen

// ---------------------------------------------------------------------------
//  Common page chrome (back arrow + title)
// ---------------------------------------------------------------------------
struct BackHit { int x = 0, y = 0, w = 64, h = 40; } g_back_hit;

void draw_page_chrome(LGFX_Sprite& s, const char* title) {
    // Back chip: rounded background + centered chevron. The big hit
    // target makes it impossible to miss compared to the bare chevron.
    s.fillRoundRect(4, 4, 52, 30, 6, Color::PANEL);
    s.fillTriangle(16, 19, 32, 9, 32, 29, Color::FG);

    s.setFont(&fonts::FreeSans12pt7b);
    s.setTextDatum(top_center);
    s.setTextColor(Color::FG, Color::BG);
    s.drawString(title, DISPLAY_WIDTH / 2, 8);
}

// Small dim label drawn above a control. All settings pages use this for
// per-card titles so things line up.
void draw_card_label(LGFX_Sprite& s, int x, int y, const char* txt,
                     uint16_t color = Color::DIM) {
    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextDatum(top_left);
    s.setTextColor(color, Color::BG);
    s.drawString(txt, x + 4, y);
}

// Two-button segmented control. `active_idx`: -1 = neither selected
// (use this when the underlying state is unknown), 0 = left active,
// 1 = right active.
void draw_segmented2(LGFX_Sprite& s, int x, int y, int w, int h,
                     const char* a_label, const char* b_label,
                     int active_idx) {
    int half = (w - 6) / 2;
    bool a_on = (active_idx == 0);
    bool b_on = (active_idx == 1);
    draw_button(s, x, y, half, h, a_label,
                a_on ? Color::ACCENT : Color::PANEL,
                a_on ? Color::BG     : Color::FG);
    draw_button(s, x + half + 6, y, half, h, b_label,
                b_on ? Color::ACCENT : Color::PANEL,
                b_on ? Color::BG     : Color::FG);
}

// Shared card geometry for every settings sub-page.
constexpr int CARD_X      = 16;
constexpr int CARD_W      = DISPLAY_WIDTH - 32;
constexpr int CARD_LABEL_H= 14;
constexpr int CARD_CTRL_H = 28;          // tightened to fit 4 cards on Bike Controls
constexpr int CARD_Y0     = 42;
constexpr int CARD_PITCH  = 50;
inline int card_label_y(int i)   { return CARD_Y0 + i * CARD_PITCH; }
inline int card_ctrl_y(int i)    { return card_label_y(i) + CARD_LABEL_H + 2; }
// segmented hit test helper: returns 0 if left, 1 if right, -1 if neither
inline int hit_segmented2(int x, int y, int card_y) {
    int cy = card_ctrl_y(card_y);
    if (y < cy || y >= cy + CARD_CTRL_H) return -1;
    int half = (CARD_W - 6) / 2;
    if (x >= CARD_X && x < CARD_X + half) return 0;
    if (x >= CARD_X + half + 6 && x < CARD_X + CARD_W) return 1;
    return -1;
}
// stepper hit test: returns -1/+1 for minus/plus, 0 for body, -2 if outside
inline int hit_stepper(int x, int y, int card_y) {
    int cy = card_ctrl_y(card_y);
    if (y < cy || y >= cy + CARD_CTRL_H) return -2;
    constexpr int BTN_W = 36;
    if (x >= CARD_X && x < CARD_X + BTN_W) return -1;
    if (x >= CARD_X + CARD_W - BTN_W && x < CARD_X + CARD_W) return +1;
    if (x >= CARD_X && x < CARD_X + CARD_W) return 0;
    return -2;
}
bool consumed_back(int x, int y, Screen back_to) {
    if (x < g_back_hit.x + g_back_hit.w && y < g_back_hit.y + g_back_hit.h) {
        set_screen(back_to);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  SETTINGS index — a vertical list of category buttons
// ---------------------------------------------------------------------------
namespace SettingsScreen {

struct Row { const char* label; Screen target; };
const Row rows[] = {
    {"Bluetooth",     Screen::SETTINGS_BT},
    {"Display / Power", Screen::SETTINGS_DISPLAY},
    {"Speed",         Screen::SETTINGS_SPEED},
    {"Bike Controls", Screen::SETTINGS_BIKE},
    {"Diagnostics",   Screen::SETTINGS_DIAG},
    {"Command Prompt",Screen::SETTINGS_CMD},
};
constexpr int ROW_X = 24;
constexpr int ROW_Y0 = 42;
constexpr int ROW_W = DISPLAY_WIDTH - 48;
constexpr int ROW_H = 28;
constexpr int ROW_GAP = 4;

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Settings");
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        int y = ROW_Y0 + (int)i * (ROW_H + ROW_GAP);
        draw_button(s, ROW_X, y, ROW_W, ROW_H, rows[i].label,
                    Color::PANEL, Color::FG);
    }
}
void on_touch(int x, int y) {
    if (consumed_back(x, y, Screen::MAIN)) return;
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        int ry = ROW_Y0 + (int)i * (ROW_H + ROW_GAP);
        if (y >= ry && y < ry + ROW_H && x >= ROW_X && x < ROW_X + ROW_W) {
            set_screen(rows[i].target);
            return;
        }
    }
}
}  // namespace SettingsScreen

// ---------------------------------------------------------------------------
//  Bluetooth settings sub-page
// ---------------------------------------------------------------------------
namespace BTSettings {
constexpr int BTN_X = 16, BTN_W = DISPLAY_WIDTH - 32, BTN_H = 38;
constexpr int REPAIR_Y = 50;

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Bluetooth");

    draw_button(s, BTN_X, REPAIR_Y, BTN_W, BTN_H,
                "Re-pair bike", Color::WARN, Color::FG);

    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextDatum(top_left);
    s.setTextColor(Color::DIM, Color::BG);
    const char* st = "?";
    switch (g_state.ble) {
        case BikeState::BleStatus::Idle:         st = "idle"; break;
        case BikeState::BleStatus::Scanning:     st = "scanning"; break;
        case BikeState::BleStatus::Connecting:   st = "connecting"; break;
        case BikeState::BleStatus::Connected:    st = "connected"; break;
        case BikeState::BleStatus::Disconnected: st = "disconnected"; break;
        default: break;
    }
    char buf[64];
    int info_y = REPAIR_Y + BTN_H + 20;
    snprintf(buf, sizeof(buf), "Status: %s", st);
    s.drawString(buf, 20, info_y);
    snprintf(buf, sizeof(buf), "Bonded: %s", g_state.bonded ? "yes" : "no");
    s.drawString(buf, 20, info_y + 18);
}
void on_touch(int x, int y) {
    if (consumed_back(x, y, Screen::SETTINGS)) return;
    if (y >= REPAIR_Y && y < REPAIR_Y + BTN_H &&
        x >= BTN_X && x < BTN_X + BTN_W) {
        ble_forget_bike();
    }
}
}  // namespace BTSettings

// ---------------------------------------------------------------------------
//  Display / Power sub-page
// ---------------------------------------------------------------------------
namespace DisplaySettings {

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Display / Power");

    draw_card_label(s, CARD_X, card_label_y(0), "Display");
    draw_button(s, CARD_X, card_ctrl_y(0), CARD_W, CARD_CTRL_H,
                "Sleep now", Color::ACCENT, Color::BG);

    draw_card_label(s, CARD_X, card_label_y(1),
                    "Sleep timeout (after disconnect)");
    char buf[24];
    snprintf(buf, sizeof(buf), "%d s", g_state.sleep_timeout_s);
    draw_stepper(s, CARD_X, card_ctrl_y(1), CARD_W, CARD_CTRL_H,
                 buf, Color::PANEL, Color::FG);

    draw_card_label(s, CARD_X, card_label_y(2), "Brightness");
    snprintf(buf, sizeof(buf), "%d / 10", g_state.brightness);
    draw_stepper(s, CARD_X, card_ctrl_y(2), CARD_W, CARD_CTRL_H,
                 buf, Color::PANEL, Color::FG);

    draw_card_label(s, CARD_X, card_label_y(3), "PAS Timeout (10 min idle)");
    draw_segmented2(s, CARD_X, card_ctrl_y(3), CARD_W, CARD_CTRL_H,
                    "On", "Off", g_state.pas_timeout_enabled ? 0 : 1);
}

void on_touch(int x, int y) {
    if (consumed_back(x, y, Screen::SETTINGS)) return;

    if (y >= card_ctrl_y(0) && y < card_ctrl_y(0) + CARD_CTRL_H &&
        x >= CARD_X && x < CARD_X + CARD_W) {
        power::sleep_now();
        set_screen(Screen::MAIN);
        return;
    }
    int s_tap = hit_stepper(x, y, 1);
    if (s_tap == -1) g_state.sleep_timeout_s =
        max(SLEEP_TIMEOUT_MIN_S, g_state.sleep_timeout_s - 5);
    else if (s_tap == +1) g_state.sleep_timeout_s =
        min(SLEEP_TIMEOUT_MAX_S, g_state.sleep_timeout_s + 5);
    int b_tap = hit_stepper(x, y, 2);
    if (b_tap == -1) g_state.brightness = max(BRIGHTNESS_MIN, g_state.brightness - 1);
    else if (b_tap == +1) g_state.brightness = min(BRIGHTNESS_MAX, g_state.brightness + 1);
    if (b_tap == -1 || b_tap == +1) power::apply_brightness();

    int p_tap = hit_segmented2(x, y, 3);
    if (p_tap == 0)      g_state.pas_timeout_enabled = true;
    else if (p_tap == 1) g_state.pas_timeout_enabled = false;
}

}  // namespace DisplaySettings

// ---------------------------------------------------------------------------
//  Speed sub-page
// ---------------------------------------------------------------------------
namespace SpeedSettings {

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Speed");

    draw_card_label(s, CARD_X, card_label_y(0), "Top speed (mph)");
    char buf[24]; snprintf(buf, sizeof(buf), "%d", g_state.top_speed);
    draw_stepper(s, CARD_X, card_ctrl_y(0), CARD_W, CARD_CTRL_H,
                 buf, Color::PANEL, Color::FG);

    // hint under the stepper, tiny font
    s.setFont(&fonts::Font0);
    s.setTextDatum(top_left);
    s.setTextColor(Color::DIM, Color::BG);
    s.drawString("display reads this when the wheel is at full speed",
                 CARD_X + 4, card_ctrl_y(0) + CARD_CTRL_H + 4);
}

void on_touch(int x, int y) {
    if (consumed_back(x, y, Screen::SETTINGS)) return;
    int s_tap = hit_stepper(x, y, 0);
    if (s_tap == -1) g_state.top_speed = max(TOP_SPEED_MIN, g_state.top_speed - 1);
    else if (s_tap == +1) g_state.top_speed = min(TOP_SPEED_MAX, g_state.top_speed + 1);
}

}  // namespace SpeedSettings

// ---------------------------------------------------------------------------
//  Bike Controls sub-page
// ---------------------------------------------------------------------------
namespace BikeSettings {

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Bike Controls");

    // --- Kickstand engage --- (this screen is only reachable while
    // unlocked; the locked-state UI is the dedicated numpad screen)
    bool can_engage = g_state.kickstand_down;
    draw_card_label(s, CARD_X, card_label_y(0),
        g_state.kickstand_down ? "Kickstand (down)"
                               : "Kickstand (up — must be down to lock)",
        can_engage ? Color::FG : Color::DIM);
    draw_button(s, CARD_X, card_ctrl_y(0), CARD_W, CARD_CTRL_H,
                "Engage Lock", Color::WARN, Color::FG, can_engage);

    // --- Headlight (C-1-4 on, C-1-5 off, state echoed by R-*) ---
    draw_card_label(s, CARD_X, card_label_y(1), "Headlight");
    draw_segmented2(s, CARD_X, card_ctrl_y(1), CARD_W, CARD_CTRL_H,
                    "On", "Off", g_state.headlight_on ? 0 : 1);

    // --- Badge Light / EL strip (C-1-16 on, C-1-17 off, no R-* echo) ---
    auto bl = g_state.badge_light;
    int bl_idx = (bl == BikeState::BadgeLight::On)  ? 0
               : (bl == BikeState::BadgeLight::Off) ? 1 : -1;
    const char* bl_label = (bl == BikeState::BadgeLight::Unknown)
                           ? "Badge Light (tap to set)" : "Badge Light";
    draw_card_label(s, CARD_X, card_label_y(2), bl_label);
    draw_segmented2(s, CARD_X, card_ctrl_y(2), CARD_W, CARD_CTRL_H,
                    "On", "Off", bl_idx);

    // --- Brake Warn (alternating signals + badge blink while braking) ---
    draw_card_label(s, CARD_X, card_label_y(3), "Brake Warn");
    draw_segmented2(s, CARD_X, card_ctrl_y(3), CARD_W, CARD_CTRL_H,
                    "On", "Off", g_state.brake_warn_enabled ? 0 : 1);
}

void on_touch(int x, int y) {
    if (consumed_back(x, y, Screen::SETTINGS)) return;

    int kcy = card_ctrl_y(0);
    if (y >= kcy && y < kcy + CARD_CTRL_H &&
        x >= CARD_X && x < CARD_X + CARD_W) {
        if (g_state.kickstand_down) set_screen(Screen::LOCK_CONFIRM);
        else                        show_toast("Deploy Kickstand To Lock");
        return;
    }
    int h_tap = hit_segmented2(x, y, 1);
    if (h_tap == 0) ble_send_command("0:C-1-4@");
    else if (h_tap == 1) ble_send_command("0:C-1-5@");

    int b_tap = hit_segmented2(x, y, 2);
    if (b_tap == 0) {
        ble_send_command("0:C-1-16@");
        g_state.badge_light = BikeState::BadgeLight::On;
    } else if (b_tap == 1) {
        ble_send_command("0:C-1-17@");
        g_state.badge_light = BikeState::BadgeLight::Off;
    }

    int bw_tap = hit_segmented2(x, y, 3);
    if (bw_tap == 0)      g_state.brake_warn_enabled = true;
    else if (bw_tap == 1) g_state.brake_warn_enabled = false;
}

}  // namespace BikeSettings

// ---------------------------------------------------------------------------
//  Diagnostics sub-page (stub)
// ---------------------------------------------------------------------------
namespace DiagSettings {

// The board's UART header is wired to the bike's BLE-module debug port —
// the same channel that revealed the BLE passkey originally. Diagnostics
// renders incoming bytes as text so we can read whatever the module
// chatters about. Non-printable bytes are shown as '·' so binary noise
// doesn't break the layout.

// Sanitize a single line for display: replace non-printable chars with '.'
// and bound length so the bitmap font doesn't run off the screen.
void sanitize_to(char* out, size_t outlen, const char* in) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    out[j] = '\0';
}

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Diagnostics");

    char buf[96];
    int y = 42;

    // ---- UART status line ----
    uint32_t bytes = uart_bytes_received();
    uint32_t last  = uart_last_byte_ms();
    uint32_t age   = last ? (millis() - last) : 0;
    bool flowing   = last && age < 3000;
    uint16_t status_c = (bytes == 0) ? Color::WARN
                       : flowing     ? Color::OK
                                     : Color::SIGNAL;
    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextDatum(top_left);
    s.setTextColor(status_c, Color::BG);
    if (bytes == 0) {
        snprintf(buf, sizeof(buf), "UART (bike): 0 bytes — check wiring");
    } else if (flowing) {
        snprintf(buf, sizeof(buf), "UART (bike): %u bytes — flowing",
                 (unsigned)bytes);
    } else {
        snprintf(buf, sizeof(buf), "UART (bike): %u bytes — last %us ago",
                 (unsigned)bytes, (unsigned)(age / 1000));
    }
    s.drawString(buf, 12, y); y += 18;

    // ---- Received text: oldest line first, then partial (live) line ----
    s.setFont(&fonts::Font0);
    s.setTextColor(Color::FG, Color::BG);
    int n = uart_recent_lines_count();
    char tr[60];
    // print oldest → newest so the layout reads top-down chronologically.
    for (int i = n - 1; i >= 0; i--) {
        const char* line = uart_recent_line(i);
        if (!line || line[0] == '\0') continue;
        sanitize_to(tr, sizeof(tr), line);
        s.drawString(tr, 12, y);
        y += 10;
    }
    // live partial (no newline yet) — shown dim with a trailing cursor
    const char* partial = uart_partial_line();
    if (partial && partial[0] != '\0') {
        sanitize_to(tr, sizeof(tr) - 1, partial);
        size_t l = strlen(tr);
        if (l + 1 < sizeof(tr)) { tr[l] = '_'; tr[l + 1] = '\0'; }
        s.setTextColor(Color::SIGNAL, Color::BG);
        s.drawString(tr, 12, y);
        s.setTextColor(Color::FG, Color::BG);
    }

    // ---- Bike-side counters along the bottom ----
    s.setFont(&fonts::Font0);
    s.setTextColor(Color::DIM, Color::BG);
    snprintf(buf, sizeof(buf), "bike: spd=%.1f whl=%d odo=%u  heap=%u",
             (double)g_state.speed_mph, (int)g_state.wheel_pulse,
             (unsigned)g_state.odometer_counter,
             (unsigned)ESP.getFreeHeap());
    s.drawString(buf, 12, DISPLAY_HEIGHT - 14);
}

void on_touch(int x, int y) { consumed_back(x, y, Screen::SETTINGS); }

}  // namespace DiagSettings

// ---------------------------------------------------------------------------
//  Command Prompt sub-page — Wi-Fi access point + web console toggle
// ---------------------------------------------------------------------------
namespace CmdSettings {

void draw(LGFX_Sprite& s) {
    s.fillSprite(Color::BG);
    draw_page_chrome(s, "Command Prompt");

    bool on = cmd_ap::active();
    draw_button(s, CARD_X, card_ctrl_y(0), CARD_W, CARD_CTRL_H,
                on ? "Stop AP" : "Start AP",
                on ? Color::WARN  : Color::ACCENT,
                on ? Color::FG    : Color::BG);

    if (on) {
        // Left column: SSID / Pass / URL / clients (compact, smaller font)
        s.setFont(&fonts::FreeSans9pt7b);
        s.setTextDatum(top_left);
        int y = card_ctrl_y(0) + CARD_CTRL_H + 12;
        auto row = [&](const char* k, const char* v, uint16_t vc) {
            s.setTextColor(Color::DIM, Color::BG);
            s.drawString(k, 12, y);
            s.setTextColor(vc, Color::BG);
            s.drawString(v, 50, y);
            y += 16;
        };
        row("SSID", cmd_ap::ssid(),     Color::FG);
        row("Pass", cmd_ap::password(), Color::FG);
        // Trim the http:// prefix so the IP fits in the narrowed column.
        const char* u = cmd_ap::ip_string();
        if (strncmp(u, "http://", 7) == 0) u += 7;
        row("URL",  u, Color::ACCENT);
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "%d client%s",
                 cmd_ap::client_count(),
                 cmd_ap::client_count() == 1 ? "" : "s");
        s.setTextColor(Color::DIM, Color::BG);
        s.drawString(cbuf, 12, y + 4);

        // Right side: scannable Wi-Fi QR — phone joins on tap.
        int qr_x = DISPLAY_WIDTH - WIFI_QR_BITMAP_W - 8;
        int qr_y = card_ctrl_y(0) + CARD_CTRL_H + 8;
        s.pushImage(qr_x, qr_y,
                    WIFI_QR_BITMAP_W, WIFI_QR_BITMAP_H, wifi_qr_bitmap);
    } else {
        s.setFont(&fonts::Font0);
        s.setTextDatum(top_left);
        s.setTextColor(Color::DIM, Color::BG);
        int hy = card_ctrl_y(0) + CARD_CTRL_H + 12;
        s.drawString("Hosts a Wi-Fi AP + web page so any phone/laptop on",  CARD_X + 4, hy);
        s.drawString("the SSID can send BLE commands to the bike. Stays",   CARD_X + 4, hy + 12);
        s.drawString("up until you tap Stop, leave this page, or reboot.",  CARD_X + 4, hy + 24);
    }
}

void on_touch(int x, int y) {
    if (consumed_back(x, y, Screen::SETTINGS)) return;
    int by = card_ctrl_y(0);
    if (y >= by && y < by + CARD_CTRL_H &&
        x >= CARD_X && x < CARD_X + CARD_W) {
        if (cmd_ap::active()) cmd_ap::stop();
        else                  cmd_ap::start();
    }
}

}  // namespace CmdSettings

// ---------------------------------------------------------------------------
//  Dispatch
// ---------------------------------------------------------------------------
void draw_current(LGFX_Sprite& s) {
    switch (g_screen) {
        case Screen::MAIN:             MainScreen::draw(s);     break;
        case Screen::NUMPAD:           NumpadScreen::draw(s);   break;
        case Screen::LOCK_CONFIRM:     LockConfirm::draw(s);    break;
        case Screen::SETTINGS:         SettingsScreen::draw(s); break;
        case Screen::SETTINGS_BT:      BTSettings::draw(s);     break;
        case Screen::SETTINGS_DISPLAY: DisplaySettings::draw(s);break;
        case Screen::SETTINGS_SPEED:   SpeedSettings::draw(s);  break;
        case Screen::SETTINGS_BIKE:    BikeSettings::draw(s);   break;
        case Screen::SETTINGS_DIAG:    DiagSettings::draw(s);   break;
        case Screen::SETTINGS_CMD:     CmdSettings::draw(s);    break;
    }
    // Toast is rendered last so it sits on top of everything.
    draw_toast(s);
}

void dispatch_touch(int x, int y) {
    switch (g_screen) {
        case Screen::MAIN:             MainScreen::on_touch(x, y);     break;
        case Screen::NUMPAD:           NumpadScreen::on_touch(x, y);   break;
        case Screen::LOCK_CONFIRM:     LockConfirm::on_touch(x, y);    break;
        case Screen::SETTINGS:         SettingsScreen::on_touch(x, y); break;
        case Screen::SETTINGS_BT:      BTSettings::on_touch(x, y);     break;
        case Screen::SETTINGS_DISPLAY: DisplaySettings::on_touch(x, y);break;
        case Screen::SETTINGS_SPEED:   SpeedSettings::on_touch(x, y);  break;
        case Screen::SETTINGS_BIKE:    BikeSettings::on_touch(x, y);   break;
        case Screen::SETTINGS_DIAG:    DiagSettings::on_touch(x, y);   break;
        case Screen::SETTINGS_CMD:     CmdSettings::on_touch(x, y);    break;
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void ui_setup(LGFX* tft) {
    g_tft = tft;
    Serial.printf("[ui] setup  free heap=%u  free PSRAM=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    g_buf.setColorDepth(16);
    g_buf.setPsram(true);
    g_sprite_ok = g_buf.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!g_sprite_ok) Serial.println("[ui] FATAL: createSprite failed");
}

void ui_loop() {
    if (!g_tft || !g_sprite_ok) return;

    // Lock state drives the numpad screen unconditionally. Whenever the
    // bike reports kickstand_locked = true we force the numpad and there's
    // no other path off it; when the bike confirms unlock we leave to MAIN.
    if (g_state.kickstand_locked) {
        if (g_screen != Screen::NUMPAD) {
            NumpadScreen::reset();
            g_screen = Screen::NUMPAD;
        }
    } else if (g_screen == Screen::NUMPAD) {
        NumpadScreen::reset();
        g_screen = Screen::MAIN;
    }
    if (power::just_woke() && g_state.kickstand_locked) {
        NumpadScreen::reset();  // don't leave stale digits across sleeps
    }
    NumpadScreen::update();

    // Skip rendering entirely while asleep — backlight is off anyway.
    if (power::state() == power::State::SLEEP) return;

    static uint32_t last_frame = 0;
    uint32_t now = millis();
    if (now - last_frame < 33) return;
    last_frame = now;

    // GOODBYE / wake-splash images are static — rendering them every
    // frame causes a visible flicker as ~150 KB of SPI traffic streams
    // through the panel 30 times per second. Render once per window
    // and let the panel hold the image until the window closes.
    static bool s_goodbye_rendered = false;
    static bool s_splash_rendered  = false;

    if (power::state() == power::State::GOODBYE) {
        if (!s_goodbye_rendered) {
            g_tft->pushImage(0, 0, GOODBYE_BITMAP_W, GOODBYE_BITMAP_H,
                             goodbye_bitmap);
            s_goodbye_rendered = true;
        }
        return;
    }
    s_goodbye_rendered = false;

    if (power::show_wake_splash()) {
        if (!s_splash_rendered) {
            g_tft->pushImage(0, 0, SPLASH_BITMAP_W, SPLASH_BITMAP_H,
                             splash_bitmap);
            g_tft->setFont(&fonts::FreeSans12pt7b);
            g_tft->setTextDatum(top_center);
            g_tft->setTextColor(TFT_WHITE, TFT_BLACK);
            g_tft->drawString(USER_SPLASH_TAGLINE, DISPLAY_WIDTH / 2, 165);
            s_splash_rendered = true;
        }
        return;
    }
    s_splash_rendered = false;

    draw_current(g_buf);
    g_buf.pushSprite(g_tft, 0, 0);
}

void ui_on_touch(int x, int y) {
    power::touch_activity();
    if (power::state() == power::State::SLEEP) return;   // wake was the action
    dispatch_touch(x, y);
}
