#!/usr/bin/env python3
"""Summarise a kestrel latency trace (/tmp/ltrace.bin, see utils/ltrace.hpp).

    ssh root@192.168.3.1 'echo 20 > /tmp/ltrace.req'   # record 20 s
    ssh root@192.168.3.1 'cat /tmp/ltrace.bin' > lt.bin
    tools/ltrace.py lt.bin

Prints each ground stage of every picture, median / p90 / p99 / max in ms.
The capture stamp is on the air unit's clock, so "arrival - capture" is shown
above its own minimum: the offset between the two clocks is unknown here.
"""
import struct
import sys

HDR, SLICE, AU, DECPUT, DECOUT, RENDER, FLIP = 2, 3, 4, 5, 6, 7, 8


def load(path):
    b = open(path, 'rb').read()
    recs = []
    for i in range(0, len(b) - 55, 56):
        t, ev, n, _, a, bb = struct.unpack_from('<QBBHIQ', b, i)
        recs.append((t, ev, a, bb, b[i + 24:i + 24 + n]))
    recs.sort(key=lambda r: r[0])
    return recs


def pictures(recs):
    pics, order, renders = {}, [], {}
    cap, cur = None, None
    for t, ev, a, b, d in recs:
        if ev == HDR:
            cap = b
        elif ev == SLICE:
            if (b >> 32) & 0xff:
                cur = {'cap': cap, 'first': t, 'last': t}
            elif cur:
                cur['last'] = t
        elif ev == AU and cur:
            pics[a] = cur
            order.append(a)
            cur = None
        elif ev == DECPUT and a in pics:
            pics[a]['put'] = b
        elif ev == DECOUT and a in pics:
            pics[a]['out'] = t
        elif ev == RENDER and a in pics:
            pics[a]['render'] = t
            renders[b] = a
        elif ev == FLIP and a in renders:
            p = renders.pop(a)
            if p in pics:
                pics[p]['flip'] = b
    return [pics[p] for p in order]


def row(name, v):
    if not v:
        return
    v = sorted(v)
    q = lambda f: v[min(len(v) - 1, int(f * len(v)))]
    print(f'{name:30} n={len(v):5}  med {q(.5):6.2f}  p90 {q(.9):6.2f}'
          f'  p99 {q(.99):6.2f}  max {v[-1]:6.2f}')


def main():
    P = pictures(load(sys.argv[1]))
    rel = [p['first'] - p['cap'] for p in P if p['cap'] is not None]
    lo = min(rel) if rel else 0
    row('arrival - capture, above min', [(x - lo) / 1e3 for x in rel])
    stages = [('first slice -> last slice', 'first', 'last'),
              ('last slice -> decoder has it', 'last', 'put'),
              ('decode', 'put', 'out'),
              ('decoded -> flip submitted', 'out', 'render'),
              ('submitted -> vblank', 'render', 'flip'),
              ('first slice -> vblank', 'first', 'flip')]
    for name, a, b in stages:
        row(name, [(p[b] - p[a]) / 1e3 for p in P if a in p and b in p])
    # Early presentation: a picture flipped before the decoder handed it out
    # went up with only its top half decoded.
    shown = [p for p in P if 'first' in p and 'flip' in p and 'render' in p]
    early = [p for p in shown if 'out' in p and p['render'] < p['out']]
    if early:
        whole = [p for p in shown if p not in early]
        row('first slice -> vblank, early', [(p['flip'] - p['first']) / 1e3 for p in early])
        row('first slice -> vblank, whole', [(p['flip'] - p['first']) / 1e3 for p in whole])


if __name__ == '__main__':
    main()
