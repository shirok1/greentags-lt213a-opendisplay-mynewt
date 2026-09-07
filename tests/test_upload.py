import asyncio
import sys
import zlib
import unittest
sys.path.insert(0, 'tools')
from upload import frame_data, transfer

class Client:
    def __init__(self, queue, mtu, refresh=0x73):
        self.queue, self.mtu_size, self.refresh = queue, mtu, refresh
        self.written = bytearray()
    async def write_gatt_char(self, char, packet, response):
        assert response and len(packet) <= self.mtu_size - 3
        if packet[1] == 0x71:
            self.written.extend(packet[2:])
        await self.queue.put(bytes([0, packet[1]]))
        if packet[1] == 0x72:
            await self.queue.put(bytes([0, self.refresh]))

class UploadTests(unittest.IsolatedAsyncioTestCase):
    async def test_mtu_and_complete_refresh(self):
        for mtu in (23,247):
            q = asyncio.Queue(); c = Client(q,mtu)
            data = frame_data(pattern='checkerboard')
            await transfer(c,None,data,q)
            self.assertEqual(c.written,data)
            self.assertTrue(q.empty())
    async def test_compressed(self):
        q=asyncio.Queue(); c=Client(q,23)
        data=frame_data(pattern='checkerboard')
        await transfer(c,None,data,q,compress=True)
        self.assertEqual(zlib.decompress(c.written),data)
    async def test_refresh_timeout_is_not_success(self):
        q=asyncio.Queue(); c=Client(q,23,0x74)
        with self.assertRaises(RuntimeError):
            await transfer(c,None,frame_data(pattern='white'),q)
    def test_white(self):
        self.assertEqual(frame_data(pattern='white'),b'\xff'*2756)

if __name__ == '__main__':
    unittest.main()
