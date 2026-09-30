#!/usr/bin/env python3
"""Make a damaged AR8030 video stream from a clean H.265 one, for
kestrel-gnd --debug-replay.

    tools/ar8030_fuzz.py clean.h265 bad.bin [--seed N] [--rate R] [--modes a,b]
    ssh root@192.168.3.1 'cat > /tmp/bad.bin' < bad.bin
    kestrel-gnd --debug-replay /tmp/bad.bin

The output is laid out as the baseband delivers it: each slice NAL follows the
air unit's per-picture header (a NAL-shaped block whose first byte is 0x80,
carrying fps, a picture counter, the size and a capture stamp - see emit_nal).
Then it is broken the ways a bad air stream breaks, each picture at --rate:

  stale   an old picture sent again, header and all (the air unit replaying
          its encoder ring: counters and stamps seconds in the past)
  mix     an old picture's slice between this picture's two halves, so both
          end up in one access unit (MPP: "POC change between slices")
  drop    a slice left out            dup    a slice sent twice
  swap    a picture's slices reversed trunc  a slice cut short
  flip    bytes flipped in a slice     junk   a NAL of random type and bytes
  params  a VPS/SPS/PPS with its body scrambled
  header  the 0x80 header's counter and stamp jumped

--cut ends the file in the middle of a picture. kestrel-gnd pauses two
seconds between passes of a replay and the next pass starts over from the
parameter sets and a keyframe, on a clock that went back: together, the air
app stopping dead during a restart and coming back.

A clean stream should be 1080p with 32x32 CTBs and two slices per picture, as
the air unit sends, e.g. from gstreamer's x265enc with
option-string="ctu=32:slices=2:bframes=0:repeat-headers=1".
"""
import argparse
import random
import re
import struct

MODES = ['stale', 'mix', 'drop', 'dup', 'swap', 'trunc', 'flip', 'junk', 'params', 'header']
SC = b'\x00\x00\x00\x01'


def nals(data):
    starts = [m.end() for m in re.finditer(b'\x00\x00\x01', data)]
    for i, s in enumerate(starts):
        e = starts[i + 1] - 3 if i + 1 < len(starts) else len(data)
        while e > s and data[e - 1] == 0:      # the 4-byte start code's zero
            e -= 1
        yield data[s:e]


def pictures(data):
    """[(param sets before it, [slices])] per picture."""
    pics, ps, cur = [], [], None
    for n in nals(data):
        t = (n[0] >> 1) & 0x3F
        if t <= 31:
            if len(n) > 2 and n[2] & 0x80:     # first_slice_segment_in_pic_flag
                cur = (ps, [n])
                pics.append(cur)
                ps = []
            elif cur:
                cur[1].append(n)
        elif t in (32, 33, 34):
            ps.append(n)
    return pics


def header(count, cap_us, fps=100, w=1920, h=1080):
    b = bytearray(32)
    b[0] = 0x80
    b[1] = 0xC2
    b[4] = 0xD2
    b[5] = 0x15
    b[9] = fps
    struct.pack_into('<HHHI', b, 12, count & 0xFFFF, w, h, cap_us & 0xFFFFFFFF)
    for i in range(22, 32):
        b[i] = 0x5A
    return bytes(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--rate', type=float, default=0.3,
                    help='chance that a picture is damaged (default 0.3)')
    ap.add_argument('--modes', default=','.join(MODES))
    ap.add_argument('--cut', action='store_true',
                    help='end the file part way through a picture')
    a = ap.parse_args()
    modes = a.modes.split(',')
    rnd = random.Random(a.seed)

    pics = pictures(open(a.src, 'rb').read())
    out = bytearray()
    sent = []                                  # (header, slices) as sent
    cap = 3193954415
    for i, (ps, slices) in enumerate(pics):
        cap += 10000
        hdr = header(i, cap)
        for p in ps:
            if rnd.random() < a.rate and 'params' in modes and len(p) > 4:
                p = p[:2] + bytes(rnd.randrange(256) for _ in range(len(p) - 2))
            out += SC + p
        slices = list(slices)
        damage = rnd.choice(modes) if rnd.random() < a.rate else None
        if damage == 'stale' and len(sent) > 50:
            old_hdr, old = sent[rnd.randrange(max(0, len(sent) - 3000), len(sent) - 20)]
            for s in (old if rnd.random() < 0.5 else [rnd.choice(old)]):
                out += SC + old_hdr + SC + s
        elif damage == 'mix' and len(sent) > 50 and len(slices) > 1:
            old_hdr, old = sent[rnd.randrange(max(0, len(sent) - 3000), len(sent) - 20)]
            if old:
                j = rnd.randrange(1, len(slices))
                for s in slices[:j]:
                    out += SC + hdr + SC + s
                out += SC + old_hdr + SC + rnd.choice(old)
                slices = slices[j:]
        elif damage == 'drop' and slices:
            del slices[rnd.randrange(len(slices))]
        elif damage == 'dup' and slices:
            j = rnd.randrange(len(slices))
            slices.insert(j, slices[j])
        elif damage == 'swap':
            slices.reverse()
        elif damage == 'trunc' and slices:
            j = rnd.randrange(len(slices))
            slices[j] = slices[j][:rnd.randrange(1, max(2, len(slices[j])))]
        elif damage == 'flip' and slices:
            j = rnd.randrange(len(slices))
            s = bytearray(slices[j])
            for _ in range(rnd.randrange(1, 8)):
                s[rnd.randrange(len(s))] ^= 1 << rnd.randrange(8)
            slices[j] = bytes(s)
        elif damage == 'junk':
            t = rnd.choice([rnd.randrange(64), 1, 19, 22, 33, 34])
            body = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 600)))
            slices.insert(rnd.randrange(len(slices) + 1), bytes([t << 1, 1]) + body)
        elif damage == 'header':
            hdr = header(rnd.randrange(65536), cap + rnd.randrange(-20000000, 45000000))
        for s in slices:
            out += SC + hdr + SC + s
        sent.append((hdr, slices))
    if a.cut and sent:
        last = sum(len(SC + hdr + SC + x) for x in slices)
        del out[len(out) - rnd.randrange(1, max(2, last)):]
    open(a.dst, 'wb').write(out)
    print(f'{len(pics)} pictures, {len(out)} bytes -> {a.dst}')


if __name__ == '__main__':
    main()
