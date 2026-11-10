import json
import struct
import sys
from pathlib import Path

exe, pdb = map(Path, sys.argv[1:3])
data = exe.read_bytes()
u16 = lambda offset: struct.unpack_from('<H', data, offset)[0]
u32 = lambda offset: struct.unpack_from('<I', data, offset)[0]
pe = u32(0x3c)
assert data[pe:pe+4] == b'PE\0\0'
optional = pe + 24
assert u16(optional) == 0x20b
sections = pe + 24 + u16(pe+20)


def offset(rva):
    for i in range(u16(pe+6)):
        s = sections + 40*i
        size, address, raw_size, raw = struct.unpack_from('<IIII', data, s+8)
        if address <= rva < address + max(size, raw_size):
            return raw + rva - address
    raise AssertionError(('unmapped RVA', rva))


debug_rva, debug_size = struct.unpack_from('<II', data, optional+112+6*8)
records = []
for i in range(debug_size//28):
    entry = offset(debug_rva) + i*28
    if u32(entry+12) == 2:
        record = u32(entry+24)
        if data[record:record+4] == b'RSDS':
            records.append((data[record+4:record+20], u32(record+20)))
assert len(records) == 1, 'Expected one executable PDB signature'

data = pdb.read_bytes()
assert data.startswith(b'Microsoft C/C++ MSF 7.00')
block_size, _, _, directory_size, _, block_map = struct.unpack_from('<IIIIII', data, 32)
count = (directory_size+block_size-1)//block_size
blocks = struct.unpack_from('<'+'I'*count, data, block_map*block_size)
directory = b''.join(data[b*block_size:(b+1)*block_size] for b in blocks)[:directory_size]
streams = struct.unpack_from('<I', directory)[0]
sizes = struct.unpack_from('<'+'I'*streams, directory, 4)
cursor = 4+streams*4
info = None
for i, size in enumerate(sizes):
    count = 0 if size == 0xffffffff else (size+block_size-1)//block_size
    blocks = struct.unpack_from('<'+'I'*count, directory, cursor)
    cursor += count*4
    if i == 1:
        info = b''.join(data[b*block_size:(b+1)*block_size] for b in blocks)[:size]
        break
assert info and len(info) >= 28
guid, age = records[0]
assert info[12:28] == guid and struct.unpack_from('<I', info, 8)[0] == age, 'EXE/PDB signature mismatch'
print(json.dumps({'status':'matched', 'executable':str(exe), 'pdb':str(pdb), 'guid_bytes':guid.hex(), 'age':age}))
