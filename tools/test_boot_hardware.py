#!/usr/bin/env python3
"""Opt-in SWD reset regression: require OS ticks and both RTCs to advance.

Run after flashing the matching ELF. This resets the attached LT213A, reads
memory without halting, and leaves the firmware running. No Flash writes.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, default=Path('bin/targets/lt213a/app/apps/opendisplay/opendisplay.elf'))
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.trials < 1:
        parser.error('--trials must be positive')
    symbols = {}
    for line in subprocess.check_output(['arm-none-eabi-nm', str(args.elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    probe = ['--chip', 'nRF51822_xxAB', '--protocol', 'swd', '--speed', '1000', '--non-interactive']

    def read(address):
        output = subprocess.check_output(['probe-rs', 'read', *probe, 'b32', hex(address), '1'], text=True)
        return int(output.split(':', 1)[1].strip(), 16)

    def snapshot():
        return {'os_ticks': read(symbols['g_os_time']),
                'rtc0': read(0x4000b504), 'rtc1': read(0x40011504),
                'pcsr': hex(read(0xe000101c))}

    rows = []
    for trial in range(args.trials):
        subprocess.run(['probe-rs', 'reset', *probe], check=True)
        started = time.monotonic()
        time.sleep(1)
        first = snapshot()
        time.sleep(1)
        second = snapshot()
        advances = {key: (second[key] - first[key]) & (0xffffffff if key == 'os_ticks' else 0xffffff)
                    for key in ('os_ticks', 'rtc0', 'rtc1')}
        elapsed = time.monotonic() - started
        # Reject a backwards jump from a reset as well as a stopped counter.
        limits = {'os_ticks': 128, 'rtc0': 32768, 'rtc1': 32768}
        row = {'trial': trial + 1, 'elapsed_s': elapsed,
               'before': first, 'after': second, 'advances': advances,
               'passed': all(0 < advances[key] < elapsed * rate * 2
                             for key, rate in limits.items())}
        rows.append(row)
        print(json.dumps(row), flush=True)
        if args.output:
            args.output.write_text(json.dumps(rows, indent=2))
        if not row['passed']:
            raise SystemExit('FAIL: OS ticks or RTC stopped or jumped after reset')
    print(f'PASS: {args.trials} resets, OS ticks and both RTCs advanced')


if __name__ == '__main__':
    main()
