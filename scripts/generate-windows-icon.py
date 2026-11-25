#!/usr/bin/env python3
"""Generate apps/windows-host/resources/AppIcon.ico from macOS AppIcon.icns.

Uses macOS `sips` + Python 3 stdlib (struct/zlib) with zero third-party deps.
Sizes 16..64 are stored as classic 32-bit BGRA BITMAPINFOHEADER DIBs + 1-bpp
AND masks for universal Win32/Explorer/LoadIcon compatibility; 128 and 256
are stored as PNG-in-ICO (Vista+).
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

SIZES = [16, 24, 32, 48, 64, 128, 256]
DIB_SIZES = {16, 24, 32, 48, 64}


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png_rgba(png_bytes: bytes) -> tuple[int, int, bytes]:
    if png_bytes[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("invalid PNG header")
    pos = 8
    width = height = bit_depth = color_type = interlace = 0
    idat = bytearray()
    palette: bytes | None = None
    trns: bytes | None = None
    while pos + 8 <= len(png_bytes):
        length, ctype = struct.unpack(">I4s", png_bytes[pos : pos + 8])
        pos += 8
        chunk = png_bytes[pos : pos + length]
        pos += length + 4
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif ctype == b"PLTE":
            palette = chunk
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if interlace != 0 or bit_depth != 8:
        raise ValueError(
            f"unsupported PNG format: bit_depth={bit_depth}, interlace={interlace}"
        )

    channels_map = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in channels_map:
        raise ValueError(f"unsupported PNG color_type={color_type}")
    bpp = channels_map[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * bpp
    expected = (stride + 1) * height
    if len(raw) != expected:
        raise ValueError(f"unexpected decompressed size {len(raw)} != {expected}")

    prev = bytearray(stride)
    rows: list[bytes] = []
    idx = 0
    for _ in range(height):
        ftype = raw[idx]
        idx += 1
        scan = bytearray(raw[idx : idx + stride])
        idx += stride
        if ftype == 0:
            pass
        elif ftype == 1:
            for i in range(bpp, stride):
                scan[i] = (scan[i] + scan[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                scan[i] = (scan[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                a = scan[i - bpp] if i >= bpp else 0
                scan[i] = (scan[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = scan[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                scan[i] = (scan[i] + _paeth(a, b, c)) & 0xFF
        else:
            raise ValueError(f"unknown PNG filter type {ftype}")
        prev = scan
        rows.append(bytes(scan))

    rgba = bytearray(width * height * 4)
    out = 0
    for row in rows:
        if color_type == 6:
            rgba[out : out + width * 4] = row
            out += width * 4
        elif color_type == 2:
            for x in range(0, stride, 3):
                rgba[out] = row[x]
                rgba[out + 1] = row[x + 1]
                rgba[out + 2] = row[x + 2]
                rgba[out + 3] = 255
                out += 4
        elif color_type == 4:
            for x in range(0, stride, 2):
                g = row[x]
                rgba[out] = g
                rgba[out + 1] = g
                rgba[out + 2] = g
                rgba[out + 3] = row[x + 1]
                out += 4
        elif color_type == 0:
            for g in row:
                rgba[out] = g
                rgba[out + 1] = g
                rgba[out + 2] = g
                rgba[out + 3] = 255
                out += 4
        elif color_type == 3:
            assert palette is not None
            for p_idx in row:
                rgba[out] = palette[p_idx * 3]
                rgba[out + 1] = palette[p_idx * 3 + 1]
                rgba[out + 2] = palette[p_idx * 3 + 2]
                rgba[out + 3] = (
                    trns[p_idx] if trns is not None and p_idx < len(trns) else 255
                )
                out += 4
    return width, height, bytes(rgba)


def encode_ico_dib(width: int, height: int, rgba: bytes) -> bytes:
    header = struct.pack(
        "<IIIHHIIIIII",
        40,          # biSize
        width,       # biWidth
        height * 2,  # biHeight (XOR + AND masks)
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression = BI_RGB
        width * height * 4,
        0,
        0,
        0,
        0,
    )
    xor_buf = bytearray(width * height * 4)
    and_stride = ((width + 31) // 32) * 4
    and_buf = bytearray(and_stride * height)

    # DIB rows are stored bottom-to-top in BGRA order.
    for y in range(height):
        src_y = height - 1 - y
        for x in range(width):
            s = (src_y * width + x) * 4
            d = (y * width + x) * 4
            r, g, b, a = rgba[s], rgba[s + 1], rgba[s + 2], rgba[s + 3]
            xor_buf[d] = b
            xor_buf[d + 1] = g
            xor_buf[d + 2] = r
            xor_buf[d + 3] = a
            if a < 128:
                byte_idx = y * and_stride + (x >> 3)
                and_buf[byte_idx] |= 0x80 >> (x & 7)
    return header + bytes(xor_buf) + bytes(and_buf)


def build_ico(icns_path: Path, out_ico: Path) -> None:
    if not icns_path.is_file():
        raise FileNotFoundError(f"missing source icon: {icns_path}")

    with tempfile.TemporaryDirectory(prefix="deskmate_ico_") as tmp:
        tmp_dir = Path(tmp)
        base_png = tmp_dir / "base.png"
        subprocess.run(
            ["/usr/bin/sips", "-s", "format", "png", str(icns_path), "--out", str(base_png)],
            check=True,
            stdout=subprocess.DEVNULL,
        )

        images: list[tuple[int, bytes]] = []
        for size in SIZES:
            resized = tmp_dir / f"icon_{size}.png"
            subprocess.run(
                [
                    "/usr/bin/sips",
                    "-z",
                    str(size),
                    str(size),
                    str(base_png),
                    "--out",
                    str(resized),
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            png_bytes = resized.read_bytes()
            if size in DIB_SIZES:
                w, h, rgba = decode_png_rgba(png_bytes)
                if w != size or h != size:
                    raise ValueError(f"expected {size}x{size}, got {w}x{h}")
                payload = encode_ico_dib(w, h, rgba)
            else:
                payload = png_bytes
            images.append((size, payload))

    count = len(images)
    offset = 6 + count * 16
    entries = bytearray()
    payload_blob = bytearray()
    for size, payload in images:
        dim = 0 if size >= 256 else size
        entries.extend(
            struct.pack(
                "<BBBBHHII",
                dim,
                dim,
                0,
                0,
                1,
                32,
                len(payload),
                offset,
            )
        )
        payload_blob.extend(payload)
        offset += len(payload)

    out_ico.parent.mkdir(parents=True, exist_ok=True)
    header = struct.pack("<HHH", 0, 1, count)
    out_ico.write_bytes(header + bytes(entries) + bytes(payload_blob))


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Generate Windows AppIcon.ico from macOS AppIcon.icns")
    parser.add_argument(
        "--icns",
        default=str(repo / "apps" / "macos-host" / "Resources" / "AppIcon.icns"),
    )
    parser.add_argument(
        "--out",
        default=str(repo / "apps" / "windows-host" / "resources" / "AppIcon.ico"),
    )
    args = parser.parse_args()
    build_ico(Path(args.icns), Path(args.out))
    print(f"Generated {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
