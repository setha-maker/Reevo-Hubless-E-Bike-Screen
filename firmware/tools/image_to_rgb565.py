#!/usr/bin/env python3
"""
image_to_rgb565.py — convert PNGs to RGB565 PROGMEM C arrays for the
ESP32-S3 dashboard.

Currently generates `firmware/src/assets.h` with one bitmap:

  * splash_bitmap[320*240]    cropped landscape strip of splash_screen_2.png
                              with the Reevo wordmark centered. Saturation
                              and contrast are boosted before quantizing to
                              RGB565, then Floyd-Steinberg dithering hides
                              the gradient banding the panel would otherwise
                              show on the bike art.

Run from the firmware/ directory:
    python3 tools/image_to_rgb565.py

Re-run any time the source PNGs or sizing change. The output file is
committed; the firmware just includes it.
"""

from pathlib import Path
from PIL import Image, ImageEnhance, ImageDraw, ImageFont
import numpy as np
import qrcode

# ----- Wi-Fi credentials -----
# These MUST match USER_AP_SSID / USER_AP_PSK in firmware/include/user_config.h.
# If you change them there, change them here too and re-run this script so the
# on-screen QR code stays in sync.
WIFI_SSID = "ReevoConnect"
WIFI_PSK  = "reevorider"
QR_PX     = 116

# ----- paths -----
TOOLS_DIR    = Path(__file__).resolve().parent
IMG_DIR      = TOOLS_DIR / "source_images"
OUT_PATH     = TOOLS_DIR.parent / "src" / "assets.h"

# ----- constants matching firmware -----
DISPLAY_W = 320
DISPLAY_H = 240
HEADLIGHT_PX = 38   # right-column button bitmap

# ----- color shaping -----
SATURATION = 1.55   # 1.0 = unchanged; >1 pops magentas/blues
CONTRAST   = 1.20   # 1.0 = unchanged; >1 makes brights brighter
BRIGHTNESS = 1.05   # tiny lift, panel can read dim


def crop_landscape_centered(img: Image.Image, focus_y: int,
                            tw: int, th: int) -> Image.Image:
    """Crop a landscape strip of aspect tw:th from a portrait source,
    vertically centered on focus_y. Returns a tw×th-sized image."""
    sw, sh = img.size
    strip_h = round(sw * th / tw)
    top = focus_y - strip_h // 2
    top = max(0, min(top, sh - strip_h))
    cropped = img.crop((0, top, sw, top + strip_h))
    return cropped.resize((tw, th), Image.LANCZOS)


def color_shape(img: Image.Image) -> Image.Image:
    """Boost saturation + contrast so the RGB565 quantization doesn't
    leave the image looking flat."""
    img = ImageEnhance.Color(img).enhance(SATURATION)
    img = ImageEnhance.Contrast(img).enhance(CONTRAST)
    img = ImageEnhance.Brightness(img).enhance(BRIGHTNESS)
    return img


def to_rgb565_dithered(img: Image.Image) -> list[int]:
    """Floyd-Steinberg dither into RGB565. The bike art has long smooth
    gradients that quantize to ugly bands without it."""
    arr = np.asarray(img.convert("RGB"), dtype=np.float32).copy()  # h, w, 3
    h, w, _ = arr.shape
    for y in range(h):
        for x in range(w):
            old = arr[y, x].copy()
            # quantize each channel to its target bit-depth, scaled back to 0..255
            new = np.empty(3, dtype=np.float32)
            new[0] = np.clip(np.round(old[0] / 255.0 * 31), 0, 31) * (255 / 31)
            new[1] = np.clip(np.round(old[1] / 255.0 * 63), 0, 63) * (255 / 63)
            new[2] = np.clip(np.round(old[2] / 255.0 * 31), 0, 31) * (255 / 31)
            arr[y, x] = new
            err = old - new
            if x + 1 < w:           arr[y,   x+1] += err * (7/16)
            if y + 1 < h:
                if x - 1 >= 0:      arr[y+1, x-1] += err * (3/16)
                arr[y+1, x]                       += err * (5/16)
                if x + 1 < w:       arr[y+1, x+1] += err * (1/16)

    # pack to 16-bit RGB565
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    r = (arr[:, :, 0] >> 3).astype(np.uint16)
    g = (arr[:, :, 1] >> 2).astype(np.uint16)
    b = (arr[:, :, 2] >> 3).astype(np.uint16)
    rgb565 = (r << 11) | (g << 5) | b
    # LovyanGFX's pushImage() for raw uint16_t* doesn't byte-swap on
    # transfer the way fillRect/drawing primitives do. The ILI9341 wants
    # MSB-first on the wire, so we pre-swap here to match the panel.
    swapped = ((rgb565 & 0x00FF) << 8) | ((rgb565 & 0xFF00) >> 8)
    return swapped.flatten().tolist()


def emit_array(name: str, data: list[int], width: int, height: int) -> str:
    lines = [
        f"constexpr int {name.upper()}_W = {width};",
        f"constexpr int {name.upper()}_H = {height};",
        f"const uint16_t {name}[{width} * {height}] PROGMEM = {{",
    ]
    per_row = 16
    for i in range(0, len(data), per_row):
        chunk = ", ".join(f"0x{v:04X}" for v in data[i:i + per_row])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def fit_centered(img: Image.Image, tw: int, th: int,
                 bg: tuple[int, int, int]) -> Image.Image:
    """Letterbox the image onto a tw×th canvas keeping aspect, padded with bg."""
    canvas = Image.new("RGB", (tw, th), bg)
    sw, sh = img.size
    scale = min(tw / sw, th / sh)
    nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
    resized = img.resize((nw, nh), Image.LANCZOS)
    canvas.paste(resized, ((tw - nw) // 2, (th - nh) // 2))
    return canvas


def text_placeholder(tw: int, th: int,
                     line: str, sub: str = "") -> Image.Image:
    """When the source PNG is missing, render text so the firmware still
    has a goodbye_bitmap to show."""
    img = Image.new("RGB", (tw, th), (10, 10, 14))
    d   = ImageDraw.Draw(img)
    try:
        big   = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 36)
        small = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 14)
    except Exception:
        big = small = ImageFont.load_default()
    bb = d.textbbox((0, 0), line, font=big)
    d.text(((tw - (bb[2] - bb[0])) / 2, (th / 2) - (bb[3] - bb[1])),
           line, font=big, fill=(240, 240, 240))
    if sub:
        sb = d.textbbox((0, 0), sub, font=small)
        d.text(((tw - (sb[2] - sb[0])) / 2, (th / 2) + 12),
               sub, font=small, fill=(170, 170, 180))
    return img


def generate_wifi_qr(ssid: str, psk: str, size_px: int) -> Image.Image:
    """Returns a size_px × size_px RGB image of a WiFi-join QR code.
    Phones scan the WIFI:S:...;T:WPA;P:...;; format and offer to connect."""
    payload = f"WIFI:S:{ssid};T:WPA;P:{psk};;"
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=1, border=2,
    )
    qr.add_data(payload)
    qr.make(fit=True)
    img = qr.make_image(fill_color="white", back_color="black").convert("RGB")
    # Nearest-neighbor upscale so cells stay crisp at panel resolution.
    return img.resize((size_px, size_px), Image.NEAREST)


def shrink_composite(img: Image.Image, size: int,
                     bg: tuple[int, int, int]) -> Image.Image:
    """Resize a square RGBA icon to size×size, flatten transparent edges
    onto bg so it tiles cleanly against the dashboard background."""
    img = img.convert("RGBA").resize((size, size), Image.LANCZOS)
    canvas = Image.new("RGB", (size, size), bg)
    canvas.paste(img, (0, 0), mask=img.split()[3])
    return canvas


def main() -> None:
    splash_src    = IMG_DIR / "splash_screen_2.png"
    goodbye_src   = IMG_DIR / "worst_bike_ever.png"
    light_on_src  = IMG_DIR / "front_light_button_on.png"
    light_off_src = IMG_DIR / "front_light_off.png"
    if not splash_src.exists():
        raise SystemExit(f"missing source image: {splash_src}")

    print(f"[splash] reading {splash_src.name}")
    splash = Image.open(splash_src).convert("RGB")
    splash_cropped = crop_landscape_centered(
        splash, focus_y=1400, tw=DISPLAY_W, th=DISPLAY_H)
    print("[splash] boosting color, dithering to RGB565...")
    splash_shaped = color_shape(splash_cropped)
    splash_data   = to_rgb565_dithered(splash_shaped)

    if goodbye_src.exists():
        print(f"[goodbye] reading {goodbye_src.name}")
        goodbye = Image.open(goodbye_src).convert("RGB")
        goodbye_canvas = fit_centered(goodbye, DISPLAY_W, DISPLAY_H,
                                      bg=(255, 255, 255))
    else:
        print(f"[goodbye] {goodbye_src.name} not found — using text placeholder")
        goodbye_canvas = text_placeholder(DISPLAY_W, DISPLAY_H,
                                          "WORST. BIKE. EVER.",
                                          "(drop worst_bike_ever.png to replace)")
    print("[goodbye] dithering to RGB565...")
    goodbye_data = to_rgb565_dithered(goodbye_canvas)

    body = [
        "// ----------------------------------------------------------------------------",
        "// assets.h — PROGMEM RGB565 bitmaps. AUTO-GENERATED by",
        "// tools/image_to_rgb565.py from PNGs in reevo_apk/all_images/.",
        "// Re-run the script and recompile to refresh.",
        "// ----------------------------------------------------------------------------",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "#include <pgmspace.h>",
        "",
        emit_array("splash_bitmap",  splash_data,  DISPLAY_W, DISPLAY_H),
        emit_array("goodbye_bitmap", goodbye_data, DISPLAY_W, DISPLAY_H),
    ]

    # Right-column icon bitmaps. All composited onto the bike-screen BG
    # so transparent corners flatten cleanly into the dashboard.
    ICON_PX = HEADLIGHT_PX
    bg_rgb  = (15, 15, 20)   # matches Color::BG
    icons = (
        ("settings_bitmap",      IMG_DIR / "Settings-02-106.png"),
        ("headlight_on_bitmap",  light_on_src),
        ("headlight_off_bitmap", light_off_src),
        ("reset_bitmap",         IMG_DIR / "Command-Reset-144.png"),
    )
    for name, src in icons:
        if not src.exists():
            print(f"[icon] {src.name} missing — skipping {name}")
            continue
        print(f"[icon] reading {src.name}")
        icon = shrink_composite(Image.open(src), ICON_PX, bg=bg_rgb)
        data = to_rgb565_dithered(icon)
        body.append(emit_array(name, data, ICON_PX, ICON_PX))

    # Wi-Fi QR code for the Command Prompt page.
    print(f"[qr] generating WIFI QR for SSID={WIFI_SSID}")
    qr_img = generate_wifi_qr(WIFI_SSID, WIFI_PSK, QR_PX)
    qr_data = to_rgb565_dithered(qr_img)
    body.append(emit_array("wifi_qr_bitmap", qr_data, QR_PX, QR_PX))

    OUT_PATH.write_text("\n".join(body))
    print(f"[done] wrote {OUT_PATH}")
    print(f"       splash    : {DISPLAY_W}×{DISPLAY_H}  ({len(splash_data)*2} B)")
    print(f"       goodbye   : {DISPLAY_W}×{DISPLAY_H}  ({len(goodbye_data)*2} B)")


if __name__ == "__main__":
    main()
