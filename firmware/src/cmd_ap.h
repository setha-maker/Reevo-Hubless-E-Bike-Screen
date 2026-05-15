// ----------------------------------------------------------------------------
//  cmd_ap.h — on-demand Wi-Fi access point + tiny web command console.
//
//  When the user taps "Start AP" on the Command Prompt settings screen,
//  the ESP32 brings up a SoftAP and a single-page web app. Joining the
//  SSID and visiting the IP in any browser gives a text box for sending
//  arbitrary BLE C-* commands plus a live log of incoming R-* notifications.
//
//  Wi-Fi is fully torn down when stopped so the radio doesn't draw power
//  while the user isn't using the feature. The AP also auto-stops when
//  the user leaves the Command Prompt settings page (handled by ui.cpp).
//  Soft display sleep (backlight off) does NOT stop the AP — only an
//  explicit Stop tap, leaving the page, or a hard reboot will.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace cmd_ap {

void setup();
void loop();

void start();
void stop();
bool active();
int  client_count();

const char* ssid();
const char* password();
const char* ip_string();

// Fed by ble.cpp on every incoming notification. Always buffered so the
// log can show recent history the moment the AP starts.
void log_notify(const char* line);

}  // namespace cmd_ap
