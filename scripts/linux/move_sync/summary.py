#!/usr/bin/env python3
# Aggregate per run: mismatch over all drags, lag slow/fast, judder, still%,
# across-track (slow straight drags), and direction flips on the reverse drag.
import json, subprocess, sys, re, math
import os
H = os.path.dirname(os.path.abspath(__file__))
rows = []
for out in sys.argv[1:]:
    r = json.loads(subprocess.run(['python3', f'{H}/analyze.py', out, '--json'], capture_output=True,
                                  text=True).stdout)
    res = {x['kind']: x for x in r['results']}
    allr = r['results']
    frames = sum(x['read'] for x in allr)
    mism = sum(x['mismatch'] for x in allr)
    phase = sum(round(x['phase_mismatch_pct'] * x['read'] / 100) for x in allr)
    # direction flips on the reverse drag (painted x changes sign of motion)
    flips = None
    stamps = [l for l in open(f'{out}/shell.log', errors='replace') if 'DXRSTAMP' in l]
    for line in open(f'{out}/marks.log'):
        if line.startswith('reverse'):
            d = dict(kv.split('=') for kv in line.split()[1:])
            xs = []
            for l in stamps:
                m = re.search(r't=(\d+) .*paint=(-?\d+),', l)
                if m and int(d['move']) <= int(m.group(1)) <= int(d['stop']) + 300000:
                    xs.append(int(m.group(2)))
            dirs = [(b > a) - (b < a) for a, b in zip(xs, xs[1:]) if b != a]
            flips = sum(1 for a, b in zip(dirs, dirs[1:]) if a != b)
    g = lambda k, f: res[k][f] if k in res else float('nan')
    rows.append((out, frames, 100 * mism / max(1, frames), 100 * phase / max(1, frames),
                 g('slow_h', 'mismatch_med'), g('fast_h', 'mismatch_med'),
                 g('slow_h', 'lag_med'), g('slow_h', 'lag_p90'), g('fast_h', 'lag_med'), g('fast_h', 'lag_p90'),
                 g('slow_h', 'judder_rms'), g('fast_h', 'judder_rms'), g('slow_h', 'still_pct'),
                 g('slow_d', 'across_max'), flips, r['timeouts']))
print('run                  frames mism% phase% mMedS mMedF lagS50 lagS90 lagF50 lagF90 jS   jF   still% acrossD flips tmo')
for x in rows:
    print(f'{x[0]:20} {x[1]:6} {x[2]:5.1f} {x[3]:6.1f} {x[4]:5.0f} {x[5]:5.0f} {x[6]:6.1f} {x[7]:6.1f} {x[8]:6.1f} '
          f'{x[9]:6.1f} {x[10]:4.1f} {x[11]:4.1f} {x[12]:6.1f} {x[13]:7.1f} {x[14]!s:>5} {x[15]:3}')
