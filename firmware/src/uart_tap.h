// ----------------------------------------------------------------------------
//  uart_tap.h — passive UART listener wired to the bike's BLE-module
//  debug port. We never transmit; the bike chatters and we log.
//
//  Pins are PIN_UART_RX / PIN_UART_TX (43/44) on the Freenove header.
//  Default baud is configurable below — the original Reevo BLE module
//  emits ASCII at 115200 (matches what CoolTerm shows).
//
//  The captured bytes are exposed two ways: a recent-line ring (newline-
//  delimited) and a live "partial" buffer for streams without newlines.
//  Both are read by the Diagnostics screen.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

void uart_tap_setup();
void uart_tap_loop();

// Total bytes received since boot. If zero after a few seconds, the
// connector isn't wired through (or the bike isn't sending anything yet).
uint32_t uart_bytes_received();

// millis() of the most recent byte received, or 0 if none yet.
uint32_t uart_last_byte_ms();

// Most-recent received lines (age 0 = newest).
const char* uart_recent_line(int age);
int  uart_recent_lines_count();

// Bytes received since the last newline. The Diagnostics screen renders
// this as the live "current" line, so a stream that never sends \n is
// still legible.
const char* uart_partial_line();
