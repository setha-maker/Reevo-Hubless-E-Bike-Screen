#!/usr/bin/env bash
# ----------------------------------------------------------------------------
#  Reevo Display — one-shot installer for macOS / Linux.
#
#  Run this from the project root after cloning:
#      ./scripts/install.sh
#
#  It will:
#    1. Make sure Python 3 is available (installs via Homebrew on macOS,
#       apt on Debian/Ubuntu).
#    2. Install PlatformIO Core and the asset-converter Python packages.
#    3. Regenerate the on-screen bitmaps from source PNGs.
#    4. Build + flash the firmware to a connected ESP32-S3.
#
#  Plug the board into USB-C BEFORE running this. If the script can't find
#  the device, it stops before flashing — you can re-run with the board
#  connected when you're ready.
# ----------------------------------------------------------------------------

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIRMWARE_DIR="$REPO_ROOT/firmware"

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }

bold "Reevo Display installer"
echo "Project root: $REPO_ROOT"

# ---- Detect OS --------------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
    Darwin) PLATFORM=macos ;;
    Linux)  PLATFORM=linux ;;
    *)
        red "Unsupported OS: $OS"
        echo "This script handles macOS and Linux. For Windows, use the manual"
        echo "install instructions in README.md."
        exit 1
        ;;
esac
echo "Platform: $PLATFORM"

# ---- Python 3 ---------------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
    bold "Installing Python 3..."
    if [ "$PLATFORM" = "macos" ]; then
        if ! command -v brew >/dev/null 2>&1; then
            bold "Installing Homebrew first..."
            /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        fi
        brew install python
    else
        sudo apt-get update
        sudo apt-get install -y python3 python3-pip python3-venv
    fi
fi
green "✓ Python 3: $(python3 --version)"

# ---- pip --------------------------------------------------------------------
if ! python3 -m pip --version >/dev/null 2>&1; then
    bold "Bootstrapping pip..."
    python3 -m ensurepip --user || true
fi

# ---- PlatformIO + asset deps ------------------------------------------------
bold "Installing PlatformIO Core and asset-converter dependencies..."
python3 -m pip install --user --upgrade \
    platformio \
    Pillow \
    numpy \
    qrcode

# PlatformIO's CLI usually lands in ~/.platformio/penv/bin or ~/Library/Python/.../bin.
PIO_BIN=""
for cand in \
    "$HOME/.platformio/penv/bin/pio" \
    "$HOME/Library/Python/3.9/bin/pio" \
    "$HOME/Library/Python/3.10/bin/pio" \
    "$HOME/Library/Python/3.11/bin/pio" \
    "$HOME/Library/Python/3.12/bin/pio" \
    "$HOME/.local/bin/pio" \
    "$(command -v pio || true)" ; do
    if [ -x "$cand" ]; then PIO_BIN="$cand"; break; fi
done
if [ -z "$PIO_BIN" ]; then
    red "Couldn't locate the 'pio' CLI after install. Add ~/.local/bin (or your"
    red "Python user bin) to PATH and re-run, or invoke pio manually."
    exit 1
fi
green "✓ PlatformIO: $PIO_BIN"

# ---- user_config.h (personal copy) ------------------------------------------
USER_CONFIG="$FIRMWARE_DIR/include/user_config.h"
EXAMPLE="$FIRMWARE_DIR/include/user_config.example.h"
if [ ! -f "$USER_CONFIG" ]; then
    bold "Creating user_config.h from the public template..."
    cp "$EXAMPLE" "$USER_CONFIG"
    green "✓ Created $USER_CONFIG"
    echo "  Edit this file to enable the owner-recovery master PIN (optional)"
    echo "  or to change the splash tagline. AP credentials + unlock PIN can"
    echo "  also be changed at runtime via the web 'changewifi' / 'lockreset'"
    echo "  commands without recompiling."
else
    green "✓ user_config.h already exists (keeping your personal copy)"
fi

# ---- Regenerate assets ------------------------------------------------------
bold "Regenerating on-screen bitmaps..."
( cd "$FIRMWARE_DIR" && python3 tools/image_to_rgb565.py )
green "✓ Assets generated"

# ---- Build + flash ----------------------------------------------------------
bold "Building firmware..."
( cd "$FIRMWARE_DIR" && "$PIO_BIN" run -e reevo )

bold "Flashing firmware (plug the ESP32-S3 into USB-C now if you haven't)..."
echo "If this fails with 'No serial data received', unplug and replug the"
echo "board, then re-run: cd firmware && $PIO_BIN run -e reevo -t upload"
( cd "$FIRMWARE_DIR" && "$PIO_BIN" run -e reevo -t upload )

green ""
green "✓ All done."
green "  Power on your bike, then your dashboard."
green "  The default unlock PIN is 1234 (change via the web 'lockreset' command)."
green ""
green "  Read the on-device manual: open the Wi-Fi prompt and type 'newreevosetup'."
