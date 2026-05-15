// ----------------------------------------------------------------------------
//  user_config.example.h — template for your personal `user_config.h`.
//
//  Copy this file to user_config.h (same directory) and edit your copy.
//  user_config.h is .gitignored, so your personal values never leak into
//  the public repo. The install script does the copy automatically on a
//  fresh clone:
//
//      cp firmware/include/user_config.example.h firmware/include/user_config.h
//
//  Runtime overrides:
//    Unlock PIN and Wi-Fi credentials can also be changed at runtime via
//    the web prompt (`lockreset`, `changewifi`). Runtime values persist in
//    NVS and override the compile-time defaults below.
// ----------------------------------------------------------------------------
#pragma once

// ===== Lock code =============================================================

// Default user unlock PIN (factory value). Must be exactly 4 digits (0-9).
// The rider can change this any time via the web `lockreset` command.
#define USER_DEFAULT_UNLOCK_PIN  "1234"

// ----- Optional owner-recovery master PIN -----------------------------------
// A second 4-digit code that ALWAYS unlocks, regardless of the user PIN.
// Useful as an "I forgot my code" backstop on your personal bike. Leave the
// line below commented out and the master feature is fully compiled out —
// safer for a public build, since anyone with the source would otherwise
// know the master.
//
// To enable: uncomment and pick a 4-digit number only you know.
//   #define USER_MASTER_PIN  "0000"

// ===== Bluetooth pairing =====================================================

// The bike's BLE module (RN4870) factory-default passkey. This is the
// correct value ONLY on a never-paired Reevo. The original Reevo app
// rotates the passkey on first bond, so any used bike has a different
// value. Recover the real passkey via the UART tap on the BLE module
// (see README), then set it at runtime with the web `setblepin XXXXXX`
// command — runtime value persists in NVS and overrides this default.
#define USER_DEFAULT_BLE_PASSKEY "111111"

// ===== Wi-Fi access point (initial values) ===================================
// These are the factory defaults for the dashboard's Wi-Fi hotspot. The
// rider can change them at runtime via the web `changewifi` command (NVS-
// persisted) without recompiling. Edit these values only if you want a
// different out-of-the-box default on a fresh flash.
#define USER_AP_SSID             "ReevoConnect"
#define USER_AP_PSK              "reevorider"          // min 8 chars

// ===== Splash screen =========================================================

// Tagline shown on boot and on wake from a locked sleep.
#define USER_SPLASH_TAGLINE      "world's worst ebike"

// ===== Display target ========================================================
//
// Default build targets the Freenove FNK0104B ESP32-S3 board:
//   https://amzn.to/4dJlGMW
//
// For a different CYD board, edit displays/display_config.h.
