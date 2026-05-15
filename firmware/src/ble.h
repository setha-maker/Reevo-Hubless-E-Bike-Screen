// ----------------------------------------------------------------------------
//  ble.h — BLE client for the Reevo bike.
//
//  Public surface is intentionally tiny: setup once, call loop() from the
//  main loop, and the bike's R-* notifications land in g_state automatically.
//  Outbound commands go through ble_send_command(); the rest of the firmware
//  doesn't need to know NimBLE exists.
// ----------------------------------------------------------------------------
#pragma once

#include <stdbool.h>

// Initialize the NimBLE stack and start the scan/connect state machine.
// Call once from setup().
void ble_setup();

// Drive the BLE state machine. Call frequently from loop(). Cheap if
// nothing's pending.
void ble_loop();

// Send a text command like "0:C-1-4@". Returns true if the write was
// dispatched (no guarantee the bike will respond). False if not connected
// or the write failed at the BLE level.
bool ble_send_command(const char* cmd);

// Erase any stored BLE bond + saved bike address. The next loop iteration
// will start a fresh scan + bond from scratch. This is the "BT re-pair"
// failsafe wired to the settings page.
void ble_forget_bike();
