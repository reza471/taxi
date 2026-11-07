#!/usr/bin/env bash
# One-command setup of everything the Zephyr app needs OUTSIDE this repository:
#   - west and the Python packages
#   - a Zephyr workspace (v4.2.0) with only the modules this app uses (incl. MCUboot)
#   - the WiFi radio blobs for the ESP32
#   - the Zephyr SDK (ESP32 compiler)
# Safe to run again: finished steps are skipped.
#
#   zephyr-app/tools/setup.sh                 # workspace in ~/zephyr-ws, SDK in ~/zephyr-sdk
#   ZWS=/data/ws ZSDK=/data/sdk zephyr-app/tools/setup.sh
#
# It does NOT create the OTA signing key (that key must be yours, offline, never in git).
# At the end it writes <workspace>/env.sh : `source` it before building.
set -euo pipefail

ZWS="${ZWS:-$HOME/zephyr-ws}"
ZSDK="${ZSDK:-$HOME/zephyr-sdk}"
ZEPHYR_TAG="${ZEPHYR_TAG:-v4.2.0}"
SDK_VER="${SDK_VER:-0.17.0}"
SDK_TOOLCHAIN="xtensa-espressif_esp32_zephyr-elf"
MODULES="hal_espressif mbedtls cmsis_6 picolibc littlefs mcuboot"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# ---------- 1. tools that must already exist ----------
say "checking tools"
for t in git python3 pip3 cmake ninja gcc curl tar xz; do
	command -v "$t" >/dev/null 2>&1 || die "'$t' is missing. Install it first (Ubuntu: sudo apt install git python3-pip cmake ninja-build gcc curl xz-utils)."
done
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' || die "Python 3.10 or newer is needed"

# ---------- 2. west and Python packages ----------
say "west and Python packages"
if ! command -v west >/dev/null 2>&1; then
	# 'pip install west' can fail while building docopt from source: use docopt-ng, then west without deps
	pip3 install -q docopt-ng python-dateutil "ruamel.yaml" pyyaml packaging colorama
	pip3 install -q --no-deps pykwalify west
fi
west --version
# build helpers of Zephyr + imgtool dependencies (MCUboot signing)
pip3 install -q pyelftools intelhex jsonschema pyserial anytree ply cryptography click cbor2 \
	|| echo "warning: some Python packages could not be installed"

# ---------- 3. Zephyr workspace ----------
say "Zephyr $ZEPHYR_TAG workspace in $ZWS"
if [ ! -d "$ZWS/.west" ]; then
	west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_TAG" "$ZWS"
else
	echo "workspace exists, skipping west init"
fi
cd "$ZWS/zephyr"
# only what this app needs (a full 'west update' is several GB)
# shellcheck disable=SC2086
west update --narrow --fetch-opt=--depth=1 $MODULES
west zephyr-export >/dev/null 2>&1 || true

# ---------- 4. WiFi radio blobs ----------
say "ESP32 WiFi blobs"
if ! west blobs fetch hal_espressif; then
	echo "west blobs fetch failed (github.com/raw may be blocked): trying raw.githubusercontent.com"
	west blobs list hal_espressif -f '{path}|{sha256}|{url}|{abspath}' | while IFS='|' read -r path sha url abs; do
		if [ -f "$abs" ] && [ "$(sha256sum "$abs" | cut -d' ' -f1)" = "$sha" ]; then continue; fi
		raw=$(echo "$url" | sed -E 's#https://github.com/([^/]+/[^/]+)/raw/(.*)#https://raw.githubusercontent.com/\1/\2#')
		mkdir -p "$(dirname "$abs")"
		curl -fsSL -m 180 -o "$abs" "$raw" || { echo "download failed: $raw"; exit 1; }
		[ "$(sha256sum "$abs" | cut -d' ' -f1)" = "$sha" ] || { echo "CHECKSUM MISMATCH: $path"; exit 1; }
	done
fi
west blobs list hal_espressif -f '{status}' | sort | uniq -c

# ---------- 5. Zephyr SDK (ESP32 compiler only) ----------
say "Zephyr SDK $SDK_VER in $ZSDK"
if [ ! -d "$ZSDK/zephyr-sdk-$SDK_VER/$SDK_TOOLCHAIN" ]; then
	mkdir -p "$ZSDK"
	arch="$(uname -m)"; os="$(uname -s | tr '[:upper:]' '[:lower:]')"
	file="zephyr-sdk-${SDK_VER}_${os}-${arch}_minimal.tar.xz"
	curl -fSL --retry 3 -o "$ZSDK/$file" "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/${file}"
	tar -C "$ZSDK" -xf "$ZSDK/$file"
	rm -f "$ZSDK/$file"
	"$ZSDK/zephyr-sdk-$SDK_VER/setup.sh" -t "$SDK_TOOLCHAIN" -h -c
else
	echo "SDK exists, skipping"
fi

# ---------- 6. environment file ----------
say "writing $ZWS/env.sh"
cat > "$ZWS/env.sh" <<ENV
# source this file, then build:   west build -b esp32_devkitc/esp32/procpu <repo>/zephyr-app
export ZEPHYR_BASE="$ZWS/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$ZSDK/zephyr-sdk-$SDK_VER"
# west only works inside the workspace: cd "$ZWS/zephyr" before "west build"
ENV

cat <<DONE

Done.
  source $ZWS/env.sh
  cd $ZWS/zephyr   # west needs to run inside the workspace (or give it -d <build dir> from anywhere in it)
  west build -b esp32_devkitc/esp32/procpu <path to this repo>/zephyr-app

OTA (needs your own signing key, make it ONCE and keep it offline, never in git):
  python3 $ZWS/bootloader/mcuboot/scripts/imgtool.py keygen -k taxi_ota_key.pem -t ecdsa-p256
  west build --sysbuild -b esp32_devkitc/esp32/procpu <repo>/zephyr-app -- -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\\"\$PWD/taxi_ota_key.pem\\"
DONE
