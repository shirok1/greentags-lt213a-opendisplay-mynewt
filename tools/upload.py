#!/usr/bin/env python3
"""Upload one 104x212 monochrome frame using OpenDisplay raw direct-write."""
import argparse
import asyncio
import struct
import zlib
from pathlib import Path

UUID = '00002446-0000-1000-8000-00805f9b34fb'
WIDTH, HEIGHT = 104, 212

def frame_data(image=None, raw=None, pattern=None):
    if raw:
        data = Path(raw).read_bytes()
    elif image:
        from PIL import Image
        with Image.open(image) as source:
            if source.size != (WIDTH, HEIGHT):
                raise ValueError('Image must be exactly 104x212 pixels')
            # Pillow mode 1 is MSB-first, 1=white, matching the panel.
            data = source.convert('1').tobytes()
    elif pattern == 'checkerboard':
        data = bytes(0xff if ((x // 2 + y // 16) % 2) else 0
                     for y in range(HEIGHT) for x in range(WIDTH // 8))
    elif pattern == 'white':
        data = bytes([0xff]) * (WIDTH // 8 * HEIGHT)
    else:
        raise ValueError('Select an image, raw frame, or test pattern')
    if len(data) != 2756:
        raise ValueError(f'Frame must be 2756 bytes, received {len(data)}')
    return data

async def transfer(client, char, frame, queue, compress=False):
    async def response(echo, timeout=40):
        packet = await asyncio.wait_for(queue.get(), timeout)
        if len(packet) < 2 or packet[0] != 0 or packet[1] != echo:
            raise RuntimeError(f'Expected ACK {echo:02x}, got {packet.hex()}')
        return packet

    async def command(op, payload=b''):
        await client.write_gatt_char(char, bytes([0, op]) + payload, response=True)
        return await response(op)

    version = await command(0x43)
    print(f'Firmware version response: {version.hex()}')
    stream = frame
    if compress:
        encoder = zlib.compressobj(wbits=9)
        stream = encoder.compress(frame) + encoder.flush()
        await command(0x70, struct.pack('<I', len(frame)))
    else:
        await command(0x70)
    chunk_size = min(230, client.mtu_size - 5)
    if chunk_size < 18:
        raise RuntimeError('Invalid ATT MTU')
    for offset in range(0, len(stream), chunk_size):
        await command(0x71, stream[offset:offset+chunk_size])
    await command(0x72, b'\x00')
    # The 0x72 ACK confirms reception only; wait for the actual panel result.
    await response(0x73)
    print('Panel refresh completed successfully.')

async def upload(args):
    from bleak import BleakClient, BleakScanner
    frame = frame_data(args.image, args.raw, args.pattern)
    wanted = args.device.casefold()
    device = await BleakScanner.find_device_by_filter(
        lambda dev, adv: wanted in {dev.address.casefold(), (adv.local_name or '').casefold()},
        timeout=15,
    )
    if device is None:
        raise RuntimeError(f'Device {args.device!r} not found')
    queue = asyncio.Queue()
    async with BleakClient(device) as client:
        service = client.services.get_service(UUID)
        char = service.get_characteristic(UUID) if service else None
        if char is None:
            raise RuntimeError('OpenDisplay service/characteristic missing')
        await client.start_notify(char, lambda sender, data: queue.put_nowait(bytes(data)))
        try:
            await transfer(client, char, frame, queue, args.compress)
        finally:
            await client.stop_notify(char)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device', required=True, help='Exact ODxxxxxx name, BLE address or macOS UUID')
    p.add_argument('--compress', action='store_true', help='Stream zlib with a 512-byte window')
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--image', help='104x212 PNG or other Pillow image')
    g.add_argument('--raw', help='2756 bytes, rows top to bottom, MSB first, 1=white')
    g.add_argument('--pattern', choices=['white', 'checkerboard'])
    args = p.parse_args()
    try:
        asyncio.run(upload(args))
    except (ValueError, RuntimeError, TimeoutError) as exc:
        p.exit(1, f'Upload failed: {exc}\n')
