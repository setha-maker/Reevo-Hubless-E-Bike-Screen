// ----------------------------------------------------------------------------
//  power.cpp — display power state machine.
//
//  AWAKE → GOODBYE (1s parting image) → SLEEP (backlight off, UI paused).
//  Any touch or BLE activity wakes us back to AWAKE.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include "power.h"
#include "state.h"
#include "config.h"

namespace {

constexpr uint32_t GOODBYE_MS = 1000;

LGFX*        g_tft = nullptr;
power::State g_state_pwr = power::State::AWAKE;
uint32_t     g_last_activity_ms = 0;
uint32_t     g_goodbye_started_ms = 0;
bool         g_just_woke = false;
// Set by sleep_now() — a manual/Lock-driven sleep that must stay asleep
// until the rider physically taps. Cleared by touch_activity(). Cleared
// by auto-sleep paths (idle timeout, bike_slept) so BLE traffic from the
// bike can still bring the screen back in those cases.
bool         g_force_sleep_until_touch = false;
// When non-zero, the UI should render the boot splash through this
// millis() — set by the touch that wakes us out of a forced sleep.
uint32_t     g_wake_splash_until_ms = 0;

void enter_state(power::State s) {
    if (s == g_state_pwr) return;
    if (g_state_pwr == power::State::SLEEP && s == power::State::AWAKE) {
        g_just_woke = true;
    }
    g_state_pwr = s;

    if (s == power::State::AWAKE) {
        if (g_tft) g_tft->setBrightness(brightness_to_pwm(g_state.brightness));
    } else if (s == power::State::GOODBYE) {
        g_goodbye_started_ms = millis();
        // Backlight stays on — the image needs to be visible.
    } else if (s == power::State::SLEEP) {
        if (g_tft) {
            g_tft->setBrightness(0);
            // Wipe the panel while the backlight is off so we don't flash
            // the last-rendered frame (e.g. the goodbye image) when the
            // user taps to wake.
            g_tft->fillScreen(0x0000);
        }
    }
}

}  // namespace

namespace power {

void setup(LGFX* tft) {
    g_tft = tft;
    g_last_activity_ms = millis();
    enter_state(State::AWAKE);
}

void loop() {
    uint32_t now = millis();
    uint32_t idle_ms    = now - g_last_activity_ms;
    uint32_t timeout_ms = (uint32_t)g_state.sleep_timeout_s * 1000U;

    switch (g_state_pwr) {
        case State::AWAKE:
            if (idle_ms >= timeout_ms) enter_state(State::GOODBYE);
            break;
        case State::GOODBYE:
            if (now - g_goodbye_started_ms >= GOODBYE_MS) enter_state(State::SLEEP);
            break;
        case State::SLEEP:
            // wait for ble_activity() / touch_activity() / wake_now()
            break;
    }
}

void touch_activity() {
    uint32_t now = millis();
    g_last_activity_ms = now;
    // If we were in a manual sleep, latch a 1-second splash window before
    // clearing the flag — the next ui_loop frames render the boot image.
    if (g_state_pwr == State::SLEEP && g_force_sleep_until_touch) {
        g_wake_splash_until_ms = now + 1000;
    }
    g_force_sleep_until_touch = false;
    if (g_state_pwr != State::AWAKE) enter_state(State::AWAKE);
}

void ble_activity() {
    g_last_activity_ms = millis();
    // BLE traffic only auto-wakes us out of an *auto* sleep. A manual sleep
    // (lock / Sleep-Now button) blocks the wake until the rider taps.
    if (g_state_pwr == State::SLEEP && !g_force_sleep_until_touch) {
        enter_state(State::AWAKE);
    }
}

void sleep_now() {
    if (g_state_pwr == State::AWAKE) {
        g_force_sleep_until_touch = true;
        enter_state(State::GOODBYE);
    }
}

void wake_now() {
    g_last_activity_ms = millis();
    g_force_sleep_until_touch = false;
    enter_state(State::AWAKE);
}

void bike_slept() {
    if (g_state_pwr == State::AWAKE) {
        g_force_sleep_until_touch = false;
        enter_state(State::GOODBYE);
    }
}

State state() { return g_state_pwr; }

bool just_woke() {
    bool b = g_just_woke;
    g_just_woke = false;
    return b;
}

bool show_wake_splash() {
    return millis() < g_wake_splash_until_ms;
}

void apply_brightness() {
    if (g_state_pwr == State::AWAKE && g_tft) {
        g_tft->setBrightness(brightness_to_pwm(g_state.brightness));
    }
}

}  // namespace power
