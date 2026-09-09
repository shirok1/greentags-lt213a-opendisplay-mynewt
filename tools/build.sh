#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
python3 tools/patch_mynewt.py
python3 tools/generate_config.py
python3 tools/generate_version.py
newt build lt213a
python3 tools/check_size.py
elf=bin/targets/lt213a/app/apps/opendisplay/opendisplay.elf
arm-none-eabi-objcopy -O ihex "$elf" "${elf%.elf}.hex"
arm-none-eabi-objcopy -O binary "$elf" "${elf%.elf}.bin"
