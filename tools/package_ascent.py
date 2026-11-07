#!/usr/bin/env python3
"""Package a static native build and its baked UI as a read-only boot runtime."""
import argparse
import array
import struct
from pathlib import Path
import shutil
import subprocess
import tempfile


def compact_idle(path):
    # XOR later themes against the first baked picture. The background is
    # identical; zero deltas compress well even in bounded SquashFS blocks.
    # Reconstruction is lossless and happens only when the idle theme changes.
    data = bytearray(path.read_bytes())
    magic, version, width, height, count = struct.unpack_from("<4s4I", data)
    if magic != b"KUIB" or version != 1 or len(data) != 20+count*(32+width*height*4):
        raise ValueError(f"Invalid baked idle artwork: {path}")
    size = width*height*4
    base = array.array("I", data[52:52+size])
    for i in range(1, count):
        offset = 20+i*(32+size)+32
        pixels = array.array("I", data[offset:offset+size])
        for j in range(len(pixels)):
            pixels[j] ^= base[j]
        data[offset:offset+size] = pixels.tobytes()
    struct.pack_into("<I", data, 4, 2)
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--strip", default="aarch64-linux-gnu-strip")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # All intermediates stay beside the requested artifact (normally .local).
    with tempfile.TemporaryDirectory(dir=args.output.parent) as tmp:
        root = Path(tmp)/"runtime"
        (root/"bin").mkdir(parents=True)
        for name in ("kestrel-gnd", "kestrel-hud", "kestrel-hud-vector"):
            source = args.build/(name if name == "kestrel-gnd" else "hud/lvgl/"+name)
            info = subprocess.check_output(["file", "-b", str(source)], text=True)
            if "ARM aarch64" not in info or "statically linked" not in info:
                raise SystemExit(f"{source}: expected a static AArch64 executable")
            target = root/"bin"/name
            shutil.copy2(source, target)
            subprocess.run([args.strip, str(target)], check=True)
        for profile in ("1280x720", "1920x1080", "menu-fonts"):
            source = args.build/"ui-assets"/profile
            if not source.is_dir():
                raise SystemExit(f"Missing generated UI profile: {source}")
            if profile != "menu-fonts":
                for name in ("idle-scenes.kui", "betaflight.kui", "canopy-font.kuf",
                             "canopy-left.kui", "canopy-right.kui"):
                    if not (source/name).is_file():
                        raise SystemExit(f"Missing UI asset: {source/name}")
            shutil.copytree(source, root/"ui"/profile)
        for profile in ("1280x720", "1920x1080"):
            compact_idle(root/"ui"/profile/"idle-scenes.kui")
        image = Path(tmp)/"runtime.squashfs"
        subprocess.run(["mksquashfs", str(root), str(image), "-comp", "xz",
                        "-b", "262144", "-all-root", "-noappend", "-no-progress",
                        "-processors", "2"], check=True)
        image.replace(args.output)
    print(f"{args.output}: {args.output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
