// ----------------------------------------------------------------------------
//  state.h — single source of truth.
//  Bike-side fields are written by ble.cpp from R-* notifications.
//  UI / user settings live here too.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include "config.h"

struct BikeState {
    // ---- Live telemetry mirrored from R-* ----
    float    speed_mph         = 0.0f;     // odometer-derived (legacy)
    uint8_t  battery_pct       = 0;
    uint8_t  battery2_pct      = 0;
    uint8_t  assist_level      = 0;
    bool     headlight_on      = false;
    bool     left_signal       = false;
    bool     right_signal      = false;
    bool     front_brake       = false;
    bool     rear_brake        = false;
    bool     kickstand_down    = false;
    bool     kickstand_locked  = false;
    bool     throttle_enabled  = true;
    // Badge light (EL strip on the head badge). The bike doesn't echo its
    // state in any R-* register we've decoded, so we can't sync at boot —
    // we keep a tri-state and only flip to Known after the user toggles.
    enum class BadgeLight : int8_t { Unknown = -1, Off = 0, On = 1 };
    BadgeLight badge_light = BadgeLight::Unknown;

    // ---- R-2-1 stream ----
    uint16_t wheel_pulse      = 0;
    uint32_t odometer_counter  = 0;
    uint32_t odo_last_ms       = 0;
    // Trip distance baseline. trip_miles = (odo_counter - this) * tick→mile
    // factor. Reset by the rider tapping the trip button on the main screen.
    uint32_t trip_baseline_odo = 0;

    // ---- BLE connection ----
    enum class BleStatus { Idle, Scanning, Connecting, Pairing, Connected, Disconnected };
    BleStatus ble                  = BleStatus::Idle;
    bool      bonded               = false;
    int       reconnect_attempt    = 0;

    // ---- User settings (would be persisted to NVS later) ----
    int   top_speed       = TOP_SPEED_DEFAULT;        // 1..20
    int   sleep_timeout_s = SLEEP_TIMEOUT_DEFAULT_S;  // 20..300
    int   brightness      = BRIGHTNESS_DEFAULT;       // 1..10
    bool  brake_warn_enabled = true;                  // alt-blink + badge on brake hold
    bool  pas_timeout_enabled = true;                 // drop PAS to 0 after 10 min idle

    // ---- Helpers ----
    float displayed_speed() const {
        if (wheel_pulse == 0) return 0.0f;
        float pct = (float)wheel_pulse / MAX_WHEEL_PULSE;
        if (pct > 1.0f) pct = 1.0f;
        return pct * (float)top_speed;
    }
    // Empirical: speed_mph = cps × 0.88 means 1 tick/sec ≈ 0.000244 mi/s.
    // So 1 odo tick ≈ 0.000244 mi.
    float trip_miles() const {
        if (odometer_counter < trip_baseline_odo) return 0.0f;
        return (float)(odometer_counter - trip_baseline_odo) * 0.000244f;
    }
};

extern BikeState g_state;
