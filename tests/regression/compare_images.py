#!/usr/bin/env python3
"""Regression comparison of verifier PNG output against a reference baseline.

Compares two images (or two directories of PNGs) using:
  - mean absolute error (MAE) per pixel over all channels
  - fraction of pixels whose max channel delta exceeds PIXEL_TOLERANCE

Exit codes: 0 = pass, 1 = fail (differences exceed thresholds), 2 = error
(missing files etc.).
"""
import sys
import os
import struct
import zlib

PIXEL_TOLERANCE = 8     # max channel delta considered "equal" (0-255)
MAE_THRESHOLD = 2.0     # mean absolute error per pixel per channel
CHANGED_RATIO = 0.02    # max fraction of pixels exceeding tolerance


def decode_png(path):
    """Minimal PNG decoder for 8-bit RGB/RGBA (what QImage::save emits)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")

    pos = 8
    width = height = 0
    bit_depth = color_type = 0
    idat = b""
    palette = None
    trns = b""

    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos+4])[0]
        ctype = data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+length]
        pos += 12 + length

        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"PLTE":
            palette = chunk
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break

    if bit_depth != 8:
        raise ValueError(f"unsupported bit depth {bit_depth}: {path}")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(idat)

    stride = width * channels
    prev = bytearray(stride)
    out = bytearray(height * stride)
    ofs = 0
    p = 0
    for y in range(height):
        filter_type = raw[p]
        p += 1
        line = bytearray(raw[p:p+stride])
        p += stride

        if filter_type == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"unknown filter {filter_type} in {path}")

        out[ofs:ofs+stride] = line
        prev = line
        ofs += stride

    # Expand palette to RGB
    if color_type == 3:
        rgb = bytearray(height * width * 3)
        for i in range(height * width):
            idx = out[i]
            rgb[i*3:i*3+3] = palette[idx*3:idx*3+3]
        return width, height, 3, bytes(rgb)

    return width, height, channels, bytes(out)


def image_diff(a_path, b_path):
    """Return (mae, changed_ratio). Raises on structural mismatch."""
    w1, h1, c1, d1 = decode_png(a_path)
    w2, h2, c2, d2 = decode_png(b_path)
    if (w1, h1) != (w2, h2):
        raise ValueError(f"dimension mismatch: {w1}x{h1} vs {w2}x{h2}")
    if c1 != c2:
        raise ValueError(f"channel mismatch: {c1} vs {c2}")

    n = w1 * h1
    total = 0
    changed = 0
    for i in range(n * c1):
        d = abs(d1[i] - d2[i])
        total += d
        if d > PIXEL_TOLERANCE:
            changed += 1
            break_out = False
    # changed pixels counted per-pixel (any channel over tolerance)
    # recompute per-pixel to avoid overcounting channels
    changed = 0
    for px in range(n):
        base = px * c1
        for ch in range(c1):
            if abs(d1[base + ch] - d2[base + ch]) > PIXEL_TOLERANCE:
                changed += 1
                break

    mae = total / (n * c1)
    return mae, changed / n


def compare_file(a_path, b_path):
    try:
        mae, ratio = image_diff(a_path, b_path)
    except ValueError as e:
        print(f"FAIL {os.path.basename(a_path)}: {e}")
        return False
    ok = mae <= MAE_THRESHOLD and ratio <= CHANGED_RATIO
    status = "PASS" if ok else "FAIL"
    print(f"{status} {os.path.basename(a_path)}: mae={mae:.3f} changed={ratio*100:.2f}%")
    return ok


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("usage: compare_images.py <ref> <actual> [ref2 actual2 ...]")
        return 2

    args = sys.argv[1:]
    if len(args) == 2 and os.path.isdir(args[0]) and os.path.isdir(args[1]):
        ref_dir, act_dir = args
        refs = sorted(f for f in os.listdir(ref_dir) if f.endswith(".png"))
        if not refs:
            print("No reference PNGs found in", ref_dir)
            return 2
        failures = 0
        missing = 0
        for name in refs:
            actual = os.path.join(act_dir, name)
            if not os.path.exists(actual):
                print(f"FAIL {name}: missing from actual output")
                missing += 1
                failures += 1
                continue
            if not compare_file(os.path.join(ref_dir, name), actual):
                failures += 1
        # New files present in actual but not reference: informational
        ref_set = set(refs)
        extra = sorted(f for f in os.listdir(act_dir)
                       if f.endswith(".png") and f not in ref_set)
        for name in extra:
            print(f"NOTE {name}: new output with no reference baseline")
        total = len(refs) + len(extra)
        print(f"\n{total - failures}/{total} matched baseline "
              f"({failures} failed, {missing} missing, {len(extra)} new)")
        return 0 if failures == 0 else 1

    # Pairwise file comparison
    failures = 0
    pairs = args
    if len(pairs) == 2:
        pairs = [args[0], args[1]]
    for i in range(0, len(pairs) - 1, 2):
        if not compare_file(pairs[i], pairs[i + 1]):
            failures += 1
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
