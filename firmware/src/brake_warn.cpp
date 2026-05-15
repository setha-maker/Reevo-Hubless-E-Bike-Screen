// ----------------------------------------------------------------------------
//  brake_warn.cpp — see brake_warn.h.
//
//  Pulses the headlight (C-1-4/5) and badge light (C-1-16/17) in phase
//  at 2 Hz while the rider holds the brake. Snapshots their pre-brake
//  states on entry and restores them on release, so toggling the warning
//  doesn't leave the bike in a different lighting state than the rider
//  expects.
// ----------------------------------------------------------------------------

#include <Arduino.h>

#include "brake_warn.h"
#include "state.h"
#include "ble.h"

namespace {

constexpr const char* CMD_LIGHT_ON  = "0:C-1-4@";
constexpr const char* CMD_LIGHT_OFF = "0:C-1-5@";
constexpr const char* CMD_BADGE_ON  = "0:C-1-16@";
constexpr const char* CMD_BADGE_OFF = "0:C-1-17@";

enum class Phase : uint8_t { IDLE, ARMING, BLINKING };
Phase    g_phase       = Phase::IDLE;
uint32_t g_phase_ms    = 0;
uint32_t g_blink_ms    = 0;
bool     g_light_phase = false;   // true = headlight on/badge off; false = swapped

// Pre-brake snapshot, captured the moment we transition ARMING → BLINKING
// and restored when we return to IDLE.
bool                   g_saved_headlight = false;
BikeState::BadgeLight  g_saved_badge     = BikeState::BadgeLight::Unknown;

constexpr uint32_t ARM_MS   = 750;
constexpr uint32_t BLINK_MS = 1000;    // each lamp gets a full second

bool braking() {
    return g_state.front_brake || g_state.rear_brake;
}
bool can_send() {
    return g_state.ble == BikeState::BleStatus::Connected;
}

// Alternating: one lamp on at a time, swapping every BLINK_MS.
// `light_phase=true`  → headlight ON,  badge OFF
// `light_phase=false` → headlight OFF, badge ON
void send_alternating(bool light_phase) {
    ble_send_command(light_phase ? CMD_LIGHT_ON  : CMD_LIGHT_OFF);
    ble_send_command(light_phase ? CMD_BADGE_OFF : CMD_BADGE_ON);
}

void enter_blinking(uint32_t now) {
    // Snapshot first — subsequent commands will echo back and update
    // g_state.headlight_on. We don't want to lose the original truth.
    g_saved_headlight = g_state.headlight_on;
    g_saved_badge     = g_state.badge_light;

    g_light_phase = true;
    send_alternating(g_light_phase);
    g_blink_ms = now;
    g_phase    = Phase::BLINKING;
    g_phase_ms = now;
}

void tick_blink(uint32_t now) {
    if (now - g_blink_ms < BLINK_MS) return;
    g_light_phase = !g_light_phase;
    send_alternating(g_light_phase);
    g_blink_ms = now;
}

void disengage() {
    // Restore exactly what we found. Badge::Unknown maps to off — best we
    // can do, since the bike doesn't echo badge state for us to query.
    ble_send_command(g_saved_headlight ? CMD_LIGHT_ON : CMD_LIGHT_OFF);
    ble_send_command(g_saved_badge == BikeState::BadgeLight::On
                     ? CMD_BADGE_ON : CMD_BADGE_OFF);
    g_phase       = Phase::IDLE;
    g_light_phase = false;
}

}  // namespace

namespace brake_warn {

void setup() { /* nothing eager */ }

void loop() {
    bool want = g_state.brake_warn_enabled && braking() && can_send();
    uint32_t now = millis();

    if (!want) {
        if (g_phase == Phase::BLINKING) disengage();
        else                            g_phase = Phase::IDLE;
        return;
    }

    switch (g_phase) {
        case Phase::IDLE:
            g_phase    = Phase::ARMING;
            g_phase_ms = now;
            break;
        case Phase::ARMING:
            if (now - g_phase_ms >= ARM_MS) enter_blinking(now);
            break;
        case Phase::BLINKING:
            tick_blink(now);
            break;
    }
}

bool active() { return g_phase == Phase::BLINKING; }

}  // namespace brake_warn
