#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Weave QA: is the 3D weave correct, as a viewer's two eyes would see it?

A stereo camera (Acer SpatialLabs Eyes) stands in for the viewer. The run:
  1. makes the A/B test patterns (pattern.py);
  2. points Leia SR's eye tracker at the camera's lenses instead of a face
     (leiasr_target.py: an ini edit with a deadline, no admin; restored at the end);
  3. shows the patterns full screen through DisplayXR (cube_handle_d3d11_win with
     DISPLAYXR_WEAVE_PATTERN);
  4. at each camera position (positioner.py: none, manual, sim or a GRBL rail/gantry):
     moves the camera, waits for the tracker to settle, captures, and measures crosstalk
     and eye order per lens (analyse.py);
  5. writes report.json and exits 0 (pass), 1 (fail) or 2 (could not run).

    python weave_qa.py --app <cube_handle_d3d11_win.exe> --ffmpeg <ffmpeg.exe>
    python weave_qa.py --positioner grbl:COM5 --grbl-axes x=X --grbl-origin x=0 \
                       --positions "x=-150;x=0;x=150"
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyse  # noqa: E402
import capture  # noqa: E402
import pattern  # noqa: E402
from leiasr_target import StereoCameraTarget, TargetError  # noqa: E402
from positioner import open_positioner  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_APP = os.path.join(REPO, "test_apps", "build", "bin", "cube_handle_d3d11_win.exe")


def parse_axes(text):
    """'x=X,z=Y' -> {'x': 'X', 'z': 'Y'};  'x=0,z=600' -> {'x': 0.0, 'z': 600.0} (numbers)."""
    out = {}
    for part in filter(None, (text or "").split(",")):
        k, _, v = part.partition("=")
        try:
            out[k.strip()] = float(v)
        except ValueError:
            out[k.strip()] = v.strip()
    return out


def parse_positions(text):
    """'x=-150;x=0,z=600' -> [{'x': -150.0}, {'x': 0.0, 'z': 600.0}]; a .json file of the same;
    empty -> one position: wherever the camera is."""
    if not text:
        return [{}]
    if text.endswith(".json") and os.path.isfile(text):
        with open(text) as f:
            return json.load(f)
    return [{k: float(v) for k, v in parse_axes(p).items()} for p in text.split(";")]


def judge(rep, max_crosstalk):
    """-> (passed, reason) for one position's analysis report."""
    if "error" in rep:
        return False, rep["error"]
    worst = max(rep["lenses"][l]["crosstalk_pct"] for l in "LR")
    if rep["eye_order"] != "ok":
        return False, f"{rep['verdict']} (crosstalk {worst:.0f} %)"
    if worst > max_crosstalk:
        return False, f"crosstalk {worst:.1f} % > {max_crosstalk:.1f} %"
    return True, f"crosstalk L {rep['lenses']['L']['crosstalk_pct']} % R {rep['lenses']['R']['crosstalk_pct']} %"


def launch_app(app, patterns, runtime_json, period_ms, log):
    env = dict(os.environ)
    env["DISPLAYXR_WEAVE_PATTERN"] = ";".join(patterns)
    env["DISPLAYXR_WEAVE_PATTERN_PERIOD_MS"] = str(period_ms)
    if runtime_json:
        env["XR_RUNTIME_JSON"] = runtime_json
    log(f"starting {os.path.basename(app)}")
    return subprocess.Popen([app], env=env, cwd=os.path.dirname(app))


def run(a, log=print):
    out = a.out or os.path.join(tempfile.gettempdir(), "weave_qa", time.strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    pattern.set_levels(a.levels)
    patterns = pattern.write_patterns(os.path.join(out, "patterns"), a.levels)
    positions = parse_positions(a.positions)
    report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "out": out, "levels": a.levels,
              "max_crosstalk_pct": a.max_crosstalk, "positions": []}
    if not os.path.isfile(a.app):
        raise FileNotFoundError(f"test app not found: {a.app} (build it: scripts\\build_windows.bat test-apps)")
    capture.find_ffmpeg(a.ffmpeg)

    pos = open_positioner(a.positioner, log=log, **({"axes": parse_axes(a.grbl_axes) or None,
                                                       "origin": parse_axes(a.grbl_origin) or None}
                                                      if a.positioner.startswith("grbl") else {}))
    app = None
    target = None
    try:
        if a.home:
            pos.home()
        if not a.no_switch:
            target = StereoCameraTarget(minutes=a.minutes, log=log)
            target.__enter__()
        app = launch_app(a.app, patterns, a.runtime_json, a.period_ms, log)
        time.sleep(a.app_start_s)
        if app.poll() is not None:
            raise RuntimeError(f"the test app exited at start (code {app.returncode}); see its log in "
                               "%LOCALAPPDATA%\\DisplayXR")
        for i, where in enumerate(positions):
            name = f"pos{i + 1}_" + ("_".join(f"{k}{v:+.0f}" for k, v in where.items()) or "here")
            log(f"--- {name}")
            pos.move_to(**where)
            time.sleep(a.settle_s)               # the tracker re-finds the lenses and the weave follows
            d = os.path.join(out, name)
            n = capture.capture(d, a.seconds, camera=a.camera, ffmpeg=a.ffmpeg)
            rep = analyse.analyse(d)
            analyse.write_report(d, rep)
            ok, why = judge(rep, a.max_crosstalk)
            log(f"{'PASS' if ok else 'FAIL'} {name}: {why} ({n} frames)")
            report["positions"].append({"name": name, "where_mm": where, "passed": ok, "summary": why,
                                        "eye_order": rep.get("eye_order"), "verdict": rep.get("verdict"),
                                        "self_check_ok": rep.get("self_check_ok"),
                                        "crosstalk_pct": {l: rep.get("lenses", {}).get(l, {}).get("crosstalk_pct")
                                                          for l in "LR"},
                                        "by_colour": {l: rep.get("lenses", {}).get(l, {}).get("by_colour")
                                                      for l in "LR"}})
            if app.poll() is not None:
                raise RuntimeError(f"the test app exited during the run (code {app.returncode})")
    finally:
        if app is not None and app.poll() is None:
            app.terminate()
            try:
                app.wait(10)
            except subprocess.TimeoutExpired:
                app.kill()
        if target is not None:
            target.__exit__(None, None, None)
        pos.close()
    report["passed"] = bool(report["positions"]) and all(p["passed"] for p in report["positions"])
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    log(f"{'PASSED' if report['passed'] else 'FAILED'}: {os.path.join(out, 'report.json')}")
    return report


def main():
    ap = argparse.ArgumentParser(description="Weave QA: crosstalk and eye order through a stereo camera.")
    ap.add_argument("--app", default=DEFAULT_APP, help="cube_handle_d3d11_win.exe (pattern mode)")
    ap.add_argument("--runtime-json", help="XR_RUNTIME_JSON for the app (default: the active runtime)")
    ap.add_argument("--ffmpeg", help="ffmpeg.exe (default: WEAVE_QA_FFMPEG or PATH)")
    ap.add_argument("--camera", default="SpatialLabs Eyes", help="DirectShow name of the stereo camera")
    ap.add_argument("--out", help="output folder (default: %%TEMP%%\\weave_qa\\<time>)")
    ap.add_argument("--levels", choices=sorted(pattern.LEVELS), default="dim")
    ap.add_argument("--positions", help="'x=-150;x=0;x=150' (display mm) or a .json list; default: one, as is")
    ap.add_argument("--positioner", default="none", help="none | manual | sim | grbl:<port>")
    ap.add_argument("--grbl-axes", default="x=X", help="display axis -> GRBL axis, e.g. x=X,z=Y")
    ap.add_argument("--grbl-origin", default="", help="display position (mm) of machine zero, e.g. x=0,z=600")
    ap.add_argument("--home", action="store_true", help="home the positioner first ($H on GRBL)")
    ap.add_argument("--seconds", type=float, default=8.0, help="capture per position")
    ap.add_argument("--period-ms", type=int, default=3000, help="how long A and B are each shown")
    ap.add_argument("--settle-s", type=float, default=4.0, help="wait after each move before capturing")
    ap.add_argument("--app-start-s", type=float, default=8.0)
    ap.add_argument("--minutes", type=float, default=10.0, help="tracking_target_until: fall back to faces after")
    ap.add_argument("--max-crosstalk", type=float, default=15.0, help="fail above this, per lens (%%)")
    ap.add_argument("--no-switch", action="store_true", help="leave the tracker's target alone")
    a = ap.parse_args()
    try:
        report = run(a)
    except (TargetError, RuntimeError, FileNotFoundError, ValueError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
    sys.exit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
