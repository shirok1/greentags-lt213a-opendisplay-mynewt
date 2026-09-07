import zlib,tempfile,subprocess,pathlib,random,sys
r=random.Random(9)
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d); raw=p/'raw'; enc=p/'enc'
 for payload in [b'\xff'*2756,bytes(r.randrange(256) for _ in range(2756)),bytes(r.randrange(8) for _ in range(2756)),bytes(range(256))*10]:
  raw.write_bytes(payload)
  for level,strategy in [(0,zlib.Z_DEFAULT_STRATEGY),(6,zlib.Z_DEFAULT_STRATEGY),(9,zlib.Z_FIXED),(6,zlib.Z_HUFFMAN_ONLY)]:
   z=zlib.compressobj(level,zlib.DEFLATED,9,8,strategy); data=z.compress(payload)+z.flush();enc.write_bytes(data)
   for chunk in [1,2,7,18,230,244]:
    assert subprocess.run([sys.argv[1],str(enc),str(raw),str(chunk)]).returncode==0,(level,strategy,chunk)
   for bad in [data[:-1],data+b'X',data[:-1]+bytes([data[-1]^1]),b'\x78\x9c'+data[2:]]:
    enc.write_bytes(bad)
    assert subprocess.run([sys.argv[1],str(enc),str(raw),'18']).returncode!=0
 print('96 zlib reference round trips and malformed stream checks passed')
