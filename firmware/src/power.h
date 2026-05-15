// ----------------------------------------------------------------------------
//  power.h — display power & idle state machine.
//
//   AWAKE     backlight on, UI updating
//   GOODBYE   transitional 1s window where the dashboard shows a parting
//             "WORST. BIKE. EVER." image before going dark
//   SLEEP     backlight off, UI rendering paused (BLE callbacks still wake
//             us; CPU light-sleeps when idle)
//
//  Any activity (touch, BLE notification, manual wake) returns us to AWAKE.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include "lgfx_board.h"

namespace power {

enum class State : uint8_t { AWAKE, GOODBYE, SLEEP };

void  setup(LGFX* tft);
void  loop();

void  touch_activity();   // call on any touch event
void  ble_activity();     // call from BLE notification handler

void  sleep_now();        // manual: jump straight through GOODBYE → SLEEP
void  wake_now();         // manual: jump straight to AWAKE
void  bike_slept();       // bike confirmed sleep — start the goodbye

State state();
bool  just_woke();        // returns true once after a SLEEP→AWAKE transition
// True for ~1 s after waking from a *manual* sleep (lock / Sleep-Now).
// The UI uses it to show the boot splash on wake. False after auto-sleep
// (idle timeout, bike_slept).
bool  show_wake_splash();

void  apply_brightness(); // re-read g_state.brightness and apply

}  // namespace power
