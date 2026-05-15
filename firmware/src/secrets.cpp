// ----------------------------------------------------------------------------
//  secrets.cpp — see secrets.h.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "secrets.h"
#include "user_config.h"

namespace {

constexpr const char* DEFAULT_PIN     = USER_DEFAULT_UNLOCK_PIN;
constexpr const char* DEFAULT_PASSKEY = USER_DEFAULT_BLE_PASSKEY;
#ifdef USER_MASTER_PIN
constexpr const char* MASTER_PIN      = USER_MASTER_PIN;
#endif

Preferences g_prefs;
char        g_pin[8]     = USER_DEFAULT_UNLOCK_PIN;
char        g_passkey[8] = USER_DEFAULT_BLE_PASSKEY;

}  // namespace

namespace secrets {

void setup() {
    g_prefs.begin("reevo_sec", false);
    String stored = g_prefs.getString("pin", DEFAULT_PIN);
    strncpy(g_pin, stored.c_str(), sizeof(g_pin) - 1);
    g_pin[sizeof(g_pin) - 1] = '\0';
    String pk = g_prefs.getString("passkey", DEFAULT_PASSKEY);
    strncpy(g_passkey, pk.c_str(), sizeof(g_passkey) - 1);
    g_passkey[sizeof(g_passkey) - 1] = '\0';
    Serial.printf("[secrets] PIN loaded (%d digits), passkey loaded (%d digits)\n",
                  (int)strlen(g_pin), (int)strlen(g_passkey));
}

const char* unlock_pin() { return g_pin; }

bool is_valid_pin_format(const char* code) {
    if (!code) return false;
    if (strlen(code) != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return true;
}

bool is_valid_unlock(const char* code) {
    if (!code) return false;
    if (strcmp(code, g_pin) == 0) return true;
#ifdef USER_MASTER_PIN
    if (strcmp(code, MASTER_PIN) == 0) return true;
#endif
    return false;
}

bool is_valid_for_reset(const char* code) {
    return is_valid_unlock(code);
}

bool set_unlock_pin(const char* new_pin) {
    if (!is_valid_pin_format(new_pin)) return false;
#ifdef USER_MASTER_PIN
    if (strcmp(new_pin, MASTER_PIN) == 0) return false;  // can't shadow master
#endif
    strncpy(g_pin, new_pin, sizeof(g_pin) - 1);
    g_pin[sizeof(g_pin) - 1] = '\0';
    g_prefs.putString("pin", g_pin);
    Serial.println("[secrets] PIN updated");
    return true;
}

const char* pair_passkey() { return g_passkey; }

bool is_valid_passkey_format(const char* code) {
    if (!code) return false;
    if (strlen(code) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return true;
}

bool set_pair_passkey(const char* code) {
    if (!is_valid_passkey_format(code)) return false;
    strncpy(g_passkey, code, sizeof(g_passkey) - 1);
    g_passkey[sizeof(g_passkey) - 1] = '\0';
    g_prefs.putString("passkey", g_passkey);
    Serial.println("[secrets] BLE passkey updated");
    return true;
}

void reset_pair_passkey() { set_pair_passkey(DEFAULT_PASSKEY); }

}  // namespace secrets
