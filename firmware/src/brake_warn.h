// ----------------------------------------------------------------------------
//  brake_warn.h — blink headlight + badge light while the rider is braking.
//
//  Behavior (when g_state.brake_warn_enabled is true and BLE is connected):
//      * brake held for ≥750 ms → start alternating blink
//      * headlight and badge swap every second (one on while the other is off)
//      * on release: both lights restored to whatever state they were in
//        right before the brake was applied
//
//  The Reevo's stock brakes are mushy enough that a visible warning to
//  traffic behind matters. This is a quality-of-life feature, gated by a
//  toggle in Bike Controls.
// ----------------------------------------------------------------------------
#pragma once

namespace brake_warn {

void setup();
void loop();
bool active();   // true while lights are actively blinking

}  // namespace brake_warn
