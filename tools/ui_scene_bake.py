#!/usr/bin/env python3
"""Bake VRX Pro idle artwork for the software renderer (host-only Cairo/Pillow)."""
import argparse
import ctypes
from pathlib import Path
import struct
import cairo
from PIL import Image


def write_image(path, width, height, channels, pixels):
    path.write_bytes(struct.pack('<4sIII', b'KUIS', width, height, channels) + pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font', type=Path, help='ChakraPetch-SemiBold.ttf from the fpvOS image')
    parser.add_argument('output', type=Path, help='Generated UI assets directory')
    parser.add_argument('--background', type=Path, help='Original background.png from the fpvOS image')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    args = parser.parse_args()
    if not args.font.is_file() or not 1 <= args.width <= 4096 or not 1 <= args.height <= 4096:
        parser.error('Provide a font and dimensions between 1 and 4096')
    fc = ctypes.CDLL('libfontconfig.so.1')
    fc.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    fc.FcConfigAppFontAddFile.restype = ctypes.c_int
    if not fc.FcConfigAppFontAddFile(None, bytes(args.font.resolve())):
        parser.error('Could not register the source font')
    args.output.mkdir(parents=True, exist_ok=True)
    # Keep identical to render_idle_title() in hud/osd_images.cpp. Separate
    # alpha masks let the active theme color fpv and OS independently.
    w, h = 2048, 256
    for part, name in enumerate(('idle-fpv.kui', 'idle-os.kui')):
        surface = cairo.ImageSurface(cairo.FORMAT_A8, w, h)
        cr = cairo.Context(surface)
        cr.select_font_face('Chakra Petch SemiBold', cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        size = 168.0
        cr.set_font_size(size)
        all_text = cr.text_extents('fpvOS')
        if all_text.width > 1:
            size *= (w * 0.50) / all_text.width
            cr.set_font_size(size)
            all_text = cr.text_extents('fpvOS')
        if all_text.height > h * 0.52:
            size *= h * 0.52 / all_text.height
            cr.set_font_size(size)
            all_text = cr.text_extents('fpvOS')
        x = (w - all_text.width) * 0.5 - all_text.x_bearing
        y = (h - all_text.height) * 0.5 - all_text.y_bearing
        if part:
            x += cr.text_extents('fpv').x_advance
        cr.set_source_rgba(1, 1, 1, 1)
        cr.move_to(x, y)
        cr.show_text('OS' if part else 'fpv')
        surface.flush()
        write_image(args.output / name, w, h, 1, bytes(surface.get_data()))
    if args.background:
        background = Image.open(args.background).convert('RGBA').resize((args.width, args.height), Image.Resampling.LANCZOS)
        write_image(args.output / 'idle-background.kui', args.width, args.height, 4, background.tobytes('raw', 'BGRA'))


if __name__ == '__main__':
    main()
