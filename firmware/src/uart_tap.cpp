// ----------------------------------------------------------------------------
//  uart_tap.cpp — passive listener on UART1, feeding the Diagnostics page.
//
//  Wired to the bike's BLE-module debug pads. This was originally going
//  to be a GPS UART, but the Reevo's GPS is internal and only ever
//  reaches the phone via the bike's GSM module → Firebase. Since that
//  cloud is defunct, GPS over BLE isn't a thing for us; the wire just
//  carries whatever the BLE-debug chatters about.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

#include "uart_tap.h"
#include "config.h"
#include "pins.h"

namespace {

HardwareSerial g_serial(1);    // ESP32-S3 UART1

constexpr int LINES        = 8;
constexpr int LINE_LEN     = 84;
char     g_lines[LINES][LINE_LEN] = {};
int      g_write_idx       = 0;
char     g_partial[LINE_LEN] = {};
int      g_partial_len     = 0;
uint32_t g_bytes_total     = 0;
uint32_t g_last_byte_ms    = 0;

void commit_partial() {
    if (g_partial_len == 0) return;
    g_partial[g_partial_len] = '\0';
    bool empty = true;
    for (int i = 0; i < g_partial_len; i++) {
        char c = g_partial[i];
        if (c != ' ' && c != '\t') { empty = false; break; }
    }
    if (!empty) {
        memcpy(g_lines[g_write_idx], g_partial, g_partial_len + 1);
        g_write_idx = (g_write_idx + 1) % LINES;
    }
    g_partial_len = 0;
}

}  // namespace

void uart_tap_setup() {
    g_serial.begin(UART_TAP_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
    Serial.printf("[uart] tap begin @ %d baud, rx=%d tx=%d\n",
                  UART_TAP_BAUD, PIN_UART_RX, PIN_UART_TX);
}

void uart_tap_loop() {
    while (g_serial.available()) {
        char c = (char)g_serial.read();
        g_bytes_total++;
        g_last_byte_ms = millis();

        if (c == '\r' || c == '\n') {
            commit_partial();
        } else if (g_partial_len < LINE_LEN - 1) {
            g_partial[g_partial_len++] = c;
        } else {
            // partial overflowed without a newline — commit and start fresh
            commit_partial();
            g_partial[g_partial_len++] = c;
        }
    }
}

uint32_t uart_bytes_received() { return g_bytes_total; }
uint32_t uart_last_byte_ms()   { return g_last_byte_ms; }
int      uart_recent_lines_count() { return LINES; }

const char* uart_recent_line(int age) {
    if (age < 0 || age >= LINES) return "";
    int idx = (g_write_idx - 1 - age + LINES * 2) % LINES;
    return g_lines[idx];
}

const char* uart_partial_line() {
    g_partial[g_partial_len] = '\0';
    return g_partial;
}
