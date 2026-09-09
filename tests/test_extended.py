"""Cross-language protocol/crypto tests against real C handlers and flash model."""
import ctypes as C
import binascii
import os
import random
import struct
import subprocess
import unittest
import zlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.cmac import CMAC
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

LIB = os.environ['OD_TEST_LIBRARY']
h = C.CDLL(LIB)
h.test_command.argtypes = [C.c_void_p, C.c_uint, C.c_uint]
h.test_reply.argtypes = [C.c_uint, C.c_void_p]
h.test_image.argtypes = [C.c_void_p]
h.test_config.argtypes = [C.c_void_p]

def command(data, mtu=247):
    n=h.test_command(data,len(data),mtu); result=[];out=C.create_string_buffer(6000)
    for i in range(n):
        size=h.test_reply(i,out);result.append(out.raw[:size])
    return result

def config():
    out=C.create_string_buffer(4096); n=h.test_config(out);return bytearray(out.raw[:n])

def crc_config(data):
    data=bytearray(data)
    c=binascii.crc_hqx(b'\0\0'+data[2:-2],0xffff)
    data[-2:]=struct.pack('<H',c);return bytes(data)

def cmac(key,data):
    c=CMAC(algorithms.AES(key));c.update(data);return c.finalize()

def image():
    out=C.create_string_buffer(6000);n=h.test_image(out);return out.raw[:n]

def compressed(data):
    c=zlib.compressobj(wbits=9);return c.compress(data)+c.flush()

class ProtocolTests(unittest.TestCase):
    def setUp(self): h.test_reset()
    def test_version_contains_build_sha_at_minimum_mtu(self):
        sha = subprocess.check_output(['git', 'rev-parse', 'HEAD']).strip()[:12]
        replies = command(b'\0\x43', 23)
        self.assertEqual(replies, [b'\0\x43\0\x02' + bytes([len(sha)]) + sha + b'\0'])
        self.assertLessEqual(len(replies[0]), 20)
    def full(self, data=b'\xff'*2756, zipped=False, etag=None):
        stream=compressed(data) if zipped else data
        start=b'\0\x70'+(struct.pack('<I',2756)+stream[:7] if zipped else b'')
        self.assertEqual(command(start),[b'\0\x70'])
        if zipped: stream=stream[7:]
        for i in range(0,len(stream),18): self.assertEqual(command(b'\0\x71'+stream[i:i+18]),[b'\0\x71'])
        end=b'\0\x72\0'+(struct.pack('>I',etag) if etag is not None else b'')
        self.assertEqual(command(end),[b'\0\x72',b'\0\x73'])
        self.assertEqual(image(),data)
    def test_compressed_and_raw(self):
        self.full();self.full(bytes(random.Random(7).randbytes(2756)),True)
        self.full(b'abcd'*689,True)
    def test_incomplete_checksum_overflow(self):
        data=compressed(b'\xff'*2756)
        self.assertEqual(command(b'\0\x70'+struct.pack('<I',2756)+data[:-1]),[b'\0\x70'])
        self.assertEqual(command(b'\0\x72\0'),[b'\xff\x72'])
        self.assertEqual(command(b'\0\x70'+struct.pack('<I',2756)+data[:-1]+bytes([data[-1]^1])),[b'\xff\x70'])
        command(b'\0\x70')
        for _ in range(11):command(b'\0\x71'+b'X'*230)
        self.assertEqual(command(b'\0\x71'+b'X'*230),[b'\xff\x71'])
        self.assertEqual(h.test_refreshes(),0)
    def test_partial_etag(self):
        self.full(etag=123)
        payload=b'\xaa\xbb\xcc\xdd'
        start=b'\0\x76\0'+struct.pack('>IIHHHH',123,456,0,0,8,2)
        self.assertEqual(command(start+payload),[b'\0\x76'])
        self.assertEqual(command(b'\0\x72\x02'),[b'\0\x72',b'\0\x73'])
        self.assertEqual(image(),payload)
        self.assertEqual(command(start),[b'\xff\x76\x01\0'])
    def test_partial_bounds(self):
        for rect,err in [((0,0,0,2),3),((104,0,8,2),3),((1,0,8,2),4)]:
            self.full(etag=1)
            start=b'\0\x76\0'+struct.pack('>IIHHHH',1,2,*rect)
            self.assertEqual(command(start),[bytes([255,0x76,err,0])])
    def test_partial_end_stream_errors(self):
        for zipped, payload, end in [
            (False, b'\xaa', b'\0\x72\x02'),
            (True, compressed(b'\xaa' * 4)[:-1], b'\0\x72\x02'),
            (False, b'\xaa' * 4, b'\0\x72\x03'),
            (False, b'\xaa' * 4, b'\0\x72'),
        ]:
            with self.subTest(zipped=zipped, end=end, size=len(payload)):
                self.full(etag=1)
                start=b'\0\x76'+bytes([zipped])+struct.pack('>IIHHHH',1,2,0,0,8,2)
                self.assertEqual(command(start+payload),[b'\0\x76'])
                refreshes=h.test_refreshes()
                self.assertEqual(command(end),[b'\xff\x72\x06\0'])
                self.assertEqual(h.test_refreshes(),refreshes)
                self.assertEqual(command(b'\0\x71x'),[b'\xff\x71'])
                self.assertEqual(command(start),[b'\xff\x76\x01\0'])
    def pipe(self,data,zipped=False,chunk=18):
        header=b'\0\x80'+struct.pack('<BBBBHI',1,int(zipped),2,2,244,len(data))
        ack=command(header)[0];self.assertEqual(ack[:5],b'\0\x80\x01\x02\x02')
        stream=compressed(data) if zipped else data
        pieces=[stream[i:i+chunk] for i in range(0,len(stream),chunk)]
        for i in range(0,len(pieces),2):
            indexes=[i+1,i] if i+1<len(pieces) else [i]
            for j in indexes: command(bytes([0,0x81,j%256])+pieces[j])
        result=command(b'\0\x82\0');self.assertEqual(result[-2:],[b'\0\x82',b'\0\x73'])
        self.assertEqual(image(),data)
    def test_pipe_reorder_wrap_and_compression(self):
        self.pipe(b'\xa5'*2756,chunk=7) # >256 chunks; sequence wrap
        self.pipe(bytes(random.Random(8).randbytes(2756)),True)
    def test_pipe_duplicate_hole_and_fatal(self):
        command(b'\0\x80'+struct.pack('<BBBBHI',1,0,2,1,244,2756))
        self.assertEqual(command(b'\0\x81\x01abc')[0],b'\0\x81\x01\0\0\0\0')
        self.assertEqual(command(b'\0\x81\x01abc')[0],b'\0\x81\x01\0\0\0\0')
        self.assertEqual(command(b'\0\x82\0')[-1],b'\xff\x82')
        r=command(b'\0\x81\x03abc');self.assertEqual(r[0][:3],b'\xff\x81\x04')
        self.assertEqual(command(b'\0\x81\x00abc'),[])
    def test_config_atomic(self):
        initial=config();changed=bytearray(initial);changed[29]=7;changed=crc_config(changed)
        for fail in range(8):
            h.test_reset();h.test_fail_flash(fail)
            self.assertEqual(command(b'\0\x41'+changed),[b'\xff\x41'])
            h.test_boot();self.assertEqual(config(),initial)
        h.test_fail_flash(-1)
        self.assertEqual(command(b'\0\x41'+changed),[b'\0\x41'])
        h.test_boot();self.assertEqual(config(),changed)
        invalid=bytearray(changed);invalid[-1]^=1
        self.assertEqual(command(b'\0\x41'+bytes(invalid)),[b'\xff\x41'])
        h.test_boot();self.assertEqual(config(),changed)
        self.assertEqual(command(b'\0\x45'),[b'\0\x45']);h.test_boot();self.assertEqual(config(),initial)
    def test_replace_committed_config_power_loss(self):
        for fail in range(8):
            h.test_reset();first=config();first[29]=3;first=crc_config(first)
            self.assertEqual(command(b'\0\x41'+first),[b'\0\x41'])
            second=bytearray(first);second[29]=9;second=crc_config(second)
            h.test_fail_flash(fail)
            self.assertEqual(command(b'\0\x41'+second),[b'\xff\x41'])
            h.test_boot();self.assertEqual(config(),first)
    def test_pipe_sack_mask(self):
        command(b'\0\x80'+struct.pack('<BBBBHI',1,0,2,2,244,2756))
        self.assertEqual(command(b'\0\x81\x01b'),[])
        self.assertEqual(command(b'\0\x81\x00a'),[b'\0\x81\x01\x01\0\0\0'])
        self.assertEqual(command(b'\0\x81\x03d'),[])
        self.assertEqual(command(b'\0\x81\x02c'),[b'\0\x81\x03\x07\0\0\0'])
        self.assertEqual(image(),b'abcd')
    def test_compressed_partial_and_pipe_partial(self):
        self.full(etag=1)
        data=b'\xff\x00\x55\xaa'
        start=b'\0\x76\x01'+struct.pack('>IIHHHH',1,2,0,0,8,2)
        self.assertEqual(command(start+compressed(data)),[b'\0\x76'])
        self.assertEqual(command(b'\0\x72\x02'),[b'\0\x72',b'\0\x73'])
        header=b'\0\x80'+struct.pack('<BBBBHI',1,2,2,1,244,4)+struct.pack('<IHHHH',2,0,0,8,2)
        self.assertEqual(command(header)[0][-1],3)
        command(b'\0\x81\0'+data)
        self.assertEqual(command(b'\0\x82\x02'+struct.pack('>I',3))[-2:],[b'\0\x82',b'\0\x73'])
        self.assertEqual(image(),data)
    def test_config_chunk_abort(self):
        initial=config()
        self.assertEqual(command(b'\0\x42x'),[b'\xff\x42'])
        self.assertEqual(command(b'\0\x41'+struct.pack('<H',250)+b'X'*200),[b'\0\x41'])
        h.test_reconnect()
        self.assertEqual(command(b'\0\x42'+b'X'*50),[b'\xff\x42']);self.assertEqual(config(),initial)
    def test_config_read_mtu(self):
        for mtu in [23,247]:
            replies=command(b'\0\x40',mtu);out=b''
            for i,r in enumerate(replies):
                self.assertLessEqual(len(r),mtu-3);self.assertEqual(int.from_bytes(r[2:4],'little'),i)
                out+=r[6:] if i==0 else r[4:]
            self.assertEqual(out,config())
    def test_hardware_unsupported(self):
        for op in [0x51,0x73,0x75,0x77]: self.assertEqual(command(bytes([0,op])),[bytes([255,op])])
        for op in [0x52,0x53]: self.assertEqual(command(bytes([0,op])),[bytes([255,op,0,0])])
        self.assertEqual(command(b'\0\x83\0'),[b'\xff\x83\xff\x02'])

class CryptoTests(unittest.TestCase):
    def setUp(self):
        h.test_reset();self.key=bytes(range(16))
        data=config();sec=bytearray(64);sec[0]=1;sec[1:17]=self.key;sec[17]=2
        data=data[:-2]+b'\0\x27'+sec+b'\0\0';self.config=crc_config(data)
        self.assertEqual(command(b'\0\x41'+self.config),[b'\0\x41'])
    def auth(self):
        r=command(b'\0\x50\0')[0];self.assertEqual(r[:3],b'\0\x50\0')
        server,device=r[3:19],r[19:];client=b'1234567890abcdef'
        proof=cmac(self.key,server+client+device)
        middle=cmac(self.key,b'OpenDisplay session\0'+device+client+server+b'\0\x80')
        aes=Cipher(algorithms.AES(self.key),modes.ECB()).encryptor()
        self.session=aes.update(b'\0'*7+b'\1'+middle[:8])+aes.finalize()
        self.sid=cmac(self.session,client+server)[:8]
        result=command(b'\0\x50'+client+proof)[0]
        self.assertEqual(result,b'\0\x50\0'+cmac(self.session,server+client+device));self.counter=0
    def envelope(self,op,payload=b''):
        nonce=self.sid+self.counter.to_bytes(8,'big');self.counter+=1;cmd=bytes([0,op])
        return cmd+nonce+AESCCM(self.session,tag_length=12).encrypt(nonce[3:],bytes([len(payload)])+payload,cmd)
    def decrypt(self,r):
        self.assertTrue(r[10]&128) # disjoint device nonce space
        plain=AESCCM(self.session,tag_length=12).decrypt(r[5:18],r[18:],r[:2])
        self.assertEqual(plain[0],len(plain)-1);return r[:2]+plain[1:]
    def test_gate_handshake_and_envelope(self):
        version=command(b'\0\x43',23)
        self.assertEqual(command(b'\0\x70'),[b'\xfe\x70'])
        self.auth()
        self.assertEqual(command(b'\0\x43',23),version)
        self.assertEqual(self.decrypt(command(self.envelope(0x70))[0]),b'\0\x70')
        r=command(self.envelope(0x40));data=b''
        for i,p in enumerate(r):
            p=self.decrypt(p);data+=p[6:] if i==0 else p[4:]
        self.assertEqual(data,self.config)
    def test_replay_and_tamper(self):
        self.auth();frame=self.envelope(0x70)
        self.assertEqual(self.decrypt(command(frame)[0]),b'\0\x70')
        self.assertEqual(command(frame),[b'\xff\x70'])
        self.assertEqual(command(b'\0\x70'),[b'\xfe\x70'])
        self.auth();frame=bytearray(self.envelope(0x70));frame[-1]^=1
        self.assertEqual(command(bytes(frame)),[b'\xff\x70'])
    def test_timeout_and_disconnect(self):
        self.auth();h.test_time(2100)
        self.assertEqual(command(self.envelope(0x70)),[b'\xfe\x70'])
        self.auth();h.test_reconnect()
        self.assertEqual(command(b'\0\x70'),[b'\xfe\x70'])
    def test_expired_challenge_and_rate_limit(self):
        command(b'\0\x50\0');h.test_time(31000)
        self.assertEqual(command(b'\0\x50'+b'X'*32),[b'\0\x50\xff'])
        for _ in range(9):h.test_reconnect();command(b'\0\x50\0')
        self.assertEqual(command(b'\0\x50\0'),[b'\0\x50\x04'])
    def test_bad_mac_cannot_retry(self):
        command(b'\0\x50\0')
        self.assertEqual(command(b'\0\x50'+b'X'*32),[b'\0\x50\x01'])
        self.assertEqual(command(b'\0\x50'+b'X'*32),[b'\0\x50\xff'])
    def test_tenth_challenge_can_complete(self):
        for _ in range(9):
            self.auth()
            h.test_reconnect()
        self.auth()  # The tenth proof must still be accepted.
        self.assertEqual(command(b'\0\x50\0'),[b'\0\x50\x04'])
        self.assertEqual(self.decrypt(command(self.envelope(0x70))[0]),b'\0\x70')
        h.test_time(60000)
        self.auth()
    def test_config_change_ack_uses_old_session_then_reauth(self):
        self.auth();new=bytearray(self.config);new[29]=8;new=crc_config(new)
        reply=command(self.envelope(0x41,new))[0]
        self.assertEqual(self.decrypt(reply),b'\0\x41')
        self.assertEqual(command(b'\0\x70'),[b'\xfe\x70'])
        self.assertEqual(config(),new)
    def test_low_mtu(self):
        self.assertEqual(command(b'\0\x50\0',23),[b'\0\x50\xff'])

if __name__=='__main__':unittest.main()
