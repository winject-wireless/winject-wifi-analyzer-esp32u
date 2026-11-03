#!/usr/bin/env bash
# Build (and optionally flash) ESP-SDR for the original ESP32 with the
# esp32-power-meter PWR command. The submodule stays untouched: it is copied to
# firmware/build/esp-sdr, patched there, and built.
#
#   firmware/build.sh                 # build
#   firmware/build.sh flash /dev/ttyUSB0
#
# Needs an ESP-IDF environment (source export.sh first). ESP-SDR pins its IDF
# commit in firmware-targets.json; set IDF_PATH to that checkout if the
# installed release does not build.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/esp-sdr"
work="$here/build/esp-sdr"

if [ ! -f "$src/main/targets/esp32/receiver.c" ]; then
    echo "firmware/esp-sdr is empty: git submodule update --init" >&2
    exit 1
fi
command -v idf.py >/dev/null || { echo "idf.py not found: source ESP-IDF export.sh" >&2; exit 1; }

rm -rf "$work"
mkdir -p "$(dirname "$work")"
if [ -z "$(ls -A "$src/components/esp-dsp" 2>/dev/null)" ]; then
    echo "esp-dsp missing: git submodule update --init --recursive" >&2
    exit 1
fi
mkdir -p "$work"
cp -a "$src/." "$work/"
find "$work" -name .git -prune -exec rm -rf {} +
cp "$here/power_meter/pwr_command.h" "$work/main/targets/esp32/"
for p in "$here"/patches/*.patch; do
    patch -d "$work" -p1 --no-backup-if-mismatch < "$p"
done

cd "$work"
idf.py -B build-esp32 -DIDF_TARGET=esp32 -DSDKCONFIG=build-esp32/sdkconfig \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build

if [ "${1:-}" = "flash" ]; then
    idf.py -B build-esp32 -p "${2:-/dev/ttyUSB0}" flash
fi
