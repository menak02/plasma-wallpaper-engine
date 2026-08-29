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
            out_data = data[base_offset + offset : base_offset + offset + length]
            print(f"Texture Header Size: {len(out_data)}")
            print("Magic:", out_data[0:8])
            print("Magic2:", out_data[9:17])
            print("Format:", struct.unpack('<I', out_data[18:22])[0])
            print("Flags:", struct.unpack('<I', out_data[22:26])[0])
            print("Texture W/H:", struct.unpack('<I', out_data[26:30])[0], struct.unpack('<I', out_data[30:34])[0])
            print("Image W/H:", struct.unpack('<I', out_data[34:38])[0], struct.unpack('<I', out_data[38:42])[0])
            print("Body Magic:", out_data[46:54])
            print("ImageCount:", struct.unpack('<I', out_data[55:59])[0])
            fif = struct.unpack('<i', out_data[59:63])[0]
            print("FIF:", fif)
            if fif > 0:
                print("Comp Fmt/MipW/MipH:", struct.unpack('<I', out_data[63:67])[0], struct.unpack('<I', out_data[67:71])[0], struct.unpack('<I', out_data[71:75])[0])
            else:
                print("LZ4 compFormat/flags/MipW/MipH:", struct.unpack('<I', out_data[63:67])[0], struct.unpack('<I', out_data[67:71])[0], struct.unpack('<I', out_data[71:75])[0], struct.unpack('<I', out_data[75:79])[0])
            return
parse_pkg(sys.argv[1], sys.argv[2])
