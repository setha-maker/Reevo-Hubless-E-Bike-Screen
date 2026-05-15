// ----------------------------------------------------------------------------
//  theme.h — user-customizable main-screen colors, persisted in NVS.
//
//  Only the speedometer (main) screen reads from here. Settings pages and
//  the numpad keep the default Color::BG/FG so navigation always looks
//  consistent regardless of what the rider has done to the dashboard.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace theme {

void setup();

uint16_t main_bg();      // RGB565
uint16_t main_speed();   // RGB565

// Accept 24-bit RGB (e.g. 0xFF8800). Returns true on success.
bool set_main_bg(uint32_t rgb888);
bool set_main_speed(uint32_t rgb888);

}  // namespace theme
