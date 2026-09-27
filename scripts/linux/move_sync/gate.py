#!/usr/bin/env python3
# Extension v10 tag gate, per harness run (spec §9.9): per drag, painted frames
# held (sync=1) vs plain (sync=0); held frames off their woven origin; plain
# frames whose actor is NOT at the window; and holds / timeouts / tag-lost
# releases from the "move sync done" lines (DISPLAYXR_DEBUG=1, which run.sh
# sets). Pair with EXTRA_APP_ENV="DXR_WL_TEST_TAG=off|toggle:ON_MS:OFF_MS".
# usage: gate.py OUTDIR...
import re, sys
pat = re.compile(r'DXRSTAMP t=(\d+) pid=\d+ paint=(-?\d+),(-?\d+) woven=(\S+) seq=(-?\d+) actor=(\S+),(\S+) '
                 r'win=(-?\d+),(-?\d+) ptr=\S+ scale=\S+ sync=(\d)')
for out in sys.argv[1:]:
    lines = open(f'{out}/shell.log', errors='replace').read().splitlines()
    st = []
    for l in lines:
        m = pat.search(l)
        if m:
            g = m.groups()
            wv = tuple(int(v) for v in g[3].split(',')) if re.fullmatch(r'-?\d+,-?\d+', g[3]) else None
            st.append(dict(t=int(g[0]), paint=(int(g[1]), int(g[2])), woven=wv, actor=(float(g[5]), float(g[6])),
                           win=(int(g[7]), int(g[8])), sync=int(g[9])))
    done = [l for l in lines if 'move sync done' in l]
    tot = dict(holds=0, timeouts=0, tagLost=0)
    for l in done:
        tot['timeouts'] += int(re.search(r'(\d+) timeout', l).group(1))
        h = re.search(r'(\d+) hold\(s\)', l)
        tl = re.search(r'(\d+) tag-lost', l)
        tot['holds'] += int(h.group(1)) if h else 0
        tot['tagLost'] += int(tl.group(1)) if tl else 0
    started = sum(1 for l in lines if 'hold started mid-move' in l)
    print(f'== {out.rstrip("/").split("/")[-1]}: {len(done)} drag(s); holds={tot["holds"]} '
          f'(mid-move starts {started}) timeouts={tot["timeouts"]} tag-lost={tot["tagLost"]}')
    marks = []
    for l in open(f'{out}/marks.log'):
        d = dict(kv.split('=') for kv in l.split()[1:])
        d['kind'] = l.split()[0]
        marks.append(d)
    T = dict(held=0, heldRead=0, heldOff=0, plain=0, plainNotAtWin=0)
    for mk in marks:
        fr = [s for s in st if int(mk['press']) <= s['t'] <= int(mk['release']) + 700000]
        held = [s for s in fr if s['sync'] == 1]
        hr = [s for s in held if s['woven'] is not None]
        hoff = [s for s in hr if s['woven'] != s['paint']]
        plain = [s for s in fr if s['sync'] == 0]
        pnw = [s for s in plain if (round(s['actor'][0]), round(s['actor'][1])) != s['win']]
        print(f'  {mk["kind"]:15} frames {len(fr):4}  held {len(held):4} (read {len(hr):4}, off-origin {len(hoff)})  '
              f'plain {len(plain):4} (actor not at window {len(pnw)})')
        T['held'] += len(held)
        T['heldRead'] += len(hr)
        T['heldOff'] += len(hoff)
        T['plain'] += len(plain)
        T['plainNotAtWin'] += len(pnw)
    print(f'  TOTAL held {T["held"]} (read {T["heldRead"]}, off-origin {T["heldOff"]}); plain {T["plain"]} '
          f'(actor not at window {T["plainNotAtWin"]})')
