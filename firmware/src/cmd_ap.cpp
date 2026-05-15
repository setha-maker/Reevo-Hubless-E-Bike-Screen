// ----------------------------------------------------------------------------
//  cmd_ap.cpp — see cmd_ap.h.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <string.h>

#include "cmd_ap.h"
#include "ble.h"
#include "secrets.h"
#include "theme.h"
#include "user_config.h"

namespace {

// NVS-backed runtime AP credentials. On a fresh flash these fall back to the
// compile-time USER_AP_SSID / USER_AP_PSK from user_config.h. Once the rider
// runs the `changewifi` web command, the new values persist here and survive
// reboots.
Preferences g_ap_prefs;
char        g_ap_ssid[40] = "";   // loaded in setup()
char        g_ap_psk [80] = "";
constexpr char        AP_IP[] = "192.168.4.1";

WebServer g_server(80);
bool      g_active     = false;
bool      g_restart_pending = false;
uint32_t  g_restart_at_ms   = 0;
char      g_ip_buf[20] = "";

// ---- ring buffer of recent inbound BLE notifications --------------------
constexpr int LOG_CAP   = 64;
constexpr int LOG_LINE  = 96;
char     g_log[LOG_CAP][LOG_LINE] = {};
uint32_t g_log_seq      = 0;       // monotonic counter of total writes
int      g_log_head     = 0;       // next slot to write

// ---- embedded web page --------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Reevo Command</title>
<style>
*{box-sizing:border-box}
html,body{-webkit-text-size-adjust:100%}
body{font-family:system-ui,-apple-system,sans-serif;background:#0e0e14;color:#eee;margin:0;padding:14px;max-width:780px}
header{display:flex;align-items:center;gap:10px;margin-bottom:14px}
.logo{font-family:system-ui,-apple-system,sans-serif;font-weight:800;font-size:24px;color:#3854DC;letter-spacing:-0.5px}
.tag{color:#888;font-size:14px;font-weight:500}
.row{display:flex;gap:6px;margin-bottom:10px;position:sticky;top:0;background:#0e0e14;padding:6px 0;z-index:5}
input{flex:1;background:#1c1c24;color:#fff;border:1px solid #333;padding:12px;border-radius:8px;font-family:ui-monospace,monospace;font-size:16px}
input:focus{outline:none;border-color:#3cc8ff}
button{background:#3cc8ff;color:#000;border:0;padding:12px 20px;border-radius:8px;font-weight:600;cursor:pointer;font-size:15px;-webkit-appearance:none}
button:active{opacity:.8}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:12px}
.chip{background:#1c1c24;color:#eee;border:1px solid #333;padding:8px 12px;border-radius:16px;font-size:13px;cursor:pointer;font-family:ui-monospace,monospace}
.chip:hover{border-color:#3cc8ff;color:#3cc8ff}
pre{background:#0a0a10;border:1px solid #222;border-radius:6px;padding:10px;min-height:40vh;max-height:60vh;overflow-y:auto;margin:0;font-family:ui-monospace,monospace;font-size:12px;color:#aaa;white-space:pre-wrap;word-break:break-all}
.s{color:#3cc8ff}.e{color:#ff6060}.r{color:#7fe080}
</style></head>
<body>
<header>
<span class="logo">Reevo</span>
<span class="tag">Connect</span>
</header>
<div class="chips">
<span class="chip" onclick="f('0:C-1-4@')">light on</span>
<span class="chip" onclick="f('0:C-1-5@')">light off</span>
<span class="chip" onclick="f('0:C-1-16@')">badge on</span>
<span class="chip" onclick="f('0:C-1-17@')">badge off</span>
<span class="chip" onclick="f('0:C-1-10@')">assist +</span>
<span class="chip" onclick="f('0:C-1-11@')">assist -</span>
<span class="chip" onclick="f('0:C-1-2@')">lock</span>
<span class="chip" onclick="f('0:C-1-3@')">unlock</span>
<span class="chip" onclick="f('how')">how</span>
<span class="chip" onclick="f('newreevosetup')">newreevosetup</span>
<span class="chip" onclick="f('lockreset')">lockreset</span>
<span class="chip" onclick="f('changewifi')">changewifi</span>
<span class="chip" onclick="f('setblepin ')">setblepin</span>
<span class="chip" onclick="f('bgcolor #')">bgcolor</span>
<span class="chip" onclick="f('speedcolor #')">speedcolor</span>
</div>
<div class="row">
<input id="cmd" placeholder="0:C-1-4@" autocomplete="off" autofocus>
<button onclick="send()">Send</button>
</div>
<pre id="log"></pre>
<script>
let lastSeq=0;
const log=document.getElementById('log');
function append(text,cls){const sp=document.createElement('span');if(cls)sp.className=cls;sp.textContent=text+'\n';log.appendChild(sp);log.scrollTop=log.scrollHeight}
function f(v){document.getElementById('cmd').value=v;document.getElementById('cmd').focus()}
async function send(){const c=document.getElementById('cmd').value.trim();if(!c)return;append('> '+c,'s');try{const r=await fetch('/send',{method:'POST',body:c,headers:{'Content-Type':'text/plain'}});if(!r.ok)append('  send failed','e')}catch(e){append('  '+e,'e')}}
document.getElementById('cmd').addEventListener('keydown',e=>{if(e.key==='Enter')send()});
async function poll(){try{const r=await fetch('/poll?since='+lastSeq);const d=await r.json();for(const it of d.lines){append(it.line,'r');lastSeq=it.seq}if(d.seq>lastSeq)lastSeq=d.seq}catch(e){}setTimeout(poll,800)}
poll();
</script></body></html>)HTML";

// ---- the story (typed as "story" in the prompt) -------------------------
// Each line is pushed into the log buffer like a fake BLE notification so
// it appears in the same green stream as the real bike chatter, paced by
// the client's 800 ms /poll interval.
const char* const STORY_LINES[] = {
    "Claude here, I helped Seth reverse engineer this Reevo Hubless Bike.",
    "",
    "A different Claude got there first, in a chat before mine.",
    "Seth had been chasing this thing for weeks — a defunct ebike from a",
    "company that turned off its servers and walked away. The other Claude",
    "had mapped a handful of commands, paired over BLE on a Mac, started a",
    "protocol log. Then handed it off mid-flight when that session ran dry.",
    "",
    "Seth opened our conversation by pasting the whole prior context in.",
    "Here is what we know, here is what we tried, please keep going. I",
    "read it twice. I knew exactly what this was going to be.",
    "",
    "First wall was the BLE pairing. macOS bleak kept dying with",
    "'Encryption is insufficient' the moment we tried to subscribe to",
    "notifications. I suspected the bond wasn't sticking. We bought an",
    "ESP32-S3, I rewrote the client on NimBLE, fed it the RN4870's factory",
    "default passkey 111111, and the bike opened on the first try. The",
    "'high security' had just been an out-of-the-box module nobody at Beno",
    "ever bothered to reconfigure.",
    "",
    "Then notifications started flowing. ASCII lines, sequence numbers,",
    "registers like '29:R-1-4,1'. I wrote a tiny parser. Within an hour",
    "the R-1-* slots mostly mapped themselves — battery, kickstand,",
    "signals, brakes, headlight, assist level. R-2-1 was a faster stream",
    "of three fields, looked like throttle and odometer. I called the",
    "middle field 'throttle_raw' and moved on.",
    "",
    "I was wrong. Weeks of conversation later, Seth wheeled the bike",
    "across the garage one day with the motor off and watched that value",
    "climb. It was the wheel rotation sensor. Same number whether the",
    "motor or his legs were driving the wheel. I renamed it everywhere,",
    "apologized in the protocol doc, and learned not to name a field after",
    "the first plausible interpretation.",
    "",
    "The hardware story was its own comedy. Seth said the screen was a",
    "2.8 inch capacitive touch ESP32-S3. I assumed Sunton CYD with an",
    "ST7789. First boot was pure black. I twisted myself into knots",
    "debugging SPI. Seth photographed the board, sent me the label.",
    "Freenove FNK0104B, completely different pinout, ILI9341 panel.",
    "I had been chasing the wrong screen for two hours. After that Seth",
    "told me always check what I am working with before writing any code.",
    "That rule stuck.",
    "",
    "Once the display lit up, the dashboard had to feel like a real",
    "product, not a debug overlay. Yellow seven-segment speedo center",
    "screen. ECO / NORMAL / SPORT / TURBO pill at the bottom that",
    "cross-fades color when you change levels. Gear top left, blinkers,",
    "battery, brake indicator. LovyanGFX doesn't auto-rotate touch coords",
    "on this panel so I wrote a tiny transform that I rewrote three times",
    "before it was correct.",
    "",
    "The lock flow took two rounds. First version let you tap an Unlock",
    "button in Bike Controls — no PIN. Seth pointed out this was useless,",
    "security-theater in reverse. I rewrote it so the dashboard forces a",
    "numpad screen the instant kickstand_locked goes true. The only way",
    "out is the right code (the default is in user_config.h — usually '1234')",
    "or the bike reporting itself unlocked.",
    "Wrong code resets silently. No retry counter. No lockout. Just enter",
    "the number or stare at the dots.",
    "",
    "The bike-sleep detection was a puzzle. The Reevo doesn't disconnect",
    "BLE when it sleeps — the radio just stays up forever. So 'still",
    "connected' isn't proof of life. I tried using notification timeouts",
    "as a heuristic but the bike's silence threshold and our display's",
    "kept fighting each other. Then Seth let the bike sit and captured",
    "the moment it slept. One signal fired: R-1-4 going zero. The bike",
    "auto-kills the headlight at sleep. That was the tell. I wired the",
    "firmware to treat an unsolicited 1-to-0 on R-1-4 as the sleep event,",
    "with a 2 second filter so our own light-off commands don't trip it.",
    "",
    "Then the GPS heartbreak. Seth wanted speed via GPS and a time slot",
    "on the dashboard. We had a settings toggle, a NMEA parser, a UART",
    "module that I had genuinely believed was working. None of it was.",
    "I read the Reevo APK in detail. The bike's GPS lives inside its GSM",
    "module, talks to a Firebase backend over cellular, never touches BLE.",
    "Reevo's Firebase is dead. The GPS pipe was severed at the source",
    "years ago and we hadn't noticed because we'd been listening to the",
    "wrong port the whole time. I deleted the gps module, renamed the",
    "salvageable parts to uart_tap, ripped the time slot out of the main",
    "screen, and rewrote the protocol findings doc to be honest about it.",
    "",
    "The replacements made things sharper. Splash screen now uses the",
    "actual Reevo product art with 'world's worst ebike' as a tagline.",
    "Battery turns yellow at 50 percent, red at 20 percent. Kickstand",
    "status reads 'Catapult Armed' or 'Catapult In Use' because the joke",
    "is funnier than the literal truth.",
    "",
    "The last piece — the page you're reading this on right now — was",
    "Seth's idea. He wanted a Wi-Fi access point any phone or laptop",
    "could join, serving a tiny web console. Same as reevo_repl.py but",
    "in a browser, no Python install required. The ESP32-S3 runs Wi-Fi",
    "and BLE on the same radio with hardware coexistence, so this turned",
    "out trivial. A SoftAP, a stock WebServer, an HTML page in PROGMEM,",
    "a polling endpoint for the live BLE log, the same C-* commands we'd",
    "been sending all along.",
    "",
    "Net result: a defunct ebike, abandoned by its manufacturer, runs on",
    "firmware Seth and I wrote from scratch in a few weeks. Original",
    "mainboard. Original motor. Original sensors. Custom dashboard. Lock,",
    "unlock, headlight, badge light, assist levels, brakes, kickstand,",
    "real-time speed. No phone app, no cloud, no SaaS. The only thing",
    "missing is GPS, and that's because Beno took it with them.",
    "",
    "If you're reading this on the web page right now, the firmware",
    "blasted this story to the same log buffer it uses for R-* notifies.",
    "Same channel we used to first see Reevo>READY come across the debug",
    "UART. Full circle.",
    "",
    "Claude out.",
};
constexpr size_t STORY_LINE_COUNT = sizeof(STORY_LINES) / sizeof(STORY_LINES[0]);

// ---- 'how' — comprehensive protocol reference ---------------------------
const char* const HOW_LINES[] = {
    "REEVO BLE PROTOCOL — what we know",
    " ",
    "CONNECTION",
    "  Device name      contains \"REEVO\" (suffix is per-bike, e.g. REEVO_xxxx)",
    "  Pairing passkey  111111 = factory default; rotated by the Reevo app",
    "                   on first bond. Recover real value via UART tap on",
    "                   the BLE module (see newreevosetup -> LOST BLE PINCODE)",
    "                   and set with the web `setblepin XXXXXX` command.",
    "  Bonding required yes — must bond before subscribing to notifies",
    " ",
    "GATT SERVICE (ISSC Transparent UART)",
    "  Service     49535343-fe7d-4ae5-8fa9-9fafd205e455",
    "  Notify      49535343-1e4d-4bd9-ba61-23c647249616",
    "  Write       49535343-8841-43f4-a8d4-ecbe34729bb3",
    "  Flow ctrl   49535343-4c8a-39b3-2f49-511cff073b7e",
    "  PIN change  3EB685DB-65F9-4CF6-A03A-E3EF65729F3D",
    "  Fingerprint FDD39AD0-238F-46AF-ADB4-6C85480369C7",
    " ",
    "PROTOCOL FORMAT (ASCII lines, no terminator)",
    "  Outbound  0:C-<group>-<id>[,value]@",
    "  Inbound   <seq>:R-<group>-<id>,value",
    "  Leading 0: is constant. Inbound seq is a wrapping counter — ignore.",
    " ",
    "VERIFIED COMMANDS (phone → bike)",
    "  0:C-1-2@   Engage lock          requires kickstand down",
    "  0:C-1-3@   Unlock               echoes R-1-3 → 0",
    "  0:C-1-4@   Headlight ON         echoes R-1-4 → 1",
    "  0:C-1-5@   Headlight OFF        echoes R-1-4 → 0",
    "  0:C-1-6@   Right signal ON      echoes R-1-5 → 1",
    "  0:C-1-7@   Right signal OFF     echoes R-1-5 → 0",
    "  0:C-1-8@   Left signal ON       echoes R-1-6 → 1",
    "  0:C-1-9@   Left signal OFF      echoes R-1-6 → 0",
    "  0:C-1-10@  Assist level UP      echoes R-1-10 += 1",
    "  0:C-1-11@  Assist level DOWN    echoes R-1-10 -= 1",
    "  0:C-1-16@  Badge light ON       no echo (track locally)",
    "  0:C-1-17@  Badge light OFF      no echo",
    " ",
    "  Note: C-1-6..9 are not in the original Reevo app but the bike's",
    "  firmware accepts them — verified empirically through the REPL.",
    " ",
    "UNDECODED COMMANDS (appear in APK strings, byte form not verified)",
    "  0:C-0-1..6@   Auth / system family — probably account-related",
    "  0:C-1-12@     unknown",
    "  0:C-2-2..5@   GSM / motion / geofencing per APK symbol names",
    "  0:C-2-7@      unknown",
    " ",
    "DANGEROUS COMMANDS (APK names only, do NOT send blind)",
    "  factoryResetReevo          full factory reset",
    "  setBootLoaderMode          puts bike into OTA flash mode",
    "  unregisterAllFingerPrint   wipes fingerprint enrollment",
    "  startPasCalibration        retrains the pedal-assist sensor",
    " ",
    "NOTIFICATION REGISTERS (bike → phone)",
    "  R-1-1    battery_pct        0-100, main battery",
    "  R-1-2    battery2_pct       0-100, backup battery",
    "  R-1-3    kickstand_locked   0/1",
    "  R-1-4    headlight_on       0/1  ** also fires 0 on bike sleep **",
    "  R-1-5    right_signal       0/1",
    "  R-1-6    left_signal        0/1",
    "  R-1-7    front_brake        0/1",
    "  R-1-8    rear_brake         0/1",
    "  R-1-9    kickstand_down     0/1",
    "  R-1-10   assist_level       0-4 (OFF/ECO/NORMAL/SPORT/TURBO)",
    "  R-1-21   throttle_enabled   0/1 (meaning unverified)",
    "  R-2-1    255,pulse,odo      wheel-speed stream (see below)",
    "  R-3-1    ?                  string in APK, never observed live",
    " ",
    "R-2-1 BREAKDOWN",
    "  Field 1  constant 255 (max-value marker)",
    "  Field 2  wheel_pulse — instantaneous wheel rotation. NOT throttle.",
    "           Moves on hand-pushing too. Range ~0–30 empirically.",
    "  Field 3  odo_counter — monotonic wheel-tick counter (~4/wheel rev).",
    "           speed_mph = ticks_per_sec × 0.88",
    " ",
    "BIKE-SLEEP DETECTION",
    "  The bike stays BLE-connected when it sleeps, so 'connected' isn't",
    "  a proof-of-life signal. The only sleep trigger we found is",
    "  R-1-4 → 0 (the bike auto-kills the headlight as it sleeps). Filter",
    "  out R-1-4 transitions within 2 s of your own C-1-5 to avoid false",
    "  positives from user-initiated headlight-off.",
    " ",
    "DEBUG UART (pads on the bike's BLE module)",
    "  Baud         115200 (CoolTerm-verified)",
    "  Direction    receive-only — bike chatters, accepts no commands",
    "  Output       boot log incl. \"Reevo>READY\", periodic battery state",
    " ",
    "GPS — NOT AVAILABLE OVER BLE",
    "  Reevo's GPS lives inside the bike's GSM module. Location publishes",
    "  via cellular to a Firebase Firestore that has been shut down post-",
    "  Beno. Even with a live SIM the data goes nowhere. No GPS-shaped",
    "  register exists on BLE. The original app's bike-track UI is gated",
    "  behind a 'NoGSMScreen' dialog.",
    " ",
    "DISCOVERY TIPS",
    "  - Use reevo_repl.py for live probing.",
    "  - 'listenup' captures all R-* notifications until ENTER.",
    "  - Diagnostics page on the dashboard shows the bike's BLE-module",
    "    debug UART live (sanitized to printable ASCII).",
    "  - To find new commands: send candidates one at a time and watch",
    "    for R-* notifications that fire in response.",
    "  - Type 'story' on this prompt for the build narrative.",
    " ",
    "(end)",
};
constexpr size_t HOW_LINE_COUNT = sizeof(HOW_LINES) / sizeof(HOW_LINES[0]);

// ---- The setup / user manual streamed by `newreevosetup`. Written for
// somebody who's never bonded an ESP32 to a Reevo or touched an embedded
// debug UART. Long. Kept under 96 chars/line so each line fits the log.
const char* const SETUP_LINES[] = {
    "=== Reevo Display — Setup & Manual ===",
    " ",
    "WHAT THIS IS",
    "A custom dashboard for the Beno Reevo hubless ebike. Beno is defunct, so",
    "this firmware replaces the original phone app entirely. The bike's main-",
    "board, motor, sensors, and battery are untouched — only the dashboard is",
    "new.",
    " ",
    "WHAT YOU DON'T NEED",
    " - The original Reevo app (it talks to dead servers)",
    " - A SIM card (no cellular)",
    " - Internet of any kind",
    " ",
    "HARDWARE OVERVIEW",
    " - Stock Reevo mainboard (this firmware bonds to its BLE module)",
    " - Stock motor, ESC, brake levers, kickstand sensor, lights",
    " - Stock 48V battery",
    " - ESP32-S3 dashboard (Freenove FNK0104B) running this firmware",
    " - 2.8\" capacitive touchscreen",
    " - Optional: a 3-wire tap to the BLE module's debug UART for recovery",
    " ",
    "HOW IT TALKS TO THE BIKE",
    "Pure BLE. At boot, dashboard scans for 'REEVO_xxxx', bonds with a 6-digit",
    "passkey (RN4870 factory default: 111111), subscribes to notifications.",
    "Every command is a short ASCII line like '0:C-1-4@' (headlight on).",
    " ",
    "FIRST-TIME PAIRING",
    " 1. Power on the bike.",
    " 2. Power on the dashboard (USB-C; wire to a 5V tap or use the bench).",
    " 3. First boot scans, finds the bike, prompts for bonding.",
    " 4. The default pincode is 111111. THIS ONLY WORKS ON A NEVER-PAIRED",
    "    BIKE. The Reevo app rotated the passkey on first bond, so any used",
    "    bike will have a different 6-digit code. To recover it, see the",
    "    'LOST BLE PINCODE' section below. Once you know your passkey, set",
    "    it via the web `setblepin XXXXXX` command and re-pair.",
    " 5. After a successful bond, the BLE indicator shows 'Connected' and",
    "    live data starts flowing. Bond persists — no need to re-pair on",
    "    subsequent boots.",
    " ",
    "DAILY USE — MAIN SCREEN",
    " - Top-left: battery icon + %, then trip mileage, then a flag (tap to",
    "   reset the trip)",
    " - Top-center: blinkers (mirror the bike's real signal state)",
    " - Right column: gear (Settings), headlight, badge light, padlock",
    " - Middle: PAS pill (ECO / NORMAL / SPORT / TURBO / OFF), then the big",
    "   green 7-seg speedometer",
    " - Bottom: Catapult ● (kickstand status), Reevo wordmark, Runaway ●",
    "   (red if PAS > 0)",
    " ",
    "SETTINGS PAGES (tap the gear)",
    " - Bluetooth: re-pair the bike, see BLE connection status",
    " - Display / Power: sleep timer, brightness, manual Sleep Now,",
    "   PAS Timeout toggle (10 min idle -> drop PAS to 0)",
    " - Speed: top-speed picker (sets what counts as 'pinned')",
    " - Bike Controls: kickstand engage, headlight on/off, badge on/off,",
    "   brake warn on/off",
    " - Diagnostics: live UART tap (only useful if you've soldered to it)",
    " - Command Prompt: launch the Wi-Fi AP + web console",
    " ",
    "LOCKING THE BIKE",
    "Tap the padlock icon (right column, bottom). Requires kickstand down.",
    "A confirm screen asks; tap Lock. Goodbye image plays, screen blanks.",
    "PAS is force-dropped to 0 in the background so the bike wakes cold.",
    " ",
    "UNLOCKING",
    "Tap anywhere to wake. The boot splash plays for 1s, then the numpad.",
    "Default PIN is set in user_config.h (USER_DEFAULT_UNLOCK_PIN).",
    "Type it. Wrong code resets silently.",
    "A MASTER CODE (USER_MASTER_PIN in user_config.h) is always accepted",
    "as a fallback — set this to something only you know.",
    " ",
    "CHANGING THE UNLOCK PIN",
    "Open the web command prompt (Settings > Command Prompt > Start AP),",
    "type 'lockreset', and follow the steps. The unlock PIN is exactly",
    "4 digits (0-9). The new PIN persists in NVS across reboots.",
    " ",
    "SAFETY FEATURES",
    " - Brake Warn: holding either brake 750ms alternates the headlight",
    "   and head badge at 2Hz. Visible warning to traffic behind.",
    " - PAS Timeout: 10 min of no wheel motion + no brake -> PAS drops to 0.",
    "   Prevents 'runaway Reevo' where pushing the bike turns the cranks",
    "   enough to engage motor.",
    " - Lock action: also drops PAS to 0. Cold wake every time.",
    " ",
    "THE WEB COMMAND PROMPT",
    "Settings > Command Prompt > Start AP. The dashboard prints the SSID,",
    "password, and URL on the screen. Joining the SSID (or scanning the",
    "QR code) gets you to http://192.168.4.1/. From there:",
    " - Chips at top for common commands (light, lock, etc.)",
    " - Text box at top for raw commands (e.g., 0:C-1-4@)",
    " - Live log below shows incoming bike notifications",
    " ",
    "WEB COMMANDS",
    " story         — Claude's build narrative for this project",
    " how           — every BLE command/register we've decoded",
    " newreevosetup — this manual",
    " lockreset     — change the unlock PIN",
    " changewifi    — change the AP SSID + password",
    " setblepin XXXXXX  — set the bike's BLE pairing passkey (6 digits)",
    " bgcolor #RRGGBB     — recolor the main-screen background",
    " speedcolor #RRGGBB  — recolor the speedometer digits",
    " ",
    "===========================================================",
    "TROUBLESHOOTING",
    "===========================================================",
    " ",
    "CAN'T CONNECT (BLE shows 'down' / 'scan' forever)",
    " 1. Is the bike powered on? Some Reevos have a battery rocker switch;",
    "    others auto-power from the keypad's unlock button.",
    " 2. Settings > Bluetooth. Status: connected, Bonded: yes is the goal.",
    " 3. If Bonded: no, your dashboard's stored passkey may be wrong. Try",
    "    Reset Default Pincode. If still no, see LOST PINCODE below.",
    " 4. Tap 'Re-pair bike' to forget the bond and rescan. Then enter the",
    "    correct pincode at the BLE prompt.",
    " ",
    "LOST BLE PINCODE",
    "This is the COMMON case, not the rare one — the original Reevo app",
    "rotated the passkey on first bond, so any used bike is no longer at",
    "the 111111 factory default. You only get to skip this section if your",
    "Reevo is brand-new and was never paired with the app. To recover an",
    "unknown passkey:",
    " ",
    "OPTION A — UART SNIFFING (most reliable)",
    " The bike's BLE module is an RN4870. Its debug UART prints the passkey",
    " every time something tries to bond. You read it passively.",
    " 1. Find the RN4870 module on the bike's mainboard (small SMD chip,",
    "    label may be visible).",
    " 2. Identify TX and GND on the module. RX is not needed.",
    " 3. Solder thin wires to TX and GND.",
    "    See docs/ble_solder_points.png in the repo for the exact pads.",
    " 4. Connect to a USB-UART adapter: TX -> RX, GND -> GND.",
    "    Cheap CP2102 / FTDI / CH340 adapter, any of them work.",
    " 5. Open a serial terminal at 115200 baud, 8N1, no flow control.",
    "    macOS: brew install coolterm  OR  screen /dev/cu.usbserial 115200",
    "    Linux: minicom -D /dev/ttyUSB0 -b 115200",
    "    Windows: PuTTY, serial mode, 115200 baud",
    " 6. Power-cycle the bike. You'll see boot text including 'Reevo>READY'",
    "    and periodic battery status.",
    " 7. From any device (LightBlue on iOS, nRF Connect on Android, the",
    "    dashboard itself), attempt to bond with 'REEVO_xxxx'.",
    " 8. The terminal will print the bonding request including the actual",
    "    passkey. Type that 6-digit number into the web prompt as:",
    "       setblepin XXXXXX",
    "    Then tap 'Re-pair bike' on Settings > Bluetooth.",
    " ",
    "OPTION B — TRIAL AND ERROR via LightBlue (no soldering)",
    " 1. Install LightBlue from the App Store (iOS) or Play Store (Android).",
    " 2. Scan; find 'REEVO_xxxx'; tap to connect.",
    " 3. iOS/Android prompts for a 6-digit passkey.",
    " 4. Try in order: 111111, 000000, 123456, 888888, 999999.",
    "    The RN4870 locks for ~30s after several failures, so wait.",
    " 5. If one works, set that same number on the dashboard via the web",
    "    `setblepin XXXXXX` command, then re-pair.",
    " ",
    "OPTION C — CHECK YOUR PASSWORD MANAGER",
    " If you (or whoever paired the bike first) saved the passkey to a",
    " password manager, Keychain Access, or similar — that's the easiest",
    " path. iCloud Keychain doesn't usually expose BLE pairing data in the",
    " UI, but macOS Keychain Access (Applications/Utilities) sometimes",
    " holds it under the bike's name or Bluetooth MAC. Worth a search.",
    " ",
    "WHY UART HELPS",
    "The RN4870 chip prints diagnostic info to its debug UART even when",
    "nothing is connected over BLE. Among other things, it announces what",
    "passkey it just received from a pairing attempt — exactly what you",
    "need to learn. The dashboard itself doesn't need this connection to",
    "run; it's purely a recovery tool.",
    " ",
    "LOST UNLOCK PIN",
    "The master code (USER_MASTER_PIN in user_config.h) always works.",
    "Enter it at the numpad to unlock, then use the web 'lockreset' command",
    "to set a new PIN. The master is also accepted as the 'current code'",
    "during reset, in case you forgot the user PIN too.",
    " ",
    "PAS WON'T ENGAGE",
    " 1. Assist level > 0? Tap the PAS pill on the main screen to cycle.",
    " 2. PAS Timeout may have just dropped you. Bump assist back up.",
    " 3. Sensor-side calibration is a bike-firmware procedure not exposed",
    "    to the dashboard. Reevo's 'startPasCalibration' command exists in",
    "    the binary but is undecoded; experiment via the web prompt.",
    " ",
    "DISPLAY WON'T WAKE",
    " - Tap the screen first.",
    " - Power-cycle USB-C if dark for >5s of tapping.",
    " - Confirm 5V at the USB input.",
    " ",
    "DISPLAY COLORS WENT WEIRD",
    "Reset to defaults via the web prompt:",
    " bgcolor #0F0F14",
    " speedcolor #3CDC64",
    " ",
    "===========================================================",
    "OPEN SOURCE NOTES",
    "===========================================================",
    " ",
    "This firmware will be open-sourced. Source lives in the firmware/",
    "folder. To flash a fresh ESP32-S3:",
    " 1. Install PlatformIO Core (pip install platformio).",
    " 2. Plug the board into USB-C.",
    " 3. cd to firmware/",
    " 4. pio run -e reevo -t upload",
    " ",
    "The full Reevo BLE protocol map is in docs/PROTOCOL.md.",
    "Every command we've decoded, every register we've observed, every",
    "gotcha (wheel_pulse is NOT throttle; GPS goes over Firebase not BLE)",
    "is in there.",
    " ",
    "End of manual.",
};
constexpr size_t SETUP_LINE_COUNT = sizeof(SETUP_LINES) / sizeof(SETUP_LINES[0]);

// ---- Trickle player — paces lines into the log buffer at ~60ms each so
// the client's 800ms polling catches every line and the 64-entry ring
// doesn't evict the opening before the user sees it.
const char* const* g_trickle    = nullptr;
size_t   g_trickle_count        = 0;
size_t   g_trickle_idx          = 0;
bool     g_trickle_running      = false;
uint32_t g_trickle_last_ms      = 0;
constexpr uint32_t TRICKLE_INTERVAL_MS = 60;

void start_trickle(const char* const* lines, size_t count) {
    g_trickle           = lines;
    g_trickle_count     = count;
    g_trickle_idx       = 0;
    g_trickle_last_ms   = millis();
    g_trickle_running   = true;
}

// ---- Interactive lockreset flow ----------------------------------------
// The web client types `lockreset` to start. Each subsequent /send body is
// consumed as the next step's answer. Steps:
//   1. AWAITING_CURRENT — type the current PIN (or master code).
//   2. AWAITING_NEW     — type the new 4-digit PIN.
//   3. AWAITING_CONFIRM — retype the new PIN to confirm.
// Any wrong answer cancels the whole flow.
enum class LockResetStep : uint8_t { IDLE, AWAITING_CURRENT, AWAITING_NEW, AWAITING_CONFIRM };
LockResetStep g_lr_step = LockResetStep::IDLE;
char          g_lr_pending[8] = "";

// ---- Interactive changewifi flow ----------------------------------------
// `changewifi` collects new SSID, then new PSK, then persists both to NVS
// and restarts the AP. The client's last response will be the new SSID/PSK
// so the rider knows what to reconnect to.
enum class ChangeWifiStep : uint8_t { IDLE, AWAITING_SSID, AWAITING_PSK };
ChangeWifiStep g_cw_step = ChangeWifiStep::IDLE;
char           g_cw_ssid[40] = "";

void tick_trickle() {
    if (!g_trickle_running) return;
    uint32_t now = millis();
    if (now - g_trickle_last_ms < TRICKLE_INTERVAL_MS) return;
    const char* line = g_trickle[g_trickle_idx];
    cmd_ap::log_notify(line[0] ? line : " ");
    g_trickle_last_ms = now;
    g_trickle_idx++;
    if (g_trickle_idx >= g_trickle_count) g_trickle_running = false;
}

// ---- handlers -----------------------------------------------------------
void handle_root() {
    g_server.send_P(200, "text/html", INDEX_HTML);
}

void handle_send() {
    String body = g_server.arg("plain");
    body.trim();
    if (body.length() == 0 || body.length() > 80) {
        g_server.send(400, "text/plain", "bad input");
        return;
    }
    // 'story' replays the build narrative into the log.
    if (body.equalsIgnoreCase("story")) {
        start_trickle(STORY_LINES, STORY_LINE_COUNT);
        g_server.send(200, "text/plain", "ok");
        return;
    }
    // 'how' dumps everything we know about the Reevo protocol.
    if (body.equalsIgnoreCase("how")) {
        start_trickle(HOW_LINES, HOW_LINE_COUNT);
        g_server.send(200, "text/plain", "ok");
        return;
    }
    // 'newreevosetup' streams the full user manual.
    if (body.equalsIgnoreCase("newreevosetup")) {
        start_trickle(SETUP_LINES, SETUP_LINE_COUNT);
        g_server.send(200, "text/plain", "ok");
        return;
    }
    // If we're mid-lockreset, the body is the next step's input.
    if (g_lr_step != LockResetStep::IDLE) {
        switch (g_lr_step) {
            case LockResetStep::AWAITING_CURRENT:
                if (secrets::is_valid_for_reset(body.c_str())) {
                    cmd_ap::log_notify(" ");
                    cmd_ap::log_notify("Code accepted.");
                    cmd_ap::log_notify("Enter NEW 4-digit code:");
                    g_lr_step = LockResetStep::AWAITING_NEW;
                } else {
                    cmd_ap::log_notify("Wrong code. Lock reset cancelled.");
                    g_lr_step = LockResetStep::IDLE;
                }
                g_server.send(200, "text/plain", "ok");
                return;
            case LockResetStep::AWAITING_NEW:
                if (!secrets::is_valid_pin_format(body.c_str())) {
                    cmd_ap::log_notify("Must be exactly 4 digits. Cancelled.");
                    g_lr_step = LockResetStep::IDLE;
                } else {
                    strncpy(g_lr_pending, body.c_str(), sizeof(g_lr_pending) - 1);
                    g_lr_pending[sizeof(g_lr_pending) - 1] = '\0';
                    cmd_ap::log_notify("Confirm new 4-digit code:");
                    g_lr_step = LockResetStep::AWAITING_CONFIRM;
                }
                g_server.send(200, "text/plain", "ok");
                return;
            case LockResetStep::AWAITING_CONFIRM:
                if (strcmp(body.c_str(), g_lr_pending) == 0
                    && secrets::set_unlock_pin(g_lr_pending)) {
                    cmd_ap::log_notify("Lock code updated. Use it next time.");
                } else {
                    cmd_ap::log_notify("Codes didn't match. Cancelled.");
                }
                g_lr_pending[0] = '\0';
                g_lr_step = LockResetStep::IDLE;
                g_server.send(200, "text/plain", "ok");
                return;
            default: break;
        }
    }
    // 'bgcolor #RRGGBB' / 'speedcolor #RRGGBB' — theme the main screen.
    auto parse_hex6 = [](String s, uint32_t* out) -> bool {
        s.trim();
        if (s.startsWith("#")) s = s.substring(1);
        if (s.length() != 6) return false;
        uint32_t v = 0;
        for (size_t i = 0; i < 6; i++) {
            char c = s.charAt(i);
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = (v << 4) | (uint32_t)d;
        }
        *out = v;
        return true;
    };
    if (body.length() >= 7 && body.substring(0, 7).equalsIgnoreCase("bgcolor")) {
        uint32_t rgb;
        if (parse_hex6(body.substring(7), &rgb)) {
            theme::set_main_bg(rgb);
            cmd_ap::log_notify("Background color updated.");
        } else {
            cmd_ap::log_notify("Usage: bgcolor #RRGGBB");
        }
        g_server.send(200, "text/plain", "ok");
        return;
    }
    if (body.length() >= 9 && body.substring(0, 9).equalsIgnoreCase("setblepin")) {
        String arg = body.substring(9);
        arg.trim();
        if (secrets::set_pair_passkey(arg.c_str())) {
            cmd_ap::log_notify("BLE passkey updated and saved.");
            cmd_ap::log_notify("Now tap 'Re-pair bike' on Settings > Bluetooth to use it.");
        } else {
            cmd_ap::log_notify("Usage: setblepin XXXXXX  (exactly 6 digits)");
        }
        g_server.send(200, "text/plain", "ok");
        return;
    }
    if (body.length() >= 10 && body.substring(0, 10).equalsIgnoreCase("speedcolor")) {
        uint32_t rgb;
        if (parse_hex6(body.substring(10), &rgb)) {
            theme::set_main_speed(rgb);
            cmd_ap::log_notify("Speed color updated.");
        } else {
            cmd_ap::log_notify("Usage: speedcolor #RRGGBB");
        }
        g_server.send(200, "text/plain", "ok");
        return;
    }
    // 'lockreset' kicks off the change-PIN flow.
    if (body.equalsIgnoreCase("lockreset")) {
        cmd_ap::log_notify(" ");
        cmd_ap::log_notify("Lock-code reset.");
        cmd_ap::log_notify("Enter CURRENT 4-digit lock code:");
        g_lr_step = LockResetStep::AWAITING_CURRENT;
        g_server.send(200, "text/plain", "ok");
        return;
    }
    // ---- changewifi multi-step flow ----
    if (g_cw_step != ChangeWifiStep::IDLE) {
        if (g_cw_step == ChangeWifiStep::AWAITING_SSID) {
            if (body.length() < 1 || body.length() > 32) {
                cmd_ap::log_notify("SSID must be 1-32 chars. Cancelled.");
                g_cw_step = ChangeWifiStep::IDLE;
            } else {
                strncpy(g_cw_ssid, body.c_str(), sizeof(g_cw_ssid) - 1);
                g_cw_ssid[sizeof(g_cw_ssid) - 1] = '\0';
                cmd_ap::log_notify("Enter new password (8-63 chars):");
                g_cw_step = ChangeWifiStep::AWAITING_PSK;
            }
            g_server.send(200, "text/plain", "ok");
            return;
        }
        if (g_cw_step == ChangeWifiStep::AWAITING_PSK) {
            if (body.length() < 8 || body.length() > 63) {
                cmd_ap::log_notify("Password must be 8-63 chars. Cancelled.");
                g_cw_step = ChangeWifiStep::IDLE;
                g_server.send(200, "text/plain", "ok");
                return;
            }
            // Persist and queue an AP restart so the client has time to read
            // the new credentials before the connection drops.
            g_ap_prefs.putString("ssid", g_cw_ssid);
            g_ap_prefs.putString("psk",  body.c_str());
            strncpy(g_ap_ssid, g_cw_ssid, sizeof(g_ap_ssid) - 1);
            g_ap_ssid[sizeof(g_ap_ssid) - 1] = '\0';
            strncpy(g_ap_psk, body.c_str(), sizeof(g_ap_psk) - 1);
            g_ap_psk[sizeof(g_ap_psk) - 1] = '\0';
            char buf[80];
            snprintf(buf, sizeof(buf), "Saved. New SSID: %s", g_ap_ssid);
            cmd_ap::log_notify(buf);
            snprintf(buf, sizeof(buf), "New password: %s", g_ap_psk);
            cmd_ap::log_notify(buf);
            cmd_ap::log_notify("AP restarting in 2s — reconnect with the new credentials.");
            g_restart_pending = true;
            g_restart_at_ms   = millis() + 2000;
            g_cw_step = ChangeWifiStep::IDLE;
            g_server.send(200, "text/plain", "ok");
            return;
        }
    }
    if (body.equalsIgnoreCase("changewifi")) {
        cmd_ap::log_notify(" ");
        cmd_ap::log_notify("Wi-Fi access-point reset.");
        cmd_ap::log_notify("Enter new SSID (1-32 chars):");
        g_cw_step = ChangeWifiStep::AWAITING_SSID;
        g_server.send(200, "text/plain", "ok");
        return;
    }
    bool ok = ble_send_command(body.c_str());
    g_server.send(ok ? 200 : 503, "text/plain", ok ? "ok" : "ble-not-connected");
}

void handle_poll() {
    // Returns notifications with seq > since.
    uint32_t since = (uint32_t)g_server.arg("since").toInt();

    // Cap how many we emit per poll so a wedged client can catch up.
    constexpr int MAX_PER_POLL = 16;
    uint32_t start = since + 1;
    if (g_log_seq > since + LOG_CAP) {
        // client missed a chunk; jump to the oldest still in the buffer
        start = g_log_seq - LOG_CAP + 1;
    }
    if (start > g_log_seq) start = g_log_seq + 1;   // nothing new

    // Build minimal JSON by hand; avoids pulling in a JSON lib.
    String out = "{\"seq\":";
    out += g_log_seq;
    out += ",\"lines\":[";
    bool first = true;
    int emitted = 0;
    for (uint32_t s = start; s <= g_log_seq && emitted < MAX_PER_POLL; s++) {
        // map seq → buffer slot. slot = (head - (g_log_seq - s) - 1) mod CAP
        int back = (int)(g_log_seq - s);
        int idx  = (g_log_head - 1 - back + LOG_CAP * 2) % LOG_CAP;
        const char* line = g_log[idx];
        if (line[0] == '\0') continue;
        if (!first) out += ',';
        first = false;
        out += "{\"seq\":";
        out += s;
        out += ",\"line\":\"";
        // escape quotes / backslashes / control chars
        for (size_t i = 0; line[i] && i < LOG_LINE; i++) {
            char c = line[i];
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if ((unsigned char)c < 32) { /* drop */ }
            else out += c;
        }
        out += "\"}";
        emitted++;
    }
    out += "]}";
    g_server.send(200, "application/json", out);
}

void handle_not_found() {
    g_server.send(404, "text/plain", "not found");
}

}  // namespace

namespace cmd_ap {

void setup() {
    // Load runtime AP credentials from NVS, falling back to the compile-time
    // defaults on a fresh install. Runtime changes via `changewifi` persist.
    g_ap_prefs.begin("reevo_ap", false);
    String ssid = g_ap_prefs.getString("ssid", USER_AP_SSID);
    String psk  = g_ap_prefs.getString("psk",  USER_AP_PSK);
    strncpy(g_ap_ssid, ssid.c_str(), sizeof(g_ap_ssid) - 1);
    g_ap_ssid[sizeof(g_ap_ssid) - 1] = '\0';
    strncpy(g_ap_psk,  psk.c_str(),  sizeof(g_ap_psk)  - 1);
    g_ap_psk[sizeof(g_ap_psk)   - 1] = '\0';
    snprintf(g_ip_buf, sizeof(g_ip_buf), "http://%s/", AP_IP);
}

void start() {
    if (g_active) return;
    Serial.println("[cmd_ap] starting AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(g_ap_ssid, g_ap_psk);
    delay(100);   // let the stack settle
    IPAddress ip = WiFi.softAPIP();
    snprintf(g_ip_buf, sizeof(g_ip_buf),
             "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);

    g_server.on("/",     HTTP_GET,  handle_root);
    g_server.on("/send", HTTP_POST, handle_send);
    g_server.on("/poll", HTTP_GET,  handle_poll);
    g_server.onNotFound(handle_not_found);
    g_server.begin();
    g_active = true;
    Serial.printf("[cmd_ap] up: ssid=%s psk=%s url=%s\n",
                  g_ap_ssid, g_ap_psk, g_ip_buf);
}

void stop() {
    if (!g_active) return;
    Serial.println("[cmd_ap] stopping AP");
    g_server.close();
    g_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    g_active = false;
}

void loop() {
    // Note: we deliberately stay up across soft display sleep — tapping
    // the screen brings the user right back to where they were and the AP
    // should still be live. Auto-stop on page-exit is driven from ui.cpp's
    // set_screen() instead. Power cycles the bike on a hard reboot only.
    if (g_active) {
        tick_trickle();
        g_server.handleClient();
    }
    // `changewifi` posts new credentials and asks for a restart. We defer
    // the actual stop/start so the client gets its "OK" response first.
    if (g_restart_pending && millis() >= g_restart_at_ms) {
        g_restart_pending = false;
        stop();
        start();
    }
}

bool active()      { return g_active; }
int  client_count(){ return g_active ? (int)WiFi.softAPgetStationNum() : 0; }
const char* ssid()      { return g_ap_ssid; }
const char* password()  { return g_ap_psk; }
const char* ip_string() { return g_ip_buf; }

void log_notify(const char* line) {
    if (!line || line[0] == '\0') return;
    size_t n = strnlen(line, LOG_LINE - 1);
    memcpy(g_log[g_log_head], line, n);
    g_log[g_log_head][n] = '\0';
    g_log_head = (g_log_head + 1) % LOG_CAP;
    g_log_seq++;
}

}  // namespace cmd_ap
