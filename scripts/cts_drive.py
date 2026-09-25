#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""
Linux interactive-CTS driver (#1727) — the port of the Windows harness
(cts_step / cts_next / cts_drive .ps1, capture_dpi_aware.ps1, bar_profile.ps1,
sab_measure.py, eq_diff.py) to X11.

Start a category with run_cts.sh (it writes a .run file), then drive it from a
second shell, one verb per step:

  ./scripts/run_cts.sh -g vulkan --interactive composition --conformance-layer &
  RUN=$TMPDIR/interactive_composition_vulkan.run   # printed as RUNFILE: by run_cts.sh
  ./scripts/cts_drive.py --run $RUN shot gradients_1     # window + atlas PNGs
  ./scripts/cts_drive.py --run $RUN bar  <window png>    # gradient oracle
  ./scripts/cts_drive.py --run $RUN select --case "GradientFormats 1" --verdict PASS
  ./scripts/cts_drive.py --run $RUN tally

Input goes to the CTS window by XSendEvent by default (--route send): no focus
needed, nothing else on the desktop touched. --route xtest activates the window
(EWMH, the SetForegroundWindow twin) and injects real server input, parking the
pointer BEFORE any modifier goes down — never move the pointer while CTRL/ALT
is held (procedure §10.6): that translates a qwerty controller. Do not use
xtest under a GNOME Wayland session (consent dialog; see cts_x11.py).

THE ONE-CLICK-ONE-CASE CHECK (#1700). `select` and `fail` count the runtime's
[QTRACE] select edges (`XCB LMB DOWN edge`, `TRIGGER PRESS`) before and after the
click and report the delta together with whether the CTS window was replaced.
A delta other than exactly one press edge is printed as ANOMALY and returns 3 —
a phantom or dropped select makes every later verdict unattributable, so stop
the run and look.

Verbs:
  window                     current CTS window id + geometry
  wait-window [--other-than ID] [--timeout S]
  shot NAME                  window PNG (XGetImage) + atlas PNG (runtime capture)
  help NAME                  hold Menu, shot NAME_help, release (composition ONLY —
                             Menu is FAIL in MinLayers and SpaceOffsets, swap in
                             GripAndAimPose)
  select [--case C --verdict V --note N]   LMB (= PASS / advance)
  fail   [--case C --note N]               hold N, LMB, release N (= FAIL)
  keys SPEC...               e.g.  ctrl+alt+e:500   shift+Up:300   r:80
                             (hold the chord for the given ms, release in reverse)
  raise-hands                CTRL+ALT then E ~500 ms (QuadHands, §10.6)
  space-offsets              the six-axis velocity recipe (§10.8, #1696)
  record --case C --verdict V [--note N] [--shot P]   log without pressing
  tally                      print the per-case log
  bar PNG                    gradient-pair oracle (GradientFormatsLinearVsNonLinear)
  sab PNG                    source-alpha-blending oracle (three columns / two squares)
  diff A.png B.png           per-pixel difference (equirect 5 vs 6, lane-vs-lane)
  probe X,Y [X,Y ...] [--samples 16 --interval 0.25]
                             sample window pixels over time (StaleSwapchain: the
                             left square must never change, the right toggles 1 Hz)
  haptics                    haptic-output lines from the runtime log
  respond [--idle-exit S]    the [actions] auto-responder (port of the Windows DBWIN
                             cts_actions_responder.ps1): follows the CTS's live
                             "Interaction message:" lines in the stdout log and
                             performs each prompted input on the named hand —
                             Press/Release/Set <path>, "Use all controller inputs",
                             and the haptic confirmation (clicks only AFTER the
                             runtime logged its "Haptic output" line). Runs until
                             the CTS exits. Needs run_cts.sh's line-buffered stdout.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cts_x11  # noqa: E402

FRAME_CLASS = b"mutter-x11-frames"  # GNOME's frame windows copy the client title
# One WARN per session the vk_native compositor draws for. The window XID is
# NOT a session marker: a new session's window routinely reuses the old XID.
SESSION_MARKER = "VK compose: draw path ready"


# ---------------------------------------------------------------------------
# run state
# ---------------------------------------------------------------------------

class Run:
    def __init__(self, path):
        self.path = path
        self.env = {}
        with open(path) as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    self.env[k] = v
        self.dir = os.path.dirname(os.path.abspath(path))
        self.stem = self.env["STEM"]
        self.shots = os.path.join(self.dir, self.stem + "_shots")
        os.makedirs(self.shots, exist_ok=True)
        self.tally = os.path.join(self.dir, self.stem + "_cases.tsv")
        self.x = cts_x11.X11(self.env.get("DISPLAY") or os.environ.get("DISPLAY"))

    def scan(self):
        """Incremental scan of the runtime stderr log. Under DXR_QTRACE it grows
        by per-frame view traces (hundreds of MB an hour), so only the new bytes
        are read and only the counters the driver needs are kept, in a small
        JSON state file beside the run."""
        import json

        st_path = os.path.join(self.dir, self.stem + "_scan.json")
        st = {"off": 0, "edges": 0, "trigger": 0, "dropped": 0, "qd": 0, "qd_last": "",
              "haptic": [], "errors": 0, "sessions": 0}
        try:
            with open(st_path) as f:
                st.update(json.load(f))
        except (OSError, ValueError):
            pass
        try:
            with open(self.env["RUNTIME_LOG"], "rb") as f:
                f.seek(st["off"])
                chunk = f.read()
        except OSError:
            return st
        # only consume whole lines
        cut = chunk.rfind(b"\n") + 1
        text = chunk[:cut].decode("utf-8", "replace")
        st["off"] += cut
        for l in text.splitlines():
            if "[QTRACE]" in l:
                if "XCB LMB DOWN edge" in l:
                    st["edges"] += 1
                elif "XCB LMB DOWN dropped" in l:
                    st["dropped"] += 1
                elif "TRIGGER PRESS" in l:
                    st["trigger"] += 1
                elif "[QTRACE] QD" in l:
                    st["qd"] += 1
                    st["qd_last"] = l.strip()
            elif SESSION_MARKER in l:
                st["sessions"] += 1
            elif "aptic" in l:
                st["haptic"] = (st["haptic"] + [l.strip()])[-50:]
            elif l.startswith("ERROR"):
                st["errors"] += 1
        with open(st_path, "w") as f:
            json.dump(st, f)
        return st

    # ---- windows -------------------------------------------------------------
    def windows(self):
        out = []
        for w in self.x.find("DisplayXR"):
            if not self.x.viewable(w):
                continue
            cls, _, _ = self.x._prop(w, "WM_CLASS")
            if cls is not None and isinstance(cls, bytes) and cls.startswith(FRAME_CLASS):
                continue
            out.append(w)
        return out

    def window(self):
        ws = self.windows()
        return max(ws) if ws else None

    def wait_window(self, other_than=None, timeout=30.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            w = self.window()
            if w is not None and w != other_than:
                return w
            time.sleep(0.1)
        return None

    # ---- counters --------------------------------------------------------------
    def select_edges(self):
        st = self.scan()
        return (st["edges"], st["trigger"], st["dropped"])

    def seq(self):
        n = len([f for f in os.listdir(self.shots) if f.endswith("_win.png")])
        return n + 1


# ---------------------------------------------------------------------------
# key chords
# ---------------------------------------------------------------------------

KEYSYM_ALIAS = {
    "ctrl": "Control_L", "control": "Control_L", "alt": "Alt_L", "shift": "Shift_L",
    "up": "Up", "down": "Down", "left": "Left", "right": "Right", "esc": "Escape",
}


def keysym(tok):
    t = KEYSYM_ALIAS.get(tok.lower(), tok)
    return t.lower() if len(t) == 1 else t


def chord(run, win, spec, route):
    """'ctrl+alt+e:500' — press left to right, hold, release right to left."""
    keys, _, ms = spec.partition(":")
    hold = int(ms) if ms else 80
    syms = [keysym(k) for k in keys.split("+") if k]
    for s in syms:
        run.x.key(win, s, True, route)
        time.sleep(0.03)
    time.sleep(hold / 1000.0)
    for s in reversed(syms):
        run.x.key(win, s, False, route)
        time.sleep(0.03)


def prepare(run, route):
    win = run.window()
    if win is None:
        raise SystemExit("no CTS window")
    if route == "xtest":  # never under GNOME Wayland — see cts_x11.py
        if not run.x.activate(win):
            print("WARN: window 0x%08x did not take X focus (focus=0x%08x)" % (win, run.x.focus()))
        run.x.park_pointer(win)
        time.sleep(0.08)
    return win


# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------

def atlas_capture(run, dst, timeout=4.0):
    cap = run.env.get("CAPTURE_DIR")
    if not cap:
        return None
    trig = os.path.join(cap, "displayxr_atlas_trigger.conformance_cli")
    out = os.path.join(cap, "displayxr_atlas.conformance_cli.png")
    try:
        os.unlink(out)
    except OSError:
        pass
    open(trig, "w").close()
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(out) and not os.path.exists(trig):
            time.sleep(0.2)  # let the PNG writer finish
            os.replace(out, dst)
            return dst
        time.sleep(0.05)
    try:
        os.unlink(trig)
    except OSError:
        pass
    return None


def shot(run, name, atlas=True):
    win = run.window()
    if win is None:
        print("shot: no CTS window")
        return None
    base = os.path.join(run.shots, "%03d_%s" % (run.seq(), re.sub(r"[^A-Za-z0-9_.-]", "_", name)))
    wpng = base + "_win.png"
    ok = run.x.shot(win, wpng)
    print("window: %s" % (wpng if ok else "FAILED"))
    if atlas:
        a = atlas_capture(run, base + "_atlas.png")
        print("atlas:  %s" % (a or "(no capture — trigger not consumed)"))
    return wpng


# ---------------------------------------------------------------------------
# verdict log
# ---------------------------------------------------------------------------

def record(run, case, verdict, note="", shotpath=""):
    new = not os.path.exists(run.tally)
    with open(run.tally, "a") as f:
        if new:
            f.write("time\tcase\tverdict\tnote\tshot\n")
        f.write("%s\t%s\t%s\t%s\t%s\n" % (time.strftime("%H:%M:%S"), case, verdict, note,
                                           os.path.basename(shotpath or "")))
    print("recorded: %s = %s" % (case, verdict))


def press_select(run, route, with_menu=False, advance_timeout=6.0):
    before = run.select_edges()
    win = prepare(run, route)
    if with_menu:
        run.x.key(win, "n", True, route)
        time.sleep(0.6)
    run.x.button(win, 1, True, route)
    time.sleep(0.08)
    run.x.button(win, 1, False, route)
    if with_menu:
        time.sleep(0.1)
        run.x.key(win, "n", False, route)
    t0 = time.time()
    replaced = False
    while time.time() - t0 < advance_timeout:
        cur = run.window()
        if cur != win:
            replaced = True
            break
        time.sleep(0.1)
    time.sleep(0.3)
    after = run.select_edges()
    d = [a - b for a, b in zip(after, before)]
    anomaly = d[0] != 1 or d[2] != 0
    print("select: xcb-edges +%d  trigger-press +%d  dropped +%d  window %s%s" %
          (d[0], d[1], d[2], "replaced" if replaced else "UNCHANGED",
           "   ANOMALY — not exactly one select edge" if anomaly else ""))
    return (3 if anomaly else 0), replaced


# ---------------------------------------------------------------------------
# oracles
# ---------------------------------------------------------------------------

def _gray(path):
    from PIL import Image

    im = Image.open(path).convert("L")
    return im, im.load()


def bar_oracle(path):
    """Two horizontal gradient bars (projection above, quad below). Find the
    bands, profile each band's centre row, compare them column by column.
    Prints: extents, 5-point profiles, mid value, max deviation from a straight
    line in ENCODED values (a perceptually linear ramp is straight here; a
    gamma-shaped error bows it), and the mean/max |upper - lower|."""
    im, px = _gray(path)
    W, H = im.size
    rows = []
    for y in range(H):
        xs = [x for x in range(W // 5, 4 * W // 5) if px[x, y] > 3]
        if len(xs) < W // 12:
            rows.append(None)
            continue
        vals = [px[x, y] for x in xs]
        # a ramp: many distinct values, left darker than right
        if len(set(vals)) < 40 or vals[len(vals) // 10] >= vals[-len(vals) // 10]:
            rows.append(None)
            continue
        rows.append((xs[0], xs[-1]))
    bands = []
    y = 0
    while y < H:
        if rows[y] is None:
            y += 1
            continue
        y0 = y
        while y < H and rows[y] is not None:
            y += 1
        if y - y0 >= 6:
            bands.append((y0, y - 1))
    if len(bands) < 2:
        print("bar: found %d gradient band(s) — cannot judge (%s)" % (len(bands), bands))
        return 2
    bands = sorted(sorted(bands, key=lambda b: b[1] - b[0], reverse=True)[:2])
    profs = []
    for (y0, y1) in bands:
        yc = (y0 + y1) // 2
        x0, x1 = rows[yc]
        profs.append((yc, x0, x1))
    x0 = max(p[1] for p in profs)
    x1 = min(p[2] for p in profs)
    out = []
    for (yc, a, b) in profs:
        prof = [px[x, yc] for x in range(x0, x1 + 1)]
        n = len(prof)
        lo, hi = prof[0], prof[-1]
        lin = max(abs(prof[i] - (lo + (hi - lo) * i / (n - 1))) for i in range(n))
        five = [prof[int(i * (n - 1) / 4)] for i in range(5)]
        out.append(prof)
        print("bar y=%d  extent %d..%d  profile %s  mid %d  lin-dev %.1f" %
              (yc, a, b, five, prof[n // 2], lin))
    diffs = [abs(p - q) for p, q in zip(out[0], out[1])]
    mean = sum(diffs) / len(diffs)
    print("bar match: mean |upper-lower| %.2f  max %d  over x %d..%d" % (mean, max(diffs), x0, x1))
    verdict = "MATCH" if mean <= 2.0 else "DIFFER"
    print("bar verdict: %s (rule: mean <= 2.0; judge shape by lin-dev + mid)" % verdict)
    return 0


def sab_oracle(path):
    """Coloured-square census: every connected-ish non-black block, its bbox and
    its mean colour along its centre row. For SourceAlphaBlending the three
    columns must agree; for ...WithEnvironment report the two right-column
    squares (the Windows reference is 254 / 155 on the white one's channel) and
    whether any RED pixel exists (red must never be visible)."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()
    red = sum(1 for y in range(0, H, 2) for x in range(0, W, 2)
              if px[x, y][0] > 150 and px[x, y][1] < 60 and px[x, y][2] < 60)
    print("sab: red-ish pixels (sampled every 2nd px): %d" % red)
    # column occupancy of saturated / non-black pixels in the middle band
    ys = range(H // 4, 3 * H // 4)
    occ = []
    for x in range(W):
        c = 0
        for y in ys[::4]:
            r, g, b = px[x, y]
            if max(r, g, b) > 20 and not (abs(r - g) < 12 and abs(g - b) < 12 and r > 200):
                c += 1
        occ.append(c)
    spans = []
    x = 0
    while x < W:
        if occ[x] < 3:
            x += 1
            continue
        x0 = x
        while x < W and occ[x] >= 3:
            x += 1
        if x - x0 > 20:
            spans.append((x0, x - 1))
    for (x0, x1) in spans:
        xc = (x0 + x1) // 2
        col = [px[xc, y] for y in ys]
        nz = [i for i, c in enumerate(col) if max(c) > 20]
        if not nz:
            continue
        ya, yb = ys[0] + nz[0], ys[0] + nz[-1]
        samples = [px[xc, ya + int((yb - ya) * t)] for t in (0.1, 0.3, 0.5, 0.7, 0.9)]
        print("sab column x=%d..%d  y=%d..%d  centre-column samples %s" % (x0, x1, ya, yb, samples))
    return 0


def diff_oracle(a, b):
    from PIL import Image, ImageChops

    A = Image.open(a).convert("RGB")
    B = Image.open(b).convert("RGB")
    if A.size != B.size:
        B = B.resize(A.size)
        print("diff: resized %s to %s" % (b, A.size))
    d = ImageChops.difference(A, B).convert("L")
    hist = d.histogram()
    n = A.size[0] * A.size[1]
    over = sum(hist[9:])
    mean = sum(i * h for i, h in enumerate(hist)) / n
    print("diff: mean %.3f  pixels >8: %d (%.3f %%)" % (mean, over, 100.0 * over / n))
    return 0


def probe(run, points, samples, interval):
    import tempfile

    from PIL import Image

    win = run.window()
    if win is None:
        print("probe: no CTS window")
        return 1
    tmp = os.path.join(tempfile.mkdtemp(prefix="cts_probe_"), "p.png")
    series = {pt: [] for pt in points}
    t0 = time.time()
    for i in range(samples):
        if run.x.shot(win, tmp):
            im = Image.open(tmp).convert("RGB")
            for pt in points:
                series[pt].append(im.getpixel(pt))
        time.sleep(max(0.0, t0 + (i + 1) * interval - time.time()))
    for pt, vals in series.items():
        distinct = sorted(set(vals))
        print("probe %s: %d samples over %.1f s, %d distinct: %s" %
              (pt, len(vals), time.time() - t0, len(distinct), distinct))
        print("         sequence: %s" % " ".join("%d,%d,%d" % v for v in vals))
    return 0


# ---------------------------------------------------------------------------
# recipes
# ---------------------------------------------------------------------------

def raise_hands(run, route):
    """§10.6: CTRL+ALT (both hands focused), E ~500 ms (+0.30 m at 0.6 m/s),
    release E, then ALT, then CTRL. Pointer parked first by prepare()."""
    win = prepare(run, route)
    before = run.scan()["qd"]
    run.x.key(win, "Control_L", True, route)
    time.sleep(0.05)
    run.x.key(win, "Alt_L", True, route)
    time.sleep(0.1)
    run.x.key(win, "e", True, route)
    time.sleep(0.5)
    run.x.key(win, "e", False, route)
    time.sleep(0.05)
    run.x.key(win, "Alt_L", False, route)
    time.sleep(0.05)
    run.x.key(win, "Control_L", False, route)
    time.sleep(0.3)
    st = run.scan()
    print("raise-hands: %d QD trace lines; last: %s" % (st["qd"] - before, st["qd_last"] or "-"))


def space_offsets(run, route):
    """§10.8 / #1696: CTRL+ALT held throughout; D, E, S linear; R then
    SHIFT+Up, R then SHIFT+Left, R then SHIFT+Z angular. ~300 ms each.
    The test ends itself the moment the last criterion latches, so every step
    first checks the session is still the one it started on — otherwise the
    rest of the recipe lands in the NEXT test's window (same XID)."""
    win = prepare(run, route)
    s0 = run.scan()["sessions"]

    def alive():
        return run.window() == win and run.scan()["sessions"] == s0

    run.x.key(win, "Control_L", True, route)
    time.sleep(0.05)
    run.x.key(win, "Alt_L", True, route)
    time.sleep(0.1)

    def hold(syms, ms=300):
        for s in syms:
            run.x.key(win, s, True, route)
            time.sleep(0.02)
        time.sleep(ms / 1000.0)
        for s in reversed(syms):
            run.x.key(win, s, False, route)
            time.sleep(0.02)
        time.sleep(0.25)

    steps = [(["d"], 300), (["e"], 300), (["s"], 300),
             (["r"], 80), (["Shift_L", "Up"], 300),
             (["r"], 80), (["Shift_L", "Left"], 300),
             (["r"], 80), (["Shift_L", "z"], 300), (["r"], 80)]
    sent = 0
    for syms, ms in steps:
        if not alive():
            break
        hold(syms, ms)
        sent += 1
    if alive():
        run.x.key(win, "Alt_L", False, route)
        time.sleep(0.05)
        run.x.key(win, "Control_L", False, route)
    ended = not alive()
    print("space-offsets: %d/%d steps sent; session %s" %
          (sent, len(steps), "ENDED (test completed itself)" if ended else "still running"))


# ---------------------------------------------------------------------------
# [actions] auto-responder
# ---------------------------------------------------------------------------

_ANSI = re.compile(r"\x1b\[[0-9;]*m")

# component -> ("key", keysym) | ("button", n); qwerty's map (qwerty_xcb.c).
_COMPONENT_INPUT = [
    ("/select/", ("button", 1)), ("/trigger/", ("button", 1)),
    ("/menu/", ("key", "n")), ("/squeeze/", ("button", 2)),
    ("/system/", ("key", "b")), ("/thumbstick/click", ("key", "v")),
]
_VECTOR_KEYS = {"up": "t", "left": "f", "down": "g", "right": "h"}


class Responder:
    def __init__(self, run, route):
        self.run = run
        self.route = route
        self.out_path = run.env["STDOUT_LOG"]
        self.off = 0
        self.msg = ""
        self.msg_seq = 0
        self.held = []  # ("key", sym) / ("button", n), in press order
        self.answered = 0
        self.log = open(os.path.join(run.dir, run.stem + "_responder.log"), "a")

    def note(self, text):
        line = "%s %s" % (time.strftime("%H:%M:%S"), text)
        print(line, flush=True)
        self.log.write(line + "\n")
        self.log.flush()

    # ---- prompt feed ------------------------------------------------------
    def poll(self):
        try:
            with open(self.out_path, "rb") as f:
                f.seek(self.off)
                chunk = f.read()
        except OSError:
            return False
        if not chunk:
            return False
        cut = chunk.rfind(b"\n") + 1
        if cut == 0:
            return False
        self.off += cut
        text = _ANSI.sub("", chunk[:cut].decode("utf-8", "replace"))
        changed = False
        parts = text.split("Interaction message: ")
        for part in parts[1:]:
            lines = part.split("\n")
            msg = [lines[0]]
            # a message may carry its own newlines ("...inputs on\n/user/hand/left",
            # "Set X\nExpected: v", "e.g.:\non /user/...")
            for extra in lines[1:4]:
                if extra.startswith(("/user/", "Expected", "on ", "true", "false")):
                    msg.append(extra)
                else:
                    break
            m = "\n".join(msg).strip()
            if m != self.msg:
                self.msg = m
                self.msg_seq += 1
                changed = True
        return changed

    # ---- input ------------------------------------------------------------
    def win(self):
        return self.run.window()

    def press(self, what, down):
        w = self.win()
        if w is None:
            return
        kind, v = what
        if kind == "key":
            self.run.x.key(w, v, down, self.route)
        else:
            self.run.x.button(w, v, down, self.route)
        if down:
            self.held.append(what)
        elif what in self.held:
            self.held.remove(what)

    def release_all(self):
        for what in reversed(list(self.held)):
            self.press(what, False)
            time.sleep(0.02)
        self.held = []

    @staticmethod
    def modifier(msg):
        # The tested hand is named in the prompt, either as a path
        # ("/user/hand/left") or, in the haptic prompt, by the localized
        # device name ("on Left Khronos Simple Controller: Menu, Select").
        # left -> CTRL, right -> ALT (qwerty's controller focus).
        if "/user/hand/left" in msg or re.search(r"\bLeft\b", msg):
            return ("key", "Control_L")
        if "/user/hand/right" in msg or re.search(r"\bRight\b", msg):
            return ("key", "Alt_L")
        return None

    @staticmethod
    def component(path):
        for frag, inp in _COMPONENT_INPUT:
            if frag in path:
                return inp
        return None

    def chord(self, mod, inputs, hold=0.15):
        if mod:
            self.press(mod, True)
            time.sleep(0.05)
        for i in inputs:
            self.press(i, True)
            time.sleep(0.03)
        time.sleep(hold)
        self.release_all()
        time.sleep(0.08)

    # ---- prompt handlers ----------------------------------------------------
    def handle(self):
        msg = self.msg
        seq = self.msg_seq
        first = msg.split("\n")[0]
        mod = self.modifier(msg)
        if first.startswith("Press ") and "/user/" in first:
            inp = self.component(first)
            if inp is None:
                self.note("UNANSWERABLE (no qwerty input): %r" % msg)
                return
            self.release_all()
            if mod:
                self.press(mod, True)
                time.sleep(0.05)
            self.press(inp, True)
            self.answered += 1
            self.note("press %s %s for %r" % (mod[1] if mod else "-", inp[1], first))
        elif first.startswith("Release ") and "/user/" in first:
            self.release_all()
            self.answered += 1
            self.note("release all for %r" % first)
        elif first.startswith("Set ") and "/user/" in first:
            exp = msg.split("Expected:")[-1].strip() if "Expected:" in msg else ""
            self.release_all()
            if exp.startswith("("):
                x, y = [float(v) for v in exp.strip("()").split(",")]
                keys = []
                if y > 0.5: keys.append(("key", _VECTOR_KEYS["up"]))
                if y < -0.5: keys.append(("key", _VECTOR_KEYS["down"]))
                if x > 0.5: keys.append(("key", _VECTOR_KEYS["right"]))
                if x < -0.5: keys.append(("key", _VECTOR_KEYS["left"]))
            else:
                inp = self.component(first)
                keys = [inp] if (inp and exp and float(exp) > 0.5) else []
            if keys:
                if mod:
                    self.press(mod, True)
                    time.sleep(0.05)
                for k in keys:
                    self.press(k, True)
            self.answered += 1
            self.note("set %r -> %s" % (first, [k[1] for k in keys] or "released"))
        elif first.startswith("Use all controller inputs") or first.startswith("Used "):
            # Cycle every boolean the rig has, one at a time and combined, with
            # the hand's modifier, until the prompt moves on. Poses are always
            # tracked on qwerty; nothing to do for them.
            self.answered += 1
            self.note("use-all on %s: cycling select / menu / select+menu" % (mod[1] if mod else "?"))
            t0 = time.time()
            while seq == self.msg_seq or self.msg.startswith("Used "):
                for combo in ([("button", 1)], [("key", "n")], [("button", 1), ("key", "n")]):
                    # re-read the hand every chord: the same prompt family
                    # moves from /user/hand/left to /user/hand/right
                    self.chord(self.modifier(self.msg), combo)
                    self.poll()
                if not (self.msg.startswith("Used ") or self.msg.startswith("Use all")):
                    break
                if time.time() - t0 > 120:
                    self.note("use-all: still unsatisfied after 120 s: %r" % self.msg)
                    break
            # the prompt that ended the cycle was consumed by our own poll()
            if not self.msg.startswith(("Used ", "Use all")):
                self.handle()
        elif first.startswith("Activate any boolean"):
            self.release_all()
            h0 = self.run.scan()["haptic"]
            n0 = len(h0)
            t0 = time.time()
            got = None
            while time.time() - t0 < 12:
                h = self.run.scan()["haptic"]
                if len(h) > n0 or (h and h0 and h[-1] != h0[-1]):
                    got = h[-1]
                    break
                time.sleep(0.05)
            if got is None:
                self.note("HAPTIC NOT OBSERVED within 12 s — not answering %r" % first)
                return
            self.chord(mod, [("button", 1)], hold=0.2)
            self.answered += 1
            self.note("haptic seen (%s) -> select on %s" % (got.strip()[:60], mod[1] if mod else "?"))
        else:
            # Place/Keep/Wait/Waiting/Turn: hands off (qwerty is already static
            # and trackable, procedure §8.7); never hold anything across these.
            self.release_all()
            self.note("idle for %r" % first.replace("\n", " | "))

    def loop(self, idle_exit):
        last = time.time()
        while True:
            if self.poll():
                last = time.time()
                self.handle()
            if self.run.window() is None and time.time() - last > idle_exit:
                self.release_all()
                self.note("no CTS window for %.0f s — responder exiting (%d prompts answered)" %
                          (idle_exit, self.answered))
                return 0
            time.sleep(0.05)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", help=".run file written by run_cts.sh --interactive")
    ap.add_argument("--route", choices=("send", "xtest"), default="send",
                    help="input route; see cts_x11.py (xtest opens a consent dialog under GNOME Wayland)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("window")
    p = sub.add_parser("wait-window")
    p.add_argument("--other-than", type=lambda s: int(s, 0))
    p.add_argument("--timeout", type=float, default=30.0)
    p = sub.add_parser("shot")
    p.add_argument("name")
    p.add_argument("--no-atlas", action="store_true")
    p = sub.add_parser("help")
    p.add_argument("name")
    for c in ("select", "fail"):
        p = sub.add_parser(c)
        p.add_argument("--case", default="")
        p.add_argument("--verdict", default="PASS" if c == "select" else "FAIL")
        p.add_argument("--note", default="")
        p.add_argument("--shot", default="")
    p = sub.add_parser("keys")
    p.add_argument("specs", nargs="+")
    sub.add_parser("raise-hands")
    sub.add_parser("space-offsets")
    p = sub.add_parser("record")
    p.add_argument("--case", required=True)
    p.add_argument("--verdict", required=True)
    p.add_argument("--note", default="")
    p.add_argument("--shot", default="")
    sub.add_parser("tally")
    p = sub.add_parser("bar")
    p.add_argument("png")
    p = sub.add_parser("sab")
    p.add_argument("png")
    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p = sub.add_parser("probe")
    p.add_argument("points", nargs="+")
    p.add_argument("--samples", type=int, default=16)
    p.add_argument("--interval", type=float, default=0.25)
    p = sub.add_parser("respond")
    p.add_argument("--idle-exit", type=float, default=45.0)
    p = sub.add_parser("haptics")
    p.add_argument("--tail", type=int, default=20)
    a = ap.parse_args(argv)

    if a.cmd == "bar":
        return bar_oracle(a.png)
    if a.cmd == "sab":
        return sab_oracle(a.png)
    if a.cmd == "diff":
        return diff_oracle(a.a, a.b)
    if not a.run:
        ap.error("--run is required for %s" % a.cmd)
    run = Run(a.run)

    if a.cmd == "window":
        w = run.window()
        if w is None:
            print("no CTS window")
            return 1
        print("0x%08x %s" % (w, " ".join(map(str, run.x.geom(w)))))
    elif a.cmd == "wait-window":
        w = run.wait_window(a.other_than, a.timeout)
        if w is None:
            print("timeout: no (new) CTS window")
            return 1
        print("0x%08x" % w)
    elif a.cmd == "shot":
        shot(run, a.name, atlas=not a.no_atlas)
    elif a.cmd == "help":
        win = prepare(run, a.route)
        run.x.key(win, "n", True, a.route)
        time.sleep(1.0)
        shot(run, a.name + "_help", atlas=False)
        run.x.key(win, "n", False, a.route)
    elif a.cmd in ("select", "fail"):
        rc, _ = press_select(run, a.route, with_menu=(a.cmd == "fail"))
        if a.case and rc == 0:
            record(run, a.case, a.verdict, a.note, a.shot)
        elif a.case:
            print("NOT recorded: %s — resolve the select anomaly first" % a.case)
        return rc
    elif a.cmd == "keys":
        win = prepare(run, a.route)
        for s in a.specs:
            chord(run, win, s, a.route)
        print("keys: sent %s" % " ".join(a.specs))
    elif a.cmd == "raise-hands":
        raise_hands(run, a.route)
    elif a.cmd == "space-offsets":
        space_offsets(run, a.route)
    elif a.cmd == "record":
        record(run, a.case, a.verdict, a.note, a.shot)
    elif a.cmd == "tally":
        if os.path.exists(run.tally):
            print(open(run.tally).read(), end="")
    elif a.cmd == "probe":
        return probe(run, [tuple(int(v) for v in pt.split(",")) for pt in a.points], a.samples, a.interval)
    elif a.cmd == "respond":
        return Responder(run, a.route).loop(a.idle_exit)
    elif a.cmd == "haptics":
        lines = run.scan()["haptic"]
        print("\n".join(lines[-a.tail:]) or "(no haptic lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
