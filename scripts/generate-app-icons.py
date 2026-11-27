#!/usr/bin/env python3
"""Generate cohesive Oink-inspired flat 2D DeskMate app icons for all 4 platforms.

Outputs:
- macOS:       apps/macos-host/Resources/AppIcon.icns
- Windows:     apps/windows-host/resources/AppIcon.ico
- HarmonyOS:   apps/harmonyos-client/AppScope/resources/base/media/app_icon.png
               apps/harmonyos-client/entry/src/main/resources/base/media/app_icon.png
- Android:     apps/android-client/app/src/main/res/drawable-nodpi/ic_launcher_background.png
               apps/android-client/app/src/main/res/drawable-nodpi/ic_launcher_foreground.png
               apps/android-client/app/src/main/res/mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/ic_launcher{,_round}.png
"""

from __future__ import annotations

import argparse
import io
import math
from pathlib import Path
import struct
import tempfile
from PIL import Image, ImageChops, ImageDraw

BG_COLOR = (0x1B, 0x1E, 0x28, 255)      # #1B1E28 (Oink charcoal-navy backdrop)
WHITE_COLOR = (0xFF, 0xFF, 0xFF, 255)   # #FFFFFF (Primary Host display)
ACCENT_COLOR = (0x2E, 0x7B, 0xFF, 255)  # #2E7BFF (Cobalt companion display & wired link)

WIN_ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
WIN_DIB_SIZES = {16, 24, 32, 48, 64}

ANDROID_MIPMAPS = {
    "mipmap-mdpi": 48,
    "mipmap-hdpi": 72,
    "mipmap-xhdpi": 96,
    "mipmap-xxhdpi": 144,
    "mipmap-xxxhdpi": 192,
}

# Standard Apple PNG-encoded .icns chunk types matching `iconutil -c icns` output.
ICNS_CHUNKS = [
    (b"icp4", 16),    # 16x16
    (b"ic11", 32),    # 16x16@2x
    (b"icp5", 32),    # 32x32
    (b"ic12", 64),    # 32x32@2x
    (b"ic07", 128),   # 128x128
    (b"ic13", 256),   # 128x128@2x
    (b"ic08", 256),   # 256x256
    (b"ic14", 512),   # 256x256@2x
    (b"ic09", 512),   # 512x512
    (b"ic10", 1024),  # 512x512@2x
]


def _encode_ico_dib(width: int, height: int, rgba: bytes) -> bytes:
    header = struct.pack(
        "<IIIHHIIIIII",
        40,
        width,
        height * 2,
        1,
        32,
        0,
        width * height * 4,
        0,
        0,
        0,
        0,
    )
    xor_buf = bytearray(width * height * 4)
    and_stride = ((width + 31) // 32) * 4
    and_buf = bytearray(and_stride * height)
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
                and_buf[y * and_stride + (x >> 3)] |= 0x80 >> (x & 7)
    return header + bytes(xor_buf) + bytes(and_buf)


def _write_ico_from_master(master_rgba: Image.Image, out_ico: Path) -> None:
    images: list[tuple[int, bytes]] = []
    for size in WIN_ICO_SIZES:
        resized = master_rgba.resize((size, size), Image.Resampling.LANCZOS)
        if size in WIN_DIB_SIZES:
            payload = _encode_ico_dib(size, size, resized.tobytes())
        else:
            with tempfile.NamedTemporaryFile(suffix=".png") as tmp:
                resized.save(tmp.name, format="PNG")
                payload = Path(tmp.name).read_bytes()
        images.append((size, payload))

    count = len(images)
    offset = 6 + count * 16
    entries = bytearray()
    payload_blob = bytearray()
    for size, payload in images:
        dim = 0 if size >= 256 else size
        entries.extend(
            struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(payload), offset)
        )
        payload_blob.extend(payload)
        offset += len(payload)

    out_ico.parent.mkdir(parents=True, exist_ok=True)
    header = struct.pack("<HHH", 0, 1, count)
    out_ico.write_bytes(header + bytes(entries) + bytes(payload_blob))


def _render_mark_layer(box_px: int) -> Image.Image:
    """Render the transparent-background DeskMate 2-display + wired link mark at box_px."""
    scale = box_px / 64.0

    def pt(x: float, y: float) -> tuple[float, float]:
        return (x * scale, y * scale)

    def box(x: float, y: float, w: float, h: float) -> list[float]:
        return [x * scale, y * scale, (x + w) * scale, (y + h) * scale]

    # 1. White Host Display mask minus Companion Knockout mask.
    host_mask = Image.new("L", (box_px, box_px), 0)
    ImageDraw.Draw(host_mask).rounded_rectangle(
        box(10.5, 14.0, 29.0, 21.5),
        radius=5.2 * scale,
        fill=255,
    )
    knockout_mask = Image.new("L", (box_px, box_px), 0)
    ImageDraw.Draw(knockout_mask).rounded_rectangle(
        box(24.5, 26.8, 32.0, 25.5),
        radius=7.6 * scale,
        fill=255,
    )
    host_mask = ImageChops.subtract(host_mask, knockout_mask)

    white_layer = Image.new("RGBA", (box_px, box_px), WHITE_COLOR)
    white_layer.putalpha(host_mask)

    # 2. Cobalt Wired USB Link + Companion Display rendered as one single-pass mask.
    blue_mask = Image.new("L", (box_px, box_px), 0)
    blue_draw = ImageDraw.Draw(blue_mask)

    stroke_r = 2.3  # stroke width = 4.6 in 64-unit space
    x_vert = 18.8
    y_horiz = 41.5
    bend_r = 5.4
    cx = x_vert + bend_r
    cy = y_horiz - bend_r
    outer_pts: list[tuple[float, float]] = [
        pt(x_vert - stroke_r, 32.5),
        pt(x_vert - stroke_r, cy),
    ]
    steps = 72
    for i in range(steps + 1):
        theta = math.pi - (math.pi * 0.5) * (i / steps)
        outer_pts.append(
            pt(cx + (bend_r + stroke_r) * math.cos(theta),
               cy + (bend_r + stroke_r) * math.sin(theta))
        )
    outer_pts.extend([
        pt(31.0, y_horiz + stroke_r),
        pt(31.0, y_horiz - stroke_r),
        pt(cx, y_horiz - stroke_r),
    ])
    for i in range(steps + 1):
        theta = (math.pi * 0.5) + (math.pi * 0.5) * (i / steps)
        outer_pts.append(
            pt(cx + (bend_r - stroke_r) * math.cos(theta),
               cy + (bend_r - stroke_r) * math.sin(theta))
        )
    outer_pts.append(pt(x_vert + stroke_r, 32.5))
    blue_draw.polygon(outer_pts, fill=255)

    # Round endcaps for the cable ends.
    for ex, ey in [(x_vert, 32.5), (31.0, y_horiz)]:
        blue_draw.ellipse(
            box(ex - stroke_r, ey - stroke_r, stroke_r * 2.0, stroke_r * 2.0),
            fill=255,
        )

    # Companion Extended Display.
    blue_draw.rounded_rectangle(
        box(27.5, 29.8, 26.0, 19.5),
        radius=5.0 * scale,
        fill=255,
    )

    blue_layer = Image.new("RGBA", (box_px, box_px), ACCENT_COLOR)
    blue_layer.putalpha(blue_mask)

    return Image.alpha_composite(blue_layer, white_layer)


def render_rounded_square_icon(
    canvas_px: int,
    body_px: float,
    corner_r: float,
    supersample: int = 4,
    mark_scale: float = 1.0,
) -> Image.Image:
    """Render an Oink-style flat rounded-square icon on transparent RGBA canvas."""
    hi = canvas_px * supersample
    img = Image.new("RGBA", (hi, hi), (0, 0, 0, 0))
    margin = (canvas_px - body_px) * 0.5 * supersample
    body_hi = body_px * supersample
    ImageDraw.Draw(img).rounded_rectangle(
        [margin, margin, margin + body_hi, margin + body_hi],
        radius=corner_r * supersample,
        fill=BG_COLOR,
    )
    mark_hi = int(round(body_hi * mark_scale))
    mark = _render_mark_layer(mark_hi)
    offset = int(round((hi - mark_hi) * 0.5))
    img.alpha_composite(mark, dest=(offset, offset))
    return img.resize((canvas_px, canvas_px), Image.Resampling.LANCZOS)


def render_full_bleed_icon(
    canvas_px: int,
    supersample: int = 4,
    mark_scale: float = 1.0,
) -> Image.Image:
    """Render a full-bleed #1B1E28 icon (for HarmonyOS system mask / Android background)."""
    hi = canvas_px * supersample
    img = Image.new("RGBA", (hi, hi), BG_COLOR)
    mark_hi = int(round(hi * mark_scale))
    mark = _render_mark_layer(mark_hi)
    offset = int(round((hi - mark_hi) * 0.5))
    img.alpha_composite(mark, dest=(offset, offset))
    return img.resize((canvas_px, canvas_px), Image.Resampling.LANCZOS)


def render_android_foreground(canvas_px: int = 432, safe_box_px: int = 336, supersample: int = 4) -> Image.Image:
    """Render Android adaptive foreground with transparent knockout within safe zone."""
    hi = canvas_px * supersample
    img = Image.new("RGBA", (hi, hi), (0, 0, 0, 0))
    mark_hi = safe_box_px * supersample
    mark = _render_mark_layer(mark_hi)
    offset = int(round((hi - mark_hi) * 0.5))
    img.alpha_composite(mark, dest=(offset, offset))
    return img.resize((canvas_px, canvas_px), Image.Resampling.LANCZOS)


def render_round_icon(canvas_px: int, diameter_ratio: float = 0.94, mark_scale: float = 0.86, supersample: int = 4) -> Image.Image:
    hi = canvas_px * supersample
    img = Image.new("RGBA", (hi, hi), (0, 0, 0, 0))
    pad = (1.0 - diameter_ratio) * 0.5 * hi
    ImageDraw.Draw(img).ellipse([pad, pad, hi - pad, hi - pad], fill=BG_COLOR)
    mark_hi = int(round(hi * diameter_ratio * mark_scale))
    mark = _render_mark_layer(mark_hi)
    offset = int(round((hi - mark_hi) * 0.5))
    img.alpha_composite(mark, dest=(offset, offset))
    return img.resize((canvas_px, canvas_px), Image.Resampling.LANCZOS)


def build_macos_icns(master_1024: Image.Image, out_icns: Path) -> None:
    out_icns.parent.mkdir(parents=True, exist_ok=True)
    png_cache: dict[int, bytes] = {}
    body = bytearray()
    for tag, size in ICNS_CHUNKS:
        if size not in png_cache:
            resized = (
                master_1024
                if size == 1024
                else master_1024.resize((size, size), Image.Resampling.LANCZOS)
            )
            buf = io.BytesIO()
            resized.save(buf, format="PNG")
            png_cache[size] = buf.getvalue()
        png_bytes = png_cache[size]
        body.extend(struct.pack(">4sI", tag, 8 + len(png_bytes)))
        body.extend(png_bytes)
    out_icns.write_bytes(struct.pack(">4sI", b"icns", 8 + len(body)) + bytes(body))


def build_preview_sheet(
    out_path: Path,
    macos_icon: Image.Image,
    windows_icon: Image.Image,
    harmony_icon: Image.Image,
    android_bg: Image.Image,
    android_fg: Image.Image,
    android_round: Image.Image,
) -> None:
    # Composite Android adaptive squircle preview (72dp viewport = 288px of 432px)
    crop_pad = (432 - 288) // 2
    android_comp = Image.alpha_composite(
        android_bg.crop((crop_pad, crop_pad, 432 - crop_pad, 432 - crop_pad)),
        android_fg.crop((crop_pad, crop_pad, 432 - crop_pad, 432 - crop_pad)),
    ).resize((256, 256), Image.Resampling.LANCZOS)
    sq_mask = Image.new("L", (256, 256), 0)
    ImageDraw.Draw(sq_mask).rounded_rectangle([0, 0, 256, 256], radius=58, fill=255)
    android_sq = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    android_sq.paste(android_comp, (0, 0), sq_mask)

    harm_sq = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    harm_sq.paste(harmony_icon.resize((256, 256), Image.Resampling.LANCZOS), (0, 0), sq_mask)

    sheet = Image.new("RGBA", (1520, 740), (0xF5, 0xF5, 0xF3, 255))
    draw = ImageDraw.Draw(sheet)
    # Bottom half dark strip to verify dark desktop & dark theme contrast
    draw.rectangle([0, 380, 1520, 740], fill=(0x11, 0x12, 0x14, 255))

    samples = [
        macos_icon.resize((256, 256), Image.Resampling.LANCZOS),
        windows_icon.resize((256, 256), Image.Resampling.LANCZOS),
        harm_sq,
        android_sq,
        android_round.resize((256, 256), Image.Resampling.LANCZOS),
    ]
    for idx, sample in enumerate(samples):
        x = 60 + idx * 290
        sheet.alpha_composite(sample, dest=(x, 60))

    # Bottom dark row: macOS/Windows small sizes (128, 64, 32, 24, 16) + Android fg on #1B1E28
    x_cursor = 60
    for src in [macos_icon, windows_icon]:
        for sz in [128, 64, 32, 24, 16]:
            small = src.resize((sz, sz), Image.Resampling.LANCZOS)
            sheet.alpha_composite(small, dest=(x_cursor, 430 + (128 - sz) // 2))
            x_cursor += sz + 28
        x_cursor += 50

    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path, format="PNG")


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Generate DeskMate app icons for all 4 platforms.")
    parser.add_argument("--preview", type=Path, default=None, help="Optional contact sheet PNG path.")
    parser.add_argument(
        "--harmony-store-icon",
        type=Path,
        default=None,
        help="Optional 1024x1024 opaque RGB PNG output path for Huawei AppGallery.",
    )
    parser.add_argument(
        "--only-harmony-store",
        action="store_true",
        help="Only generate the 1024x1024 AppGallery store icon without modifying platform icons.",
    )
    args = parser.parse_args()

    if args.harmony_store_icon is not None or args.only_harmony_store:
        store_out = args.harmony_store_icon or (repo / "dist" / "harmonyos" / "store" / "app_icon_1024.png")
        if not store_out.is_absolute():
            store_out = repo / store_out
        store_out.parent.mkdir(parents=True, exist_ok=True)
        store_1024 = render_full_bleed_icon(1024, supersample=4, mark_scale=0.96).convert("RGB")
        store_1024.save(store_out, format="PNG")
        print(f"Generated HarmonyOS AppGallery 1024x1024 opaque store icon at {store_out}")
        if args.only_harmony_store:
            return 0

    # 1. macOS: 1024x1024 with Oink's 824x824 rx=185 squircle proportion
    macos_master = render_rounded_square_icon(1024, body_px=824.0, corner_r=185.0, supersample=4, mark_scale=1.0)
    build_macos_icns(macos_master, repo / "apps" / "macos-host" / "Resources" / "AppIcon.icns")

    # 2. Windows: 1024x1024 master with Oink's 240/256 (960x960 rx=176) proportion
    windows_master = render_rounded_square_icon(1024, body_px=960.0, corner_r=176.0, supersample=4, mark_scale=1.0)
    _write_ico_from_master(windows_master, repo / "apps" / "windows-host" / "resources" / "AppIcon.ico")

    # 3. HarmonyOS NEXT: 512x512 full-bleed #1B1E28
    harmony_512 = render_full_bleed_icon(512, supersample=4, mark_scale=0.96)
    for rel in [
        "apps/harmonyos-client/AppScope/resources/base/media/app_icon.png",
        "apps/harmonyos-client/entry/src/main/resources/base/media/app_icon.png",
    ]:
        out = repo / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        harmony_512.save(out, format="PNG")

    # 4. Android Adaptive background + foreground + legacy mipmaps
    res_dir = repo / "apps" / "android-client" / "app" / "src" / "main" / "res"
    android_bg = Image.new("RGBA", (432, 432), BG_COLOR)
    android_fg = render_android_foreground(432, safe_box_px=268, supersample=4)
    android_bg.save(res_dir / "drawable-nodpi" / "ic_launcher_background.png", format="PNG")
    android_fg.save(res_dir / "drawable-nodpi" / "ic_launcher_foreground.png", format="PNG")

    legacy_sq_192 = render_rounded_square_icon(192, body_px=176.0, corner_r=42.0, supersample=4, mark_scale=0.98)
    legacy_round_192 = render_round_icon(192, diameter_ratio=0.94, mark_scale=0.86, supersample=4)
    for folder, size in ANDROID_MIPMAPS.items():
        sq = legacy_sq_192 if size == 192 else legacy_sq_192.resize((size, size), Image.Resampling.LANCZOS)
        rnd = legacy_round_192 if size == 192 else legacy_round_192.resize((size, size), Image.Resampling.LANCZOS)
        (res_dir / folder).mkdir(parents=True, exist_ok=True)
        sq.save(res_dir / folder / "ic_launcher.png", format="PNG")
        rnd.save(res_dir / folder / "ic_launcher_round.png", format="PNG")

    if args.preview is not None:
        build_preview_sheet(
            args.preview,
            macos_master,
            windows_master,
            harmony_512,
            android_bg,
            android_fg,
            legacy_round_192,
        )
        print(f"Generated preview sheet at {args.preview}")

    print("Generated DeskMate icons for macOS, Windows, HarmonyOS NEXT, and Android.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
