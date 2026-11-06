#!/usr/bin/env python3
"""Bake the original HUD face into a bounded A8 glyph atlas with kerning."""
import argparse
import math
from pathlib import Path
import shutil
import struct

from PIL import Image, ImageDraw, ImageFont


def icon(code):
    """Original vec_icons.cpp paths, baked into spare atlas glyphs at the host."""
    supersample = 4
    image = Image.new("L", (96 * supersample, 64 * supersample))
    draw = ImageDraw.Draw(image)
    # Both occupy the same 80px slot; the goggles keep their wide aspect.
    if code == 0xE000:
        scale, ox, oy = 56 / 27.8, 2 + (80 - 56) / 2, 34 - 28
        origin = (2.1, 2.1)
    elif code == 0xE002:
        scale, ox, oy = 80 / 27.2, 2, 34 - 14.4 * (80 / 27.2) / 2
        origin = (2.4, 8.8)
    else:
        scale, ox, oy = 80 / 29.6, 2, 34 - 13.6 * (80 / 29.6) / 2
        origin = (1.2, 9.2)
    def point(x, y):
        return ((ox + (x - origin[0]) * scale) * supersample,
                (oy + (y - origin[1]) * scale) * supersample)
    def line(points, width):
        draw.line([point(x, y) for x, y in points], fill=255, width=round(width * scale * supersample))
    if code == 0xE000:
        line([(10.5, 10.5), (21.5, 21.5)], 2.6)
        line([(21.5, 10.5), (10.5, 21.5)], 2.6)
        for x, y in [(7.5, 7.5), (24.5, 7.5), (7.5, 24.5), (24.5, 24.5)]:
            draw.ellipse((*point(x - 4.2, y - 4.2), *point(x + 4.2, y + 4.2)),
                         outline=255, width=round(2.4 * scale * supersample))
    elif code == 0xE002:
        line([(3.6, 10), (3.6, 22)], 2.4)
        line([(28.4, 10), (28.4, 22)], 2.4)
        line([(3.6, 16), (28.4, 16)], 2.2)
        line([(10.6, 11.6), (6.4, 16), (10.6, 20.4)], 2.2)
        line([(21.4, 11.6), (25.6, 16), (21.4, 20.4)], 2.2)
    else:
        for x in (2.4, 17.4):
            draw.rounded_rectangle((*point(x, 10.4), *point(x + 12.2, 21.6)), radius=4 * scale * supersample,
                                   outline=255, width=round(2.4 * scale * supersample))
        line([(14.6, 16), (17.4, 16)], 2.4)
    return image.resize((96, 64), Image.Resampling.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path, help="ChakraPetch-SemiBold.ttf, as used by the GLES OSD")
    parser.add_argument("output", type=Path, help="Output canopy-font.kuf")
    parser.add_argument("--license", type=Path, help="Font license (default: OFL.txt beside the TTF)")
    parser.add_argument("--pixels", type=int, default=40, help="Native ink size (8..64 pixels)")
    args = parser.parse_args()
    if not 8 <= args.pixels <= 64:
        parser.error("pixels must be 8..64")
    pixels = args.pixels
    cell = math.ceil(pixels * 64 / 40)
    baseline = round(pixels * 48 / 40)
    license_path = args.license or args.font.parent / "OFL.txt"
    if not license_path.is_file():
        parser.error("Provide the source font's OFL license with --license")
    font = ImageFont.truetype(str(args.font), pixels)
    codes = list(range(32, 127)) + [0xB0, 0x2022, 0xE000, 0xE001, 0xE002]
    chars = [chr(code) for code in codes]
    advances = [round(80 * pixels / 40 * 64) if code >= 0xE000 else round(font.getlength(char) * 64) for code, char in zip(codes, chars)]
    payload = bytearray(struct.pack("<4s5I", b"KUIF", 1, pixels, cell, baseline, len(codes)))
    for code, char, advance in zip(codes, chars, advances):
        if code >= 0xE000:
            image = icon(code).resize((round(96 * pixels / 40), cell), Image.Resampling.LANCZOS)
            payload.extend(struct.pack("<IIi", code, image.width, advance))
            payload.extend(image.tobytes())
            continue
        box = font.getbbox(char, anchor="ls")
        width = max(16, (math.ceil(2 + max(advance / 64, box[2])) + 4 + 15) & ~15)
        image = Image.new("L", (width, cell))
        ImageDraw.Draw(image).text((2, baseline), char, font=font, fill=255, anchor="ls")
        payload.extend(struct.pack("<IIi", code, width, advance))
        payload.extend(image.tobytes())
    for code, a, advance in zip(codes, chars, advances):
        for next_code, b, next_advance in zip(codes, chars, advances):
            kern = 0 if code >= 0xE000 or next_code >= 0xE000 else round(font.getlength(a + b) * 64) - advance - next_advance
            payload.extend(struct.pack("<h", kern))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    shutil.copyfile(license_path, args.output.with_name("canopy-font-OFL.txt"))
    print(f"{args.output}: {len(codes)} glyphs, {pixels}px ink / {cell}px cell / baseline {baseline}, {len(payload)} bytes")


if __name__ == "__main__":
    main()
