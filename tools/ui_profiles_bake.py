#!/usr/bin/env python3
"""Build the two native UI profiles; refresh rates share their resolution's assets."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ('font', 'background', 'idle_tool', 'bf_tool', 'wedge_tool', 'output'):
        p.add_argument(arg, type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    font_tool = Path(__file__).with_name('ui_font_bake.py')
    # Small glyph atlases are shared by both resolutions and UI-scale settings.
    # The target loads only sizes it uses; no complete menu/theme copies per size.
    for pixels in range(8, 49):
        subprocess.run([sys.executable, str(font_tool), str(a.font),
                        str(a.output/'menu-fonts'/f'font-{pixels}.kuf'),
                        '--pixels', str(pixels)], check=True)
    for w, h in ((1280, 720), (1920, 1080)):
        profile = a.output/f'{w}x{h}'
        profile.mkdir(exist_ok=True)
        shutil.copyfile(a.output/'menu-fonts/font-40.kuf', profile/'canopy-font.kuf')
        shutil.copyfile(a.output/'menu-fonts/canopy-font-OFL.txt', profile/'canopy-font-OFL.txt')
        subprocess.run([str(a.wedge_tool), str(w), str(h), str(profile)], check=True)
        subprocess.run([str(a.idle_tool), str(a.font), str(a.background), str(w), str(h),
                        str(profile/'idle-scenes.kui')], check=True)
        subprocess.run([str(a.bf_tool), str(a.font), str(profile/'betaflight.kui'), str(h)], check=True)
    (a.output/'profiles.stamp').write_text('1280x720\n1920x1080\n')


if __name__ == '__main__':
    main()
