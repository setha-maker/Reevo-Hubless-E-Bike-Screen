// ----------------------------------------------------------------------------
//  secrets.h — unlock PIN storage with a fixed master override.
//
//  The user's PIN persists across reboots in NVS (Preferences). A separate
//  master code is baked in so the owner can always get back in if they
//  forget their PIN.
// ----------------------------------------------------------------------------
#pragma once

namespace secrets {

void setup();

// Current user-settable PIN (NVS-backed). Default is USER_DEFAULT_UNLOCK_PIN
// from user_config.h on a fresh install.
const char* unlock_pin();

// Accepts the user PIN OR the master code.
bool is_valid_unlock(const char* code);

// Same set of valid codes the lock-reset flow accepts (current PIN or master).
bool is_valid_for_reset(const char* code);

// Persist a new user PIN. Must be 4 digits. Returns false if rejected.
bool set_unlock_pin(const char* new_pin);

// Validates "exactly 4 digits".
bool is_valid_pin_format(const char* code);

// ---- BLE pair passkey -----------------------------------------------------
// The 6-digit number the bike's BLE module expects during bonding. NVS-backed
// so it survives reboot; defaults to "111111" (the RN4870 factory default).
const char* pair_passkey();
bool        set_pair_passkey(const char* code);
void        reset_pair_passkey();
bool        is_valid_passkey_format(const char* code);

}  // namespace secrets
