// ----------------------------------------------------------------------------
//  theme.cpp — see theme.h.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <Preferences.h>

#include "theme.h"
#include "config.h"

namespace {

Preferences g_prefs;
uint16_t    g_bg    = Color::BG;
uint16_t    g_speed = Color::OK;

uint16_t rgb888_to_565(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >>  8) & 0xFF;
    uint8_t b = (rgb      ) & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

}  // namespace

namespace theme {

void setup() {
    g_prefs.begin("reevo_theme", false);
    g_bg    = (uint16_t)g_prefs.getUShort("bg",    Color::BG);
    g_speed = (uint16_t)g_prefs.getUShort("speed", Color::OK);
    Serial.printf("[theme] bg=0x%04X speed=0x%04X\n", g_bg, g_speed);
}

uint16_t main_bg()    { return g_bg; }
uint16_t main_speed() { return g_speed; }

bool set_main_bg(uint32_t rgb888) {
    g_bg = rgb888_to_565(rgb888);
    g_prefs.putUShort("bg", g_bg);
    return true;
}

bool set_main_speed(uint32_t rgb888) {
    g_speed = rgb888_to_565(rgb888);
    g_prefs.putUShort("speed", g_speed);
    return true;
}

}  // namespace theme
