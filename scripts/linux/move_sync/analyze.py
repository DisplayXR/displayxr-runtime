#!/usr/bin/env python3
# Per-drag metrics from a harness run: DXRSTAMP lines (shell.log) + marks.log.
# usage: analyze.py OUTDIR [--json]
import json, math, re, statistics, sys

out = sys.argv[1]
PERIOD = 5  # sim_display interlace period (device px), the phase model

stamps = []
pat = re.compile(r'DXRSTAMP t=(\d+) pid=\d+ paint=(-?\d+),(-?\d+) woven=(\S+) seq=(-?\d+) actor=(\S+),(\S+) '
                 r'win=(-?\d+),(-?\d+) ptr=(-?\d+),(-?\d+) scale=(\S+) sync=(\d)')
for line in open(f'{out}/shell.log', errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    g = m.groups()
    woven = None
    if re.fullmatch(r'-?\d+,-?\d+', g[3]):
        woven = tuple(int(v) for v in g[3].split(','))
    stamps.append(dict(t=int(g[0]), paint=(int(g[1]), int(g[2])), woven=woven, seq=int(g[4]),
                       actor=(float(g[5]), float(g[6])), win=(int(g[7]), int(g[8])), ptr=(int(g[9]), int(g[10])),
                       scale=float(g[11]), sync=int(g[12])))

sync_lines = [l for l in open(f'{out}/shell.log', errors='replace') if 'move sync done' in l]
timeouts = sum(int(re.search(r'(\d+) timeout', l).group(1)) for l in sync_lines)

marks = []
for line in open(f'{out}/marks.log'):
    parts = line.split()
    d = {'kind': parts[0]}
    for kv in parts[1:]:
        k, v = kv.split('=')
        d[k] = int(v)
    marks.append(d)

DIRS = {'slow_h': (1, 0), 'fast_h': (1, 0), 'back_h': (-1, 0), 'release_mid': (1, 0), 'rest_start': (1, 0),
        'slow_d': (0.94, 0.34), 'fast_d': (0.94, 0.34), 'reverse': (1, 0), 'tile_top': (0, -1), 'tile_left': (-1, 0)}


def pct(v, q):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


results = []
for mk in marks:
    base_kind = mk['kind'][4:] if mk['kind'].startswith('rmb_') else mk['kind']
    fr = [s for s in stamps if mk['press'] <= s['t'] <= mk['release'] + 700000]
    if not fr:
        continue
    scale = fr[0]['scale']
    read = [s for s in fr if s['woven'] is not None]
    mism = [s for s in read if s['woven'] != s['paint']]
    mags = [max(abs(s['paint'][0] - s['woven'][0]), abs(s['paint'][1] - s['woven'][1])) for s in mism]
    phase = [s for s in read if (s['paint'][0] - s['woven'][0]) % PERIOD != 0]
    # Pointer lag: where the content should be (pointer minus the grab
    # anchor taken before motion) vs where it is painted, device px.
    pre = [s for s in fr if s['t'] < mk['move']] or fr[:1]
    a0 = (pre[-1]['ptr'][0] - pre[-1]['actor'][0], pre[-1]['ptr'][1] - pre[-1]['actor'][1])
    moving = [s for s in fr if mk['move'] <= s['t'] <= mk['stop'] + 50000]
    lags = [math.hypot((s['ptr'][0] - a0[0] - s['actor'][0]) * scale, (s['ptr'][1] - a0[1] - s['actor'][1]) * scale)
            for s in moving]
    # Smoothness on straight drags: max sideways deviation of the painted
    # position from the drag line through the first painted position.
    ux, uy = DIRS.get(base_kind, (1, 0))
    n = math.hypot(ux, uy)
    ux, uy = ux / n, uy / n
    p0 = pre[-1]['paint']
    across = [abs(-(s['paint'][0] - p0[0]) * uy + (s['paint'][1] - p0[1]) * ux) for s in moving]
    along = [(s['paint'][0] - p0[0]) * ux + (s['paint'][1] - p0[1]) * uy for s in moving]
    back = sum(1 for a, b in zip(along, along[1:]) if b < a - 0.5)
    steps = [abs(b - a) for a, b in zip(along, along[1:])]
    # Judder: residual of the painted along-track position from a straight
    # constant-velocity fit over the steady part of the motion (skip 100 ms
    # at each end), device px. Only for single-direction drags.
    judder_rms = judder_max = float('nan')
    still = 0.0
    steady = [s for s in moving if mk['move'] + 100000 <= s['t'] <= mk['stop'] - 100000]
    if base_kind != 'reverse' and len(steady) > 5:
        ts = [s['t'] / 1e6 for s in steady]
        al = [(s['paint'][0] - p0[0]) * ux + (s['paint'][1] - p0[1]) * uy for s in steady]
        mt, ma = sum(ts) / len(ts), sum(al) / len(al)
        var = sum((t - mt) ** 2 for t in ts)
        k = sum((t - mt) * (a - ma) for t, a in zip(ts, al)) / var if var else 0
        res = [a - (ma + k * (t - mt)) for t, a in zip(ts, al)]
        judder_rms = math.sqrt(sum(r * r for r in res) / len(res))
        judder_max = max(abs(r) for r in res)
        still = 100.0 * sum(1 for a, b in zip(al, al[1:]) if abs(b - a) < 0.5) / max(1, len(al) - 1)
    r = dict(kind=mk['kind'], frames=len(fr), read=len(read), mismatch=len(mism),
             mismatch_pct=100.0 * len(mism) / max(1, len(read)),
             mismatch_med=statistics.median(mags) if mags else 0, mismatch_max=max(mags) if mags else 0,
             phase_mismatch_pct=100.0 * len(phase) / max(1, len(read)),
             lag_med=pct(lags, 0.5), lag_p90=pct(lags, 0.9), lag_max=max(lags) if lags else float('nan'),
             across_max=max(across) if across else float('nan'), back_steps=back,
             step_max=max(steps) if steps else float('nan'), moving_frames=len(moving),
             judder_rms=judder_rms, judder_max=judder_max, still_pct=still)
    results.append(r)

if '--json' in sys.argv:
    print(json.dumps(dict(results=results, timeouts=timeouts)))
else:
    print(f'{"drag":12} {"frames":>6} {"read":>5} {"mism%":>6} {"mMed":>5} {"mMax":>5} {"phase%":>7} '
          f'{"lagMed":>7} {"lagP90":>7} {"lagMax":>7} {"across":>7} {"back":>5} {"stepMax":>7} {"jRMS":>6} {"jMax":>6} {"still%":>6}')
    for r in results:
        print(f'{r["kind"]:12} {r["frames"]:6} {r["read"]:5} {r["mismatch_pct"]:6.1f} {r["mismatch_med"]:5.0f} '
              f'{r["mismatch_max"]:5.0f} {r["phase_mismatch_pct"]:7.1f} {r["lag_med"]:7.1f} {r["lag_p90"]:7.1f} '
              f'{r["lag_max"]:7.1f} {r["across_max"]:7.1f} {r["back_steps"]:5} {r["step_max"]:7.1f} '
              f'{r["judder_rms"]:6.1f} {r["judder_max"]:6.1f} {r["still_pct"]:6.1f}')
    print(f'move-sync timeouts: {timeouts}')
