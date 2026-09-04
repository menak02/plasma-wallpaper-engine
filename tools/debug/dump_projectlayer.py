import sys
import struct
import json

def parse_pkg(path, target):
    with open(path, 'rb') as f:
        data = f.read()

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
        if name.endswith(target):
            out = data[base_offset + offset : base_offset + offset + length]
            print(out.decode('utf-8', errors='ignore'))
            return

parse_pkg(sys.argv[1], sys.argv[2])
