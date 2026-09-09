#!/bin/sh
# Flash the application only; preserve the two configuration slots.
set -eu
case "${1:-}" in
    -h|--help)
        echo "Usage: $0"
        echo "Flash the built LT213A HEX with probe-rs over SWD at 1000 kHz."
        echo "Verifies the write and resets the target. Run tools/build.sh first."
        echo "If multiple probes are attached, select one with PROBE_RS_PROBE=VID:PID:SERIAL."
        exit 0
        ;;
esac
if [ "$#" -ne 0 ]; then
    echo "Usage: $0 (no arguments; use --help for details)" >&2
    exit 2
fi
cd "$(dirname "$0")/.."
firmware=bin/targets/lt213a/app/apps/opendisplay/opendisplay.hex
if [ ! -f "$firmware" ]; then
    echo "Firmware not found: $firmware. Run ./tools/build.sh first." >&2
    exit 1
fi
command -v probe-rs >/dev/null 2>&1 || {
    echo "probe-rs is not installed or is not on PATH." >&2
    exit 1
}
exec probe-rs download --chip nRF51822_xxAB --protocol swd --speed 1000 \
    --non-interactive --binary-format hex --verify --reset "$firmware"
