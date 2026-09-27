#!/usr/bin/env python3
# Paint stalls DURING MOTION (move .. stop), per run: count and total ms of
# gaps > 60 ms, excluding the pre-motion press period common to every mode.
import re, sys
for out in sys.argv[1:]:
    ts = [int(m.group(1)) for l in open(f'{out}/shell.log', errors='replace')
          for m in [re.search(r'DXRSTAMP t=(\d+)', l)] if m]
    n = tot = 0
    motion = 0.0
    for line in open(f'{out}/marks.log'):
        d = dict(kv.split('=') for kv in line.split()[1:])
        a, b = int(d['move']), int(d['stop'])
        motion += (b - a) / 1e6
        fr = [t for t in ts if a <= t <= b]
        for x, y in zip(fr, fr[1:]):
            if y - x > 60000:
                n += 1
                tot += (y - x) / 1000
    print(f'{out:22} motion {motion:5.1f} s: {n:3} stalls >60 ms, {tot:6.0f} ms total ({100 * tot / 1000 / motion:4.1f}% of motion time)')
