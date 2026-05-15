# Reevo Display

A custom dashboard for the **Beno Reevo** hubless ebike. The company abandoned the project
and the original phone app is dead — this project replaces the entire app with
an ESP32-S3 touchscreen that talks directly to the bike over BLE. No cloud, no
SIM, no account.

If you own a Reevo and a soldering iron and twenty bucks, you can have a
working dashboard again.

---

## What you get

- Live speedometer (green 7-segment), battery %, trip odometer with reset
- Manual control of headlight, badge light, kickstand lock, assist level
- Touch-PIN unlock when you lock the bike
- **Brake warn** — flashes headlight + badge in alternation when you brake
- **PAS auto-timeout** — drops pedal assist to 0 after 10 min idle so the
  bike never takes off when you push it through the garage
- **Master code recovery** if you forget your PIN
- Built-in **Wi-Fi access point + web console** for sending raw BLE commands,
  resetting your PIN, recoloring the screen, and viewing a full setup manual

---

## Hardware

### Display (required)

The default target is the **Freenove FNK0104B** ESP32-S3 board with 2.8″
capacitive touchscreen.

> Buy: <https://amzn.to/4dJlGMW>

Specs that matter:
- ESP32-S3 with 16 MB flash, 8 MB PSRAM
- 240×320 ILI9341 IPS panel
- FT6336U capacitive touch
- Native USB-C (programming + power)
- 4-pin UART header (5V, GND, TX, RX)

If you bought a different "CYD" board, see [Using a different display](#using-a-different-display) below.

### Power

The display takes 5V via USB-C. Three ways to power it:

1. **Recommended (clean):** Tap into the bike's **5V UART line on the
   mainboard** and run that to the 5V/GND pins of the display's UART header.
   This is what my (Seth's) build does — the screen comes on when the bike
   does and goes off when the battery is removed.
2. **USB-C battery brick** — easy, less clean.
3. Any other power source, using a cheap buck converter to step down to 3V

### BLE pairing — read this before you flash

The bike's BLE module (RN4870) ships with passkey `111111`. **That default
only works if the bike has never been paired with the original Reevo app.**
The first time the original app paired with your bike, it rotated the
passkey to a value only the app knew. You can't factory-reset that without
already being able to bond — chicken and egg.

So, in order of likelihood, here's how you actually find the passkey:

- **A.** You wrote it down when you first set the bike up. Rarest.
- **B.** It's stored somewhere on the device that originally paired. On
  macOS, *Keychain Access* sometimes has BLE pairing data (search for
  "Reevo" or the bike's MAC). iOS keeps it locked away inside the system.
  Worth a look, mileage will vary.
- **C.** Read it off the bike's BLE module debug UART. This sounds
  intimidating but it's actually how I recovered mine, and it's the
  reliable path for any bike that was ever paired with the original app.
  You would need to take the bike apart completely and remove the main
  board in the motor compartment. It's gnarly. It's scary. It's doable.
  Details below.

### Recovering the passkey via the UART (the real path for most bikes)

1. Solder two thin wires to the **TX** and **GND** pads on the bike's
   RN4870 BLE module. While you're at it, grab a 5V to power the screen.
   See [`docs/ble_solder_points.png`](docs/ble_solder_points.png)
   for the exact pads.
3. Run those wires to a USB-UART adapter (CP2102, FTDI, CH340 — any will
   do). Open a serial terminal at **115200 baud, 8N1, no flow control**.
4. With the terminal listening, attempt to pair with the bike from
   *anything* — your dashboard, **LightBlue** on iOS, **nRF Connect** on
   Android. Enter `111111` (or any 6-digit number — it'll fail). The pair
   attempt itself fails, but the bike's BLE module **prints the real
   expected passkey to the UART** when it sees the request. Wild but real.
5. Read the passkey off the terminal. It's a 6-digit number.
6. Punch it into the dashboard: open Settings → Command Prompt → Start AP,
   join the Wi-Fi, browser to `192.168.4.1/`, type:
   ```
   setblepin 234567
   ```
   (replace with the number you read). Then tap **Re-pair bike** under
   Bluetooth settings. Done.

**You do not need to do any of this on a new, never-paired Reevo** — the
default `111111` works there. But for anything secondhand, plan on the
UART tap.

---

## Quick install (macOS / Linux)

If you've never touched any of this before:

```bash
git clone https://github.com/setha-maker/Reevo-Hubless-E-Bike-Screen.git
cd Reevo-Hubless-E-Bike-Screen
./scripts/install.sh
```

The script installs Homebrew (if needed), Python 3, PlatformIO, and the
asset-converter dependencies, then builds and flashes the firmware. Plug
the ESP32-S3 board into USB-C **before** running it.

## Manual install

Already have a working dev environment? Install requirements yourself:

```bash
pip install platformio Pillow numpy qrcode
```

Then:

```bash
cd firmware
python3 tools/image_to_rgb565.py    # generates src/assets.h bitmaps
pio run -e reevo -t upload          # builds and flashes
```

---

## Configuration

The repo ships a public template:

> **[`firmware/include/user_config.example.h`](firmware/include/user_config.example.h)**

The install script copies it to `firmware/include/user_config.h` on first
run. That copy is **gitignored** — your personal values never leak into
the public repo. If you're doing the install manually:

```bash
cp firmware/include/user_config.example.h firmware/include/user_config.h
```

Then edit `user_config.h`. The defines:

| Define | Default | What it does |
|---|---|---|
| `USER_DEFAULT_UNLOCK_PIN` | `"1234"` | Initial unlock PIN; rider can change at runtime |
| `USER_MASTER_PIN` | *(commented out)* | Optional owner-recovery master code. Uncomment in your personal copy if you want one. |
| `USER_DEFAULT_BLE_PASSKEY` | `"111111"` | RN4870 factory default. **Only correct for never-paired bikes** — for any used Reevo, recover the real passkey via the UART tap (see above) and set it at runtime with `setblepin`. |
| `USER_AP_SSID` | `"ReevoConnect"` | Initial Wi-Fi network the dashboard broadcasts |
| `USER_AP_PSK` | `"reevorider"` | Initial hotspot password (min 8 chars) |
| `USER_SPLASH_TAGLINE` | `"world's worst ebike"` | Text under the Reevo logo on the boot splash |

### Things you almost never need to recompile for

- **Unlock PIN** — change at runtime via the web `lockreset` command.
- **Wi-Fi SSID + password** — change at runtime via the web `changewifi`
  command. New values persist in NVS and survive reboots.
- **BLE pairing passkey** — change at runtime via the web `setblepin XXXXXX`
  command (e.g., once you've recovered the real passkey via the UART tap).
- **Splash colors** — change at runtime via the web `bgcolor` / `speedcolor`
  commands.

The compile-time defaults above are just what a fresh, factory-new flash
shows on first boot.

### The optional master PIN

`USER_MASTER_PIN` is commented out in the public template — the master-code
feature is fully compiled out for public builds. If you want a personal "I
forgot my code" backstop, uncomment the line in your `user_config.h` (which
is gitignored) and put a 4-digit code only you know. Don't commit it.

---

## Using a different display

If you bought a different ESP32-S3 CYD board, edit:

> **[`firmware/include/displays/display_config.h`](firmware/include/displays/display_config.h)**

This is the single source of truth for everything display-related: pin
numbers, panel driver, panel rotation, color order, touch chip, SPI
frequency. Common alternative profiles (Sunton CYD, generic ST7789, etc.)
are listed as commented-out blocks at the bottom — uncomment the one that
matches and recompile.

Things that vary between boards:
- SPI pins (MOSI, MISO, SCLK, CS, DC, RST, BL)
- Touch I²C pins (SDA, SCL, INT, RST)
- Panel driver chip (ILI9341 / ST7789 / ILI9488)
- BGR vs RGB color order
- Color inversion
- Backlight active-high vs active-low
- Native panel size and rotation

If your board isn't in the list, find its schematic and fill in the values.

---

## First-time use

1. Power on the bike.
2. Power on the dashboard.
3. Wait ~10 seconds. The dashboard scans for any BLE device with "REEVO"
   in its name and tries to bond with the configured passkey (default
   `111111`).
   - **Bond succeeds → live telemetry appears.** You're done.
   - **Bond fails / dashboard sits on "scan"** → your bike's passkey has
     been rotated away from the factory default. Use the UART recovery
     procedure above to read the real passkey, then `setblepin XXXXXX`
     from the web command prompt and tap **Re-pair bike**.
4. The default unlock PIN is `1234`. The first time you lock, the numpad
   will ask for it. Change it with the web `lockreset` command.

After that, everything that needs configuring can be done from the
dashboard itself — no recompiling.

---

## Built-in help

The dashboard's web command prompt has three giant text dumps you can read
right from your phone after joining the AP:

| Command | Contents |
|---|---|
| `newreevosetup` | Full end-user manual with troubleshooting tree |
| `how` | Complete BLE protocol reference (every command & register) |
| `story` | Claude's narrative of how this firmware came to be |

To get there: tap the gear icon on the main screen → **Command Prompt** →
**Start AP** → join the SSID (or scan the QR) → open the URL in any
browser → tap the chip for the command you want.

---

## Troubleshooting (short version)

Detailed guide: type `newreevosetup` on the web prompt.

| Problem | First thing to try |
|---|---|
| BLE won't connect | Settings → Bluetooth → Reset Default Pincode → Re-pair bike |
| Forgot your unlock PIN | Type the master code (whatever you set `USER_MASTER_PIN` to) |
| Screen won't wake | Tap it. Then power-cycle USB-C. |
| PAS won't engage | Tap the PAS pill to raise assist level; the timeout may have dropped it |
| BLE bond fails / "scan" forever | The bike's passkey isn't 111111 anymore. Solder UART tap → trigger a pair attempt from any BLE app → read the real 6-digit passkey from the module's debug output → `setblepin XXXXXX` on the web prompt. See the BLE pairing section above. |

---

## Project layout

```
firmware/
  include/
    user_config.example.h    ← public template (committed)
    user_config.h            ← your personal copy (gitignored — install
                                 script makes it from the example)
    displays/
      display_config.h       ← edit me if your board isn't a Freenove FNK0104B
    config.h, pins.h         ← derived from display_config.h
    lgfx_board.h             ← LovyanGFX panel/touch config
  src/                       ← the actual firmware
  tools/
    image_to_rgb565.py       ← bakes splash, icons, QR into src/assets.h
  tools/
    image_to_rgb565.py       ← bakes splash, icons, QR into src/assets.h
    source_images/           ← source PNGs the converter reads
docs/
  PROTOCOL.md                ← full BLE protocol reverse-engineering notes
  ble_solder_points.png      ← reference for the RN4870 UART tap
scripts/
  install.sh                 ← macOS/Linux one-shot installer
```

---

## License

MIT. See [LICENSE](LICENSE).

This project is unaffiliated with Beno. Reevo is their trademark; the
hardware design is referenced only for interoperability after the company's
shutdown.

## Credits

Built by Seth Alvo, with assistance from Claude (Anthropic).
Special thanks to the prior Claude session that did the initial BLE
reverse-engineering, and to every Reevo owner who's still riding theirs.
