# Reevo Bike BLE Protocol — Reverse Engineering Findings

Extracted from: `libapp.so` (Dart native code), `reevo.apk` DEX bytecode,
and live BLE capture via `reevo_repl.py` against a production Reevo unit.

---

## BLE Service & Characteristics

| Role | UUID |
|------|------|
| **ISSC Transparent UART Service** | `49535343-fe7d-4ae5-8fa9-9fafd205e455` |
| **Notify** (bike → phone) | `49535343-1e4d-4bd9-ba61-23c647249616` |
| **Write** (phone → bike) | `49535343-8841-43f4-a8d4-ecbe34729bb3` |
| **Flow Control** | `49535343-4c8a-39b3-2f49-511cff073b7e` |
| **PIN Code Change** | `3EB685DB-65F9-4CF6-A03A-E3EF65729F3D` |
| **Fingerprint / Auth** | `FDD39AD0-238F-46AF-ADB4-6C85480369C7` |

The bike requires **BLE bonding/pairing** before any characteristic access.
The factory-default RN4870 passkey is **`111111`**, which works on
never-paired bikes. **The original Reevo app rotated the passkey on first
bond**, so any bike that's been paired before has a different value. To
recover an unknown passkey, tap the BLE module's debug UART (TX + GND) at
115200 baud and watch what it prints during a bonding attempt — the module
echoes the expected passkey in plain text. macOS `bleak` couldn't get past
the encrypted-subscribe step; NimBLE on ESP32-S3 works once the passkey
is correct.

---

## Line format (both directions)

All traffic on the Notify/Write characteristics is **ASCII** lines.

- **Outbound (write):** `0:C-<group>-<id>[,<value>]@`
- **Inbound (notify):** `<seq>:R-<group>-<id>,<value>` (no terminator)

The leading `0:` is constant on writes. Inbound `<seq>` is a sequence
counter that wraps; safe to ignore.

---

## Decoded commands (phone → bike)

Confirmed by toggling and watching the corresponding `R-1-*` register echo.

| Command | Action | R-* echo |
|---|---|---|
| `0:C-1-2@` | Engage lock (kickstand must be down) | `R-1-3 → 1` |
| `0:C-1-3@` | Unlock | `R-1-3 → 0` |
| `0:C-1-4@` | Headlight ON | `R-1-4 → 1` |
| `0:C-1-5@` | Headlight OFF | `R-1-4 → 0` |
| `0:C-1-10@` | Assist level UP (pasIncrease) | `R-1-10 += 1` |
| `0:C-1-11@` | Assist level DOWN (pasDecrease) | `R-1-10 -= 1` |
| `0:C-1-6@`  | Right turn signal ON  | `R-1-5 → 1` |
| `0:C-1-7@`  | Right turn signal OFF | `R-1-5 → 0` |
| `0:C-1-8@`  | Left turn signal ON   | `R-1-6 → 1` |
| `0:C-1-9@`  | Left turn signal OFF  | `R-1-6 → 0` |
| `0:C-1-16@` | Badge light ON (the EL strip on the head badge) | *no echo* |
| `0:C-1-17@` | Badge light OFF | *no echo* |

> **Note on signals:** `C-1-6..9` are **not present** in the original
> Reevo app's command surface — the app only *reads* turn-signal state
> via `R-1-5/6` listeners and lets the rider toggle the signals via the
> bike's physical buttons. The commands work anyway on bike firmware
> revisions we've tested (verified empirically through the REPL). The
> dashboard's Brake Warn feature initially used them to alternate left
> and right while braking, but the bike's controller didn't tolerate the
> rapid back-and-forth well, so the feature now blinks the headlight
> and badge in unison instead. The signal commands are documented here
> in case they're useful later.

> **Note:** The Dart source labels `0:C-1-16/17` as `setThrottleOn/setThrottleOff`,
> but on the actual bike they drive the badge-light EL strip. Either the
> label in the APK is stale or this firmware revision repurposed the
> command. Trust the empirical behavior.

### Known but unmapped commands

These strings appear as outbound writes in the binary; their byte form
hasn't been confirmed against a live bike yet.

`C-0-1..6`, `C-1-12`, `C-2-2..5`, `C-2-7`. Probable mapping (from
proximity to Dart symbols): `startPasCalibration`,
`setMotionDetection On/Off`, `setGeofencingMode On/Off`,
`turnOnGSM`, `unregisterAllFingerPrint`, `factoryResetReevo`,
`enableDebugMode`, `setBootLoaderMode`. **None look like GPS queries.**

---

## Decoded notifications (bike → phone)

### `R-1-*` — discrete state, value `0` or `1` (or small int)

| Reg | Field | Notes |
|---|---|---|
| 1 | `battery_pct` | Main battery, 0–100 |
| 2 | `battery2_pct` | Backup battery, 0–100 |
| 3 | `kickstand_locked` | 1 when bike is locked |
| 4 | `headlight_on` | **Also fires `→ 0` when the bike auto-sleeps** (the bike kills the headlight at sleep). Used by our firmware as the bike-sleep signal. |
| 5 | `right_signal` | |
| 6 | `left_signal` | |
| 7 | `front_brake` | |
| 8 | `rear_brake` | |
| 9 | `kickstand_down` | Physical kickstand position |
| 10 | `assist_level` | 0..4 (OFF / ECO / NORMAL / SPORT / TURBO) |
| 21 | `throttle_enabled` | Software throttle-enable bit |

Registers `R-1-11..20` were observed but always read 0 or noise during
capture — likely TBD.

### `R-2-1` — wheel-sensor stream (high rate during motion)

Format: `255,<wheel_pulse>,<odo_counter>`

- `255` is a constant (max-value marker, maybe).
- `wheel_pulse` is an **instantaneous wheel-rotation sensor reading**,
  *not* throttle position. It tracks how fast the wheel is currently
  spinning — climbs with motor speed under throttle, and also moves
  when the bike is being **wheeled by hand**. Empirically: 0 at rest,
  ~3 at walking pace, ~30 with throttle pinned.
- `odo_counter` is a monotonically-increasing wheel-tick counter
  (~4 ticks per wheel revolution).

Stops streaming when the wheel is stationary; firmware treats >1.5s of
silence as "speed = 0".

### `R-3-1` — undecoded

Appears as a string symbol in `libapp.so` but no decoded usage. Likely
auth/session metadata; almost certainly **not** GPS coordinates (see
GPS section below).

### `R-0-*` — undecoded

Appear in the binary, never observed during normal connected operation.

---

## GPS / Location — **not available over BLE**

Confirmed by APK analysis:

- The app has a dedicated **`NoGSMScreen`** that blocks the bike-tracking
  UI if the bike's GSM (cellular) module isn't active.
- The track screen subscribes to Firebase Firestore — see the string
  `"Start Listening bike from firebase"`. There's no BLE listener for
  any location-shaped data.
- Symbols like `bikeLatLng`, `latitude`, `longitude` exist, but they're
  populated from Firestore documents, not BLE notifications.

**Architecture:**
```
Bike GPS antenna → Bike GSM module → cellular → Reevo Firebase
                                                       │
                                                       ▼
                                                 phone app
```

Reevo's Firebase backend was shut down with the company, so even with a
working SIM and active cellular plan, the data would publish to a server
that doesn't exist. **The BLE protocol never carried GPS in the first
place** — there's no register or command to query.

---

## Sleep behavior

The Reevo **does not** disconnect BLE when it sleeps; the radio stays
up. The only reliable sleep-transition signal we've observed is `R-1-4 → 0`
(the bike auto-turns off the headlight at sleep). Our firmware filters
out user-initiated `C-1-5` writes within a 2s window to avoid false
positives.

If the headlight was already off when the bike sleeps, there is no
notification at all and the dashboard falls back to a notification-idle
timeout (default 60s, configurable).

---

## UART debug port

Two distinct UARTs on the bike:

| Port | Behavior |
|---|---|
| **PC console** (PC_TX / PC_RX pads on the mainboard) | Output-only. Bike boot log + periodic battery status. ANSI escape codes. |
| **BLE-module debug** (where the dashboard's UART tap is wired) | The BLE module's own diagnostic output. ASCII at 115200 baud. |

Both are **passive observation** — neither accepts commands. The dashboard's
Diagnostics page exposes whichever UART is wired through, sanitized to
printable ASCII.

---

## Security / Authentication

- Bike supports **1 admin + 5 user profiles** (`setAdminProfileUUID`,
  `setUserProfile1UUID..5UUID`)
- App uses a **6-digit PIN** for bike auth
- Default PIN is **not** `111111` (the BLE pairing passkey) — that's the
  RN4870 module's factory default. The bike-side app PIN is separate.
- App warns: "Not changing the default PIN code may lead to security
  vulnerabilities"
- Fingerprint sensor: up to 5 prints per bike
- `authChallenge` string suggests challenge-response, not implemented in
  this dashboard

---

## App architecture (for reference)

- Flutter app, Dart native code in `libapp.so`
- BLE via `flutter_blue`
- OTA firmware update via **YMODEM** over BLE UART
- Firebase backend (auth, storage, Firestore — now dead)
- Mapbox SDK for map display
