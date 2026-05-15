// ----------------------------------------------------------------------------
//  main.cpp — Reevo Dashboard firmware entry point.
//
//  Owns the display + touch hardware. Delegates BLE to ble.cpp and all
//  rendering / touch handling to ui.cpp. This file stays tiny so it's
//  easy to keep in mind.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include "lgfx_board.h"
#include "state.h"
#include "ble.h"
#include "ui.h"
#include "uart_tap.h"
#include "power.h"
#include "cmd_ap.h"
#include "brake_warn.h"
#include "pas_timeout.h"
#include "secrets.h"
#include "theme.h"
#include "assets.h"
#include "user_config.h"

BikeState g_state;
static LGFX tft;

// ---------------------------------------------------------------------------
//  Touch — LovyanGFX's FT5x06 driver doesn't auto-rotate touch coords,
//  so we pull raw and apply our own rotation-1 transform.
// ---------------------------------------------------------------------------
static inline void touch_transform(int32_t rx, int32_t ry,
                                   int32_t* x, int32_t* y) {
    *x = (PANEL_NATIVE_HEIGHT - 1) - ry;
    *y = rx;
}

static bool get_touch(int32_t* x, int32_t* y) {
    int32_t rx, ry;
    if (!tft.getTouchRaw(&rx, &ry)) return false;
    touch_transform(rx, ry, x, y);
    return true;
}

// ---------------------------------------------------------------------------
//  Splash — shown once at boot
// ---------------------------------------------------------------------------
static void splash() {
    // Blit the bike-art bitmap; the Reevo wordmark is baked in at center.
    tft.pushImage(0, 0, SPLASH_BITMAP_W, SPLASH_BITMAP_H, splash_bitmap);
    // Tagline below the wordmark. White-on-dark sits on the bike art well.
    tft.setFont(&fonts::FreeSans12pt7b);
    tft.setTextDatum(top_center);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);  // BG is dark; antialiased edges blend ok
    tft.drawString(USER_SPLASH_TAGLINE, DISPLAY_WIDTH / 2, 165);
    delay(1500);
}

// ---------------------------------------------------------------------------
//  Setup + loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    uint32_t deadline = millis() + 4000;
    while (!Serial && millis() < deadline) delay(10);
    Serial.println();
    Serial.println("[boot] === Reevo dashboard ===");
    Serial.printf("[boot] free heap=%u  free PSRAM=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    Serial.println("[boot] tft.init()");
    tft.init();
    Serial.println("[boot] setRotation/setBrightness");
    tft.setRotation(DISPLAY_ROTATION);
    tft.setBrightness(brightness_to_pwm(g_state.brightness));

    Serial.println("[boot] splash()");
    splash();

    Serial.println("[boot] secrets::setup()");
    secrets::setup();

    Serial.println("[boot] theme::setup()");
    theme::setup();

    Serial.println("[boot] ui_setup()");
    ui_setup(&tft);

    Serial.println("[boot] power::setup()");
    power::setup(&tft);

    Serial.println("[boot] uart_tap_setup()");
    uart_tap_setup();

    Serial.println("[boot] cmd_ap::setup()");
    cmd_ap::setup();

    Serial.println("[boot] brake_warn::setup()");
    brake_warn::setup();

    Serial.println("[boot] pas_timeout::setup()");
    pas_timeout::setup();

    Serial.println("[boot] ble_setup()");
    ble_setup();

    Serial.println("[boot] setup() complete — entering loop");
}

void loop() {
    ble_loop();
    uart_tap_loop();
    cmd_ap::loop();
    brake_warn::loop();
    pas_timeout::loop();

    // Touch: dispatch on rising-edge only. Note that ui_on_touch() itself
    // calls power::touch_activity(), so a touch while asleep will wake first
    // and then be processed.
    static bool was_down = false;
    int32_t x, y;
    bool down = get_touch(&x, &y);
    if (down && !was_down) {
        ui_on_touch((int)x, (int)y);
    }
    was_down = down;

    power::loop();
    ui_loop();
    delay(5);
}
