import sys
import struct
def parse_pkg(path, target):
    with open(path, 'rb') as f: data = f.read()
    pos = 0
    def read_uint32():
        nonlocal pos
        val = struct.unpack('<I', data[pos:pos+4])[0]
        pos += 4
        return val
    def read_str():
        nonlocal pos
        length = read_uint32()
        s = data[pos:pos+length]
        pos += length
        return s.decode('utf-8', errors='ignore').strip('\x00')
    header = read_str()
    count = read_uint32()
    entries = []
    for _ in range(count):
        name = read_str()
        offset = read_uint32()
        length = read_uint32()
        entries.append((name, offset, length))
    base_offset = pos
    for name, offset, length in entries:
        if name == target or name.endswith(target):
            out = data[base_offset + offset : base_offset + offset + length]
            print(f"Name: {name}")
            print(f"Format: {struct.unpack('<I', out[18:22])[0]}")
            print(f"TEXB: {out[46:54]}")
            print(f"ImageCount: {struct.unpack('<I', out[55:59])[0]}")
            print(f"int0: {struct.unpack('<I', out[59:63])[0]}")
            print(f"int1: {struct.unpack('<I', out[63:67])[0]}")
            print(f"int2: {struct.unpack('<I', out[67:71])[0]}")
            print(f"int3: {struct.unpack('<I', out[71:75])[0]}")
            print(f"int4: {struct.unpack('<I', out[75:79])[0]}")
            print(f"int5: {struct.unpack('<I', out[79:83])[0]}")
            print(f"int6: {struct.unpack('<I', out[83:87])[0]}")
            
parse_pkg(sys.argv[1], sys.argv[2])
