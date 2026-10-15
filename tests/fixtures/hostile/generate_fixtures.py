#!/usr/bin/env python3
"""Generate hostile-input fixtures for parser hardening tests.

Each fixture targets a specific finding from RED_TEAM_REPORT_2026-10-15.md.
The parser must reject/degrade on every one of them without crashing or
making oversized allocations:

- overflow-dims.tex   (C3):  DXT1 mip claiming 65536x65536 with a 16-byte
                             payload; width*height*4 wraps to 0 in 32 bits.
- texb1-uncapped.tex  (H2):  TEXB0001 entry with uncompSize=0xFFFFFFF0
                             (uncapped LZ4 allocation / negative int cast).
- huge-entry.pkg      (P1):  entries with 4 GiB length and wrapping
                             offset+length against a tiny file.
- seek-bomb-negative.mp4 (H5): ISO-BMFF box with extended size -1 so the
                             demuxer seeks before the buffer start.
- seek-bomb-oversize.mp4 (H5): ISO-BMFF box claiming a ~4 GiB size so the
                             demuxer seeks far past the buffer end.

Deterministic output; re-running overwrites the fixtures byte-identically.
"""

import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent


def u32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def tex_file(body_tag: bytes, mips: list[bytes]) -> bytes:
    out = b"TEXV0005\x00"
    out += b"TEXI0001\x00"
    out += u32(7)          # format = DXT1
    out += u32(0)          # flags
    out += u32(65536)      # textureWidth
    out += u32(65536)      # textureHeight
    out += u32(65536)      # width
    out += u32(65536)      # height
    out += u32(0)          # reserved
    out += body_tag + b"\x00"
    out += u32(len(mips))
    for mip in mips:
        out += mip
    return out


def main() -> None:
    # C3: one DXT1 mip, 65536x65536 declared, 16 bytes of payload. A valid
    # DXT1 4x4 block so the decoder reaches the pixel loops.
    overflow_mip = (
        u32(7)             # compFormat = DXT1
        + u32(65536)       # mipWidth
        + u32(65536)       # mipHeight
        + u32(0)           # compression = raw
        + u32(0)           # uncompSize (raw path)
        + u32(16)          # compSize
        + b"\x00" * 16     # one DXT1 block
    )
    (HERE / "overflow-dims.tex").write_bytes(tex_file(b"TEXB0001", [overflow_mip]))

    # H2: TEXB0001 LZ4 entry with an uncapped uncompressed size.
    uncapped_mip = (
        u32(0)             # compFormat
        + u32(4)           # mipWidth
        + u32(4)           # mipHeight
        + u32(1)           # compression = LZ4
        + u32(0xFFFFFFF0)  # uncompSize: ~4 GiB allocation pre-fix
        + u32(8)           # compSize
        + b"\x00" * 8      # garbage LZ4 stream
    )
    (HERE / "texb1-uncapped.tex").write_bytes(tex_file(b"TEXB0001", [uncapped_mip]))

    # PKG: header + 3 entries against a 4-byte payload section.
    pkg = u32(4) + b"PKGV"
    pkg += u32(3)  # fileCount
    for name, offset, length in (
        (b"small.txt", 0, 4),                    # valid
        (b"huge.bin", 0, 0xFFFFFFF0),            # 4 GiB length vs 4-byte file
        (b"wrap.bin", 0x80000000, 0x80000000),   # offset+length wraps to 0 in u32
    ):
        pkg += u32(len(name)) + name + u32(offset) + u32(length)
    pkg += b"DATA"  # payload section (m_baseOffset points here)
    (HERE / "huge-entry.pkg").write_bytes(pkg)

    ftyp = struct.pack(">I", 20) + b"ftyp" + b"isom" + struct.pack(">I", 0) + b"isom"

    # H5: extended-size box with size -1 -> seek before buffer start.
    bomb_negative = ftyp + struct.pack(">I", 1) + b"mdat" + struct.pack(">Q", 0xFFFFFFFFFFFFFFFF)
    (HERE / "seek-bomb-negative.mp4").write_bytes(bomb_negative)

    # H5: 32-bit box size 0xFFFFFFFF -> seek ~4 GiB past buffer end.
    bomb_oversize = ftyp + struct.pack(">I", 0xFFFFFFFF) + b"free"
    (HERE / "seek-bomb-oversize.mp4").write_bytes(bomb_oversize)

    # H5: structurally valid mp4 whose first sample sits at a hostile chunk
    # offset. mov_read_packet seeks there during avformat_find_stream_info,
    # which drives the AVIO seek callback with an out-of-range position.
    for name, use_co64, offset in (
        ("seek-bomb-stco.mp4", False, 0xFFFFFFFF),
        ("seek-bomb-co64.mp4", True, 0xFFFFFFFFFFFFFFFF),
    ):
        (HERE / name).write_bytes(bomb_chunk_offset(ftyp, use_co64, offset))

    for f in sorted(HERE.iterdir()):
        if f.suffix in {".tex", ".pkg", ".mp4"}:
            print(f"{f.name}: {f.stat().st_size} bytes")


def box(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", 8 + len(payload)) + kind + payload


def bomb_chunk_offset(ftyp: bytes, use_co64: bool, offset: int) -> bytes:
    def v0(kind: bytes, payload: bytes) -> bytes:
        return box(kind, b"\x00\x00\x00\x00" + payload)

    matrix = struct.pack(">9I", 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000)

    mvhd = v0(b"mvhd",
              struct.pack(">IIIII", 0, 0, 1000, 1000, 0x00010000) +
              struct.pack(">H", 0x0100) + b"\x00" * 10 + matrix + b"\x00" * 24 +
              struct.pack(">I", 2))
    tkhd = v0(b"tkhd",
              b"\x00\x00\x00\x03" + struct.pack(">III", 0, 0, 1) + b"\x00" * 4 +
              struct.pack(">I", 1000) + b"\x00" * 8 + struct.pack(">HHH", 0, 0, 0) +
              b"\x00\x00" + matrix + struct.pack(">II", 0x00400000, 0x00400000))
    mdhd = v0(b"mdhd", struct.pack(">IIII", 0, 0, 1000, 1000) + b"\x55\xc4\x00\x00")
    hdlr = v0(b"hdlr", struct.pack(">I", 0) + b"vide" + b"\x00" * 12 + b"VideoHandler\x00")
    vmhd = box(b"vmhd", b"\x00\x00\x00\x01" + b"\x00" * 8)
    dref = v0(b"dref", struct.pack(">I", 1) + box(b"url ", b"\x00\x00\x00\x01"))
    dinf = box(b"dinf", dref)

    # VisualSampleEntry for 'raw ' (uncompressed): 78 bytes after the header.
    sample_entry = (b"\x00" * 6 + struct.pack(">H", 1) +
                    b"\x00" * 16 +
                    struct.pack(">HH", 4, 4) +
                    struct.pack(">II", 0x00480000, 0x00480000) +
                    struct.pack(">I", 0) + struct.pack(">H", 1) +
                    b"\x00" * 32 +
                    struct.pack(">Hh", 24, -1))
    stsd = v0(b"stsd", struct.pack(">I", 1) + box(b"raw ", sample_entry))
    stts = v0(b"stts", struct.pack(">III", 1, 1, 1000))
    stsc = v0(b"stsc", struct.pack(">IIII", 1, 1, 1, 1))
    stsz = v0(b"stsz", struct.pack(">II", 16, 1))
    if use_co64:
        chunk_off = v0(b"co64", struct.pack(">IQ", 1, offset))
    else:
        chunk_off = v0(b"stco", struct.pack(">II", 1, offset))
    stbl = box(b"stbl", stsd + stts + stsc + stsz + chunk_off)
    minf = box(b"minf", vmhd + dinf + stbl)
    mdia = box(b"mdia", mdhd + hdlr + minf)
    trak = box(b"trak", tkhd + mdia)
    moov = box(b"moov", mvhd + trak)
    mdat = box(b"mdat", b"\x00" * 16)
    return ftyp + moov + mdat


if __name__ == "__main__":
    main()
