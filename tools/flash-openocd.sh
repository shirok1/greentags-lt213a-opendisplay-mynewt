#!/bin/sh
# Flash the application only; preserve the two configuration slots.
set -eu
case "${1:-}" in
    -h|--help)
        echo "Usage: $0"
        echo "Flash the built LT213A HEX with OpenOCD / CMSIS-DAP over SWD at 1000 kHz."
        echo "Verifies the write and resets the target. Run tools/build.sh first."
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
command -v openocd >/dev/null 2>&1 || {
    echo "OpenOCD is not installed or is not on PATH." >&2
    exit 1
}
exec openocd -f tools/openocd-lt213a.cfg -c "program $firmware verify reset exit"
