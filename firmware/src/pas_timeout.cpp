// ----------------------------------------------------------------------------
//  pas_timeout.cpp — see pas_timeout.h.
// ----------------------------------------------------------------------------

#include <Arduino.h>

#include "pas_timeout.h"
#include "state.h"
#include "ble.h"

namespace {

constexpr uint32_t TIMEOUT_MS            = 10UL * 60UL * 1000UL;  // 10 min
constexpr uint32_t DECREMENT_INTERVAL_MS = 200;                   // C-1-11 paced

uint32_t g_last_activity_ms  = 0;
uint32_t g_last_decrement_ms = 0;
bool     g_firing            = false;
bool     g_latched_off       = false;     // we've already dropped PAS to 0
uint8_t  g_last_seen_assist  = 0;         // for detecting manual + button presses
bool     g_force_to_zero     = false;     // lock action requested an immediate drop

// Activity = the rider is currently touching the bike in some load-bearing
// way (wheel turning, brake squeezed), OR they just bumped PAS up. We only
// ever *decrement* via C-1-11, so any rise in assist_level is by definition
// the rider hitting the + button — that's intent, treat as activity so the
// timeout doesn't fight them.
bool activity_now() {
    bool ride = g_state.wheel_pulse > 0
             || g_state.front_brake
             || g_state.rear_brake;
    bool assist_up = g_state.assist_level > g_last_seen_assist;
    g_last_seen_assist = g_state.assist_level;
    return ride || assist_up;
}

}  // namespace

namespace pas_timeout {

void setup() {
    g_last_activity_ms = millis();
}

void loop() {
    uint32_t now = millis();
    bool can_send_now = (g_state.ble == BikeState::BleStatus::Connected);

    // Force-to-zero (lock action) runs independent of the timeout setting
    // and overrides any other state. Trickle out C-1-11 decrements until
    // the bike echoes assist_level=0.
    if (g_force_to_zero) {
        if (g_state.assist_level == 0) {
            g_force_to_zero = false;
            return;
        }
        if (can_send_now && (now - g_last_decrement_ms) >= DECREMENT_INTERVAL_MS) {
            ble_send_command("0:C-1-11@");
            g_last_decrement_ms = now;
        }
        return;
    }

    // Feature disabled → keep timer fresh and bail.
    if (!g_state.pas_timeout_enabled) {
        g_last_activity_ms = now;
        g_firing       = false;
        g_latched_off  = false;
        return;
    }

    // Any activity cancels firing and re-arms us for next time.
    if (activity_now()) {
        g_last_activity_ms = now;
        g_firing       = false;
        g_latched_off  = false;
        return;
    }

    // Already latched off after a successful drop — sit quiet until the
    // rider does something (wheel/brake/PAS+). Without this, the firmware
    // would re-fire every loop because the idle clock is still > 10 min.
    if (g_latched_off) return;

    bool can_send = (g_state.ble == BikeState::BleStatus::Connected);

    if (g_firing) {
        if (g_state.assist_level == 0) {
            g_firing      = false;
            g_latched_off = true;
            return;
        }
        if (can_send && (now - g_last_decrement_ms) >= DECREMENT_INTERVAL_MS) {
            ble_send_command("0:C-1-11@");   // pasDecrease
            g_last_decrement_ms = now;
        }
        return;
    }

    uint32_t idle = now - g_last_activity_ms;
    if (idle >= TIMEOUT_MS && g_state.assist_level > 0 && can_send) {
        g_firing            = true;
        g_last_decrement_ms = 0;
    }
}

void force_to_zero() {
    g_force_to_zero    = true;
    g_last_decrement_ms = 0;     // fire the first C-1-11 immediately
}

}  // namespace pas_timeout
