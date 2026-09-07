#!/usr/bin/env python3
"""Check the linked image, including interrupt stack and runtime heap reserve."""
import argparse
import subprocess

p = argparse.ArgumentParser()
p.add_argument('elf', nargs='?', default='bin/targets/lt213a/app/apps/opendisplay/opendisplay.elf')
a = p.parse_args()
symbols = {}
for line in subprocess.check_output(['arm-none-eabi-nm', '-n', a.elf], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
sections = subprocess.check_output(['arm-none-eabi-objdump', '-h', a.elf], text=True).splitlines()
flash_end = 0
for i, line in enumerate(sections):
    fields = line.split()
    if len(fields) >= 7 and fields[0].isdigit() and i+1 < len(sections) and 'LOAD' in sections[i+1]:
        size, lma = int(fields[2],16), int(fields[4],16)
        if lma < 0x20000000:
            flash_end = max(flash_end, lma+size)
assert symbols['__isr_vector_start'] == 0, 'vector table must start at zero'
assert symbols['__StackTop'] == 0x20004000, 'wrong RAM size'
static = symbols['__HeapBase'] - 0x20000000
irq_stack = symbols['__StackTop'] - symbols['__StackLimit']
heap = symbols['__HeapLimit'] - symbols['__HeapBase']
assert flash_end <= 118*1024, 'flash overflow'
assert static + irq_stack + heap == 16*1024, 'RAM layout mismatch'
assert heap >= 2048, 'less than 2 KiB runtime heap reserve'
print(f'Flash application: {flash_end:,} / 120,832 bytes; config slots: 10,240 bytes')
print(f'RAM: {static:,} static (includes task stacks and BLE pools) + {irq_stack:,} IRQ stack')
print(f'Runtime heap available: {heap:,} bytes (GATT startup allocations consume part of this)')
