// ----------------------------------------------------------------------------
//  ble.cpp — NimBLE client that:
//    * scans for a device whose name starts with "REEVO"
//    * bonds using a fixed passkey (RN4870 factory default 111111)
//    * connects to the ISSC Transparent UART service and subscribes to
//      its NOTIFY characteristic
//    * parses each incoming "<seq>:R-X-Y,value..." line into g_state
//    * auto-reconnects with exponential backoff after a disconnect
//
//  Bond storage is handled by NimBLE itself via NVS — so a power cycle
//  just reconnects without re-pairing.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>
#include <stdlib.h>

#include "ble.h"
#include "config.h"
#include "state.h"
#include "power.h"
#include "cmd_ap.h"
#include "secrets.h"

// We use anonymous namespace for file-local everything.
namespace {

// ---------------------------------------------------------------------------
//  File-local state
// ---------------------------------------------------------------------------

NimBLEScan*                  g_scan       = nullptr;
NimBLEClient*                g_client     = nullptr;
NimBLEAddress                g_bike_addr;
bool                         g_have_addr  = false;

NimBLERemoteCharacteristic*  g_write_char  = nullptr;
NimBLERemoteCharacteristic*  g_notify_char = nullptr;

uint32_t g_next_attempt_ms = 0;
uint32_t g_backoff_ms      = BLE_RECONNECT_BACKOFF_MIN_MS;

// Pending event flags set by NimBLE callbacks, consumed by ble_loop().
volatile bool g_event_scan_found   = false;
volatile bool g_event_disconnected = false;

// Set whenever the dashboard sends C-1-5 (headlight off). We use this to
// suppress the bike-sleep heuristic for ~2 s afterward, because the bike
// echoes back R-1-4,0 for both user-initiated and auto-sleep transitions.
uint32_t g_user_headlight_off_ms = 0;

// ---------------------------------------------------------------------------
//  Tiny notification parser — maps R-X-Y,value into g_state
// ---------------------------------------------------------------------------

void apply_register(int group, int reg, const char* value) {
    auto as_bool = [&]() { return value[0] == '1'; };

    if (group == 1) {
        switch (reg) {
            case 1:  g_state.battery_pct       = (uint8_t)atoi(value); return;
            case 2:  g_state.battery2_pct      = (uint8_t)atoi(value); return;
            case 3:  g_state.kickstand_locked  = as_bool();            return;
            case 4: {
                bool new_val = as_bool();
                // The Reevo emits R-1-4,0 as part of its idle-sleep
                // transition (the bike automatically kills the headlight
                // when it sleeps). If we didn't just send a headlight-off
                // command ourselves, treat the 1→0 transition as the
                // bike's "going to sleep" signal.
                if (g_state.headlight_on && !new_val) {
                    bool ours = g_user_headlight_off_ms != 0 &&
                                (millis() - g_user_headlight_off_ms) < 2000;
                    if (!ours) power::bike_slept();
                }
                g_state.headlight_on = new_val;
                return;
            }
            case 5:  g_state.right_signal      = as_bool();            return;
            case 6:  g_state.left_signal       = as_bool();            return;
            case 7:  g_state.front_brake       = as_bool();            return;
            case 8:  g_state.rear_brake        = as_bool();            return;
            case 9:  g_state.kickstand_down    = as_bool();            return;
            case 10: g_state.assist_level      = (uint8_t)atoi(value); return;
            case 21: g_state.throttle_enabled  = as_bool();            return;
            default: return;
        }
    }
    if (group == 2 && reg == 1) {
        // R-2-1 = "255, wheel_pulse, odo_counter".
        //   wheel_pulse — instantaneous wheel-rotation sensor reading,
        //   NOT throttle position. Climbs from 0 to ~30 with bike speed;
        //   moves even when the bike is being pushed by hand.
        //   odo_counter — monotonically-increasing wheel-tick counter
        //   (rolls forward by ~4 per wheel revolution).
        int amax, pulse, odo;
        if (sscanf(value, "%d,%d,%d", &amax, &pulse, &odo) == 3) {
            uint32_t now_ms = millis();
            if (g_state.odo_last_ms != 0 && (uint32_t)odo > g_state.odometer_counter) {
                uint32_t dt        = now_ms - g_state.odo_last_ms;
                uint32_t dcounter  = (uint32_t)odo - g_state.odometer_counter;
                if (dt > 0 && dt < 5000) {
                    // Ticks-per-second × empirical 0.88 ≈ mph.
                    float cps = (float)dcounter * 1000.0f / (float)dt;
                    g_state.speed_mph = cps * 0.88f;
                }
            }
            g_state.wheel_pulse      = (uint16_t)pulse;
            g_state.odometer_counter = (uint32_t)odo;
            g_state.odo_last_ms      = now_ms;
        }
        return;
    }
}

void parse_notify(const uint8_t* data, size_t len) {
    if (len < 5 || len > 200) return;
    char buf[210];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    // Strip leading sequence number, e.g. "27:R-1-4,1" -> "R-1-4,1"
    const char* colon = strchr(buf, ':');
    const char* body  = colon ? colon + 1 : buf;

    if (body[0] != 'R' || body[1] != '-') return;   // only R-* updates state

    int group = 0, reg = 0;
    char value[160] = {0};   // zero-init so we never feed apply_register stack garbage
    int matched = sscanf(body, "R-%d-%d,%159[^\n]", &group, &reg, value);
    if (matched >= 2) apply_register(group, reg, value);
}

void on_notify_cb(NimBLERemoteCharacteristic* c, uint8_t* data,
                  size_t len, bool /*isNotify*/) {
    // Mirror the raw ASCII line into the command-AP log ring buffer so the
    // web console can show live bike chatter. Done before parsing so even
    // malformed lines still surface there.
    char line[128];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, data, n);
    line[n] = '\0';
    cmd_ap::log_notify(line);

    parse_notify(data, len);
    power::ble_activity();
}

// ---------------------------------------------------------------------------
//  NimBLE callbacks
// ---------------------------------------------------------------------------

class ScanCb : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* dev) override {
        const std::string& name = dev->getName();
        // Case-insensitive substring search for "reevo" — different bikes
        // advertise different names (e.g., "REEVO_xxxx" with per-unit suffix,
        // "Reevo Bike", "REEVO-XYZ"). Anything containing the word matches.
        std::string lower(name);
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("reevo") == std::string::npos) return;

        Serial.printf("[BLE] found %s  [%s]\n",
                      name.c_str(),
                      dev->getAddress().toString().c_str());
        g_bike_addr        = dev->getAddress();
        g_have_addr        = true;
        g_event_scan_found = true;
        g_scan->stop();
    }
};

class ClientCb : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* /*c*/) override {
        g_state.ble = BikeState::BleStatus::Connecting;
        Serial.println("[BLE] onConnect");
    }
    void onDisconnect(NimBLEClient* /*c*/) override {
        g_state.ble          = BikeState::BleStatus::Disconnected;
        g_event_disconnected = true;
        g_write_char         = nullptr;
        g_notify_char        = nullptr;
        Serial.println("[BLE] onDisconnect");
    }
    uint32_t onPassKeyRequest() override {
        const char* pk = secrets::pair_passkey();
        Serial.printf("[BLE] onPassKeyRequest -> %s\n", pk);
        return (uint32_t)atol(pk);
    }
    bool onConfirmPIN(uint32_t /*pin*/) override { return true; }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (desc->sec_state.bonded || desc->sec_state.encrypted) {
            g_state.bonded = true;
            Serial.println("[BLE] auth OK (bonded)");
        } else {
            g_state.bonded = false;
            Serial.println("[BLE] auth FAILED");
        }
    }
};

ScanCb*   g_scan_cb   = nullptr;
ClientCb* g_client_cb = nullptr;

// ---------------------------------------------------------------------------
//  State machine helpers
// ---------------------------------------------------------------------------

void start_scan() {
    if (!g_scan) {
        g_scan = NimBLEDevice::getScan();
        g_scan->setAdvertisedDeviceCallbacks(g_scan_cb, false);
        g_scan->setActiveScan(true);
        g_scan->setInterval(100);
        g_scan->setWindow(99);
    }
    Serial.println("[BLE] scan start");
    g_state.ble = BikeState::BleStatus::Scanning;
    g_scan->start(BLE_SCAN_DURATION_S, false);
}

bool ensure_client() {
    if (g_client) return true;
    g_client = NimBLEDevice::createClient();
    if (!g_client) return false;
    g_client->setClientCallbacks(g_client_cb, false);
    g_client->setConnectTimeout(8);
    return true;
}

bool connect_and_subscribe() {
    if (!ensure_client()) return false;
    if (!g_have_addr) return false;

    g_state.ble = BikeState::BleStatus::Connecting;
    Serial.printf("[BLE] connecting to %s...\n",
                  g_bike_addr.toString().c_str());

    if (!g_client->connect(g_bike_addr, false)) {
        Serial.println("[BLE] connect() failed");
        return false;
    }

    // Bond if we aren't already
    if (!g_client->secureConnection()) {
        Serial.println("[BLE] secureConnection() failed");
        g_client->disconnect();
        return false;
    }

    NimBLERemoteService* svc = g_client->getService(REEVO_SVC_UUID);
    if (!svc) {
        Serial.println("[BLE] ISSC service not found");
        g_client->disconnect();
        return false;
    }
    g_write_char  = svc->getCharacteristic(REEVO_WRITE_UUID);
    g_notify_char = svc->getCharacteristic(REEVO_NOTIFY_UUID);
    if (!g_write_char || !g_notify_char) {
        Serial.println("[BLE] required characteristics missing");
        g_client->disconnect();
        return false;
    }
    if (!g_notify_char->canNotify() ||
        !g_notify_char->subscribe(true, on_notify_cb)) {
        Serial.println("[BLE] subscribe() failed");
        g_client->disconnect();
        return false;
    }

    g_state.ble = BikeState::BleStatus::Connected;
    g_state.reconnect_attempt = 0;
    g_backoff_ms = BLE_RECONNECT_BACKOFF_MIN_MS;
    Serial.println("[BLE] connected + subscribed");
    return true;
}

void schedule_reconnect() {
    g_state.reconnect_attempt++;
    g_next_attempt_ms = millis() + g_backoff_ms;
    g_backoff_ms = g_backoff_ms < BLE_RECONNECT_BACKOFF_MAX_MS
                   ? min<uint32_t>(g_backoff_ms * 2, BLE_RECONNECT_BACKOFF_MAX_MS)
                   : BLE_RECONNECT_BACKOFF_MAX_MS;
    Serial.printf("[BLE] retry in %lu ms (attempt %d)\n",
                  (unsigned long)g_backoff_ms,
                  g_state.reconnect_attempt);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void ble_setup() {
    g_scan_cb   = new ScanCb();
    g_client_cb = new ClientCb();

    NimBLEDevice::init("ReevoDash");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Bond + MITM + Secure Connections, passkey supplied via callback.
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

    // If we've bonded before, NimBLE remembers the address — use it for
    // direct reconnect without scanning.
    int n_bonds = NimBLEDevice::getNumBonds();
    if (n_bonds > 0) {
        g_bike_addr = NimBLEDevice::getBondedAddress(0);
        g_have_addr = true;
        g_state.bonded = true;
        Serial.printf("[BLE] %d bond(s) on file, will reconnect to %s\n",
                      n_bonds, g_bike_addr.toString().c_str());
    } else {
        Serial.println("[BLE] no bonds on file, will scan");
    }

    g_state.ble = BikeState::BleStatus::Idle;
    g_next_attempt_ms = millis();  // try immediately
}

void ble_loop() {
    // Consume events first so they can't race with us
    if (g_event_scan_found) {
        g_event_scan_found = false;
        g_next_attempt_ms  = millis();   // try connect on the next tick
    }
    if (g_event_disconnected) {
        g_event_disconnected = false;
        schedule_reconnect();
    }

    // Age out stale speed: if no R-2-1 update for 1.5s the wheel has stopped
    // turning — zero the speed and the odometer baseline so the next update
    // won't compute a huge delta off a stale timestamp.
    if (g_state.odo_last_ms != 0 && millis() - g_state.odo_last_ms > 1500) {
        g_state.speed_mph    = 0.0f;
        g_state.wheel_pulse  = 0;
        g_state.odo_last_ms  = 0;
    }

    auto s = g_state.ble;

    if (s == BikeState::BleStatus::Connected) return;
    if (millis() < g_next_attempt_ms) return;

    if (g_have_addr) {
        if (!connect_and_subscribe()) {
            schedule_reconnect();
        }
    } else {
        start_scan();
        // Schedule the next attempt after the scan timeout in case nothing
        // was found.
        g_next_attempt_ms = millis() + (BLE_SCAN_DURATION_S + 1) * 1000;
    }
}

bool ble_send_command(const char* cmd) {
    if (!cmd || g_state.ble != BikeState::BleStatus::Connected) return false;
    if (!g_write_char) return false;
    // remember the moment we asked for headlight-off, so the parser can
    // tell our request apart from the bike's auto-off-at-sleep echo.
    if (strstr(cmd, "C-1-5") != nullptr) g_user_headlight_off_ms = millis();
    size_t n = strlen(cmd);
    return g_write_char->writeValue((uint8_t*)cmd, n, false);
}

void ble_forget_bike() {
    Serial.println("[BLE] forget_bike() — wiping bonds and address");
    if (g_client && g_client->isConnected()) {
        g_client->disconnect();
    }
    NimBLEDevice::deleteAllBonds();
    g_have_addr      = false;
    g_state.bonded   = false;
    g_state.ble      = BikeState::BleStatus::Idle;
    g_backoff_ms     = BLE_RECONNECT_BACKOFF_MIN_MS;
    g_next_attempt_ms = millis() + 500;
}
