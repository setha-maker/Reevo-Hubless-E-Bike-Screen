// ----------------------------------------------------------------------------
//  pas_timeout.h — anti-Runaway-Reevo safety:
//      after 10 min of no wheel movement and no brake input, force the
//      bike's pedal-assist level back to 0.
//
//  Background: the Reevo doesn't reset PAS on its own sleep cycle. So if
//  you parked it in ECO/SPORT/etc. and walk it out of the garage hours
//  later, the cranks turning against gear resistance is enough to engage
//  the motor — the bike takes off under you. This watchdog drops PAS to
//  off while the bike is unattended so the next ride starts cold.
//
//  Gated by g_state.pas_timeout_enabled (toggle on Display / Power page).
// ----------------------------------------------------------------------------
#pragma once

namespace pas_timeout {

void setup();
void loop();

// Trigger an immediate decrement sequence to drive assist_level to 0,
// bypassing the idle-timeout. Called from the lock action so the bike
// can't wake up still in ECO/SPORT and run away on the rider.
void force_to_zero();

}  // namespace pas_timeout
