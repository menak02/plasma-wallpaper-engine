#!/usr/bin/env python3
"""Generate a tiny deterministic scene.pkg fixture for the CI regression test.

Produces:
  tests/regression/fixture_test_scene/scene.pkg            (PKGV001 archive)
  tests/regression/fixture_baseline/fixture_test_scene.png  (expected render)

The scene is one full-screen gradient layer; the expected PNG is computed
in pure numpy so no Qt/Vulkan is needed to regenerate the baseline.
Determinism relies on: fixed 4x4 PNG input, the painter draw path, fixed
1920x1080 output, and the [0..1] channel gradation baked into the C++ render.
"""
import io
import os
import struct
import zlib

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.join(HERE, "fixture_test_scene")
BASELINE_DIR = os.path.join(HERE, "fixture_baseline")

W, H = 4, 4  # source texture size

scene_json = b"""{
  "general": {
    "title": "fixture_test_scene",
    "orthogonalprojection": { "width": 1920, "height": 1080 },
    "clearcolor": [0, 0, 0]
  },
  "objects": [
    {
      "id": 1,
      "name": "Gradient",
      "image": "materials/gradient.tex",
      "origin": [960, 540, 0],
      "scale": [1, 1, 1],
      "angles": [0, 0, 0],
      "size": [1920, 1080],
      "parallaxDepth": [0, 0, 0],
      "visible": true,
      "opacity": 1
    }
  ]
}
"""

project_json = b"""{ "properties": {} }
"""


def make_gradient_png() -> bytes:
    """4x4 RGBA gradient, row -> red, column -> green, opaque alpha."""
    yy, xx = np.meshgrid(np.arange(H, dtype=np.float64), np.arange(W, dtype=np.float64), indexing="ij")
    r = (yy / (H - 1) * 255.0).round().astype(np.uint8)
    g = (xx / (W - 1) * 255.0).round().astype(np.uint8)
    b = np.zeros((H, W), dtype=np.uint8)
    a = np.full((H, W), 255, dtype=np.uint8)
    rgba = np.dstack([r, g, b, a])
    img = Image.fromarray(rgba, "RGBA")
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def u32(n: int) -> bytes:
    return struct.pack("<I", n)


def sized(s: bytes) -> bytes:
    return u32(len(s)) + s


def build_pkg(files: dict) -> bytes:
    """PKGV001: header string, file count, then per-file (name, u32 offset,
    u32 length), then raw payloads back to back. Offsets are relative to the
    first payload byte (m_baseOffset in PkgReader)."""
    names = sorted(files)
    header = b"PKGV001"
    table = u32(len(names))
    offset = 0
    entries = []
    for name in names:
        data = files[name]
        entries.append((name, offset, len(data)))
        offset += len(data)
    out = sized(header) + table
    for name, off, length in entries:
        out += sized(name.encode("utf-8")) + u32(off) + u32(length)
    for name in names:
        out += files[name]
    return out


def expected_render() -> np.ndarray:
    """Mirror the verifier's painter math: full-screen layer at 1920x1080 with
    size 1920x1080 scaled 1:1 from a 1920x1080 ortho scene -> smooth pixmap
    stretch of the 4x4 texture. Approximate bilinear by upscaling with PIL
    bilinear, then composite opaque over the black clear color."""
    tex = make_gradient_png()
    img = Image.open(io.BytesIO(tex)).convert("RGBA")
    up = img.resize((1920, 1080), Image.BILINEAR)
    arr = np.asarray(up, dtype=np.float64)  # H x W x 4
    alpha = arr[..., 3:4] / 255.0
    # over-composite on black (clear color 0,0,0) then PNG stores straight RGB
    rgb = arr[..., :3] * alpha  # alpha=255 everywhere -> unchanged
    canvas = np.zeros((1080, 1920, 3), dtype=np.float64)
    canvas = rgb * alpha + canvas * (1 - alpha)
    return canvas.round().astype(np.uint8)


def main() -> None:
    os.makedirs(PKG_DIR, exist_ok=True)
    os.makedirs(BASELINE_DIR, exist_ok=True)

    pkg = build_pkg({
        "scene.json": scene_json,
        "project.json": project_json,
        "materials/gradient.tex": make_gradient_png(),
    })
    with open(os.path.join(PKG_DIR, "scene.pkg"), "wb") as f:
        f.write(pkg)

    # Baseline PNG mirrors what QImage::save writes (RGB888, no alpha needed
    # for the diff math — compare_images.py handles both layouts).
    out = expected_render()
    Image.fromarray(out, "RGB").save(
        os.path.join(BASELINE_DIR, "fixture_test_scene.png"), "PNG"
    )
    print(f"wrote {os.path.join(PKG_DIR, 'scene.pkg')} ({len(pkg)} bytes)")
    print(f"wrote baseline fixture_test_scene.png ({out.shape[1]}x{out.shape[0]})")


if __name__ == "__main__":
    main()
