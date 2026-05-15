// ----------------------------------------------------------------------------
//  ui.h — dashboard UI module.
//
//  Owns: the off-screen sprite buffer, the screen stack (main / settings /
//  numpad / uart), all drawing, and touch routing.  Reads g_state for
//  telemetry, calls ble_send_command() for outbound commands.
//
//  main.cpp just needs to call ui_setup() once and ui_loop() every frame,
//  forwarding any touch events through ui_on_touch().
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include "lgfx_board.h"

void ui_setup(LGFX* tft);

// Call frequently from main loop. Redraws when state changes or when the
// blink/animation timer ticks. Cheap when nothing changed.
void ui_loop();

// Called on a fresh touch-down event. Coordinates are in display pixels
// (post touch-transform). The current screen decides what to do.
void ui_on_touch(int x, int y);
