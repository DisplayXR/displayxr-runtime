# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Point Leia SR's eye tracker at the stereo camera, without admin rights, and back.

Needs an SR Eye Tracker that follows tracking_target while running: LeiaSR with
LeiaInc/LeiaSR#948 and the system environment variable LEIASR_LIVE_TRACKING_TARGET=1
(set once on the QA machine). Such a tracker writes
    %ProgramData%/Simulated Reality/Devices/<serial>/config/tracking_target.status
on start and on every switch, which is how this module knows the switch has happened.

    with StereoCameraTarget(minutes=10) as target:   # tracker follows the camera's lenses
        ...capture...
    # back to faces here, even after an exception; and the tracker returns to faces by
    # itself once the deadline passes, even if this process is killed.
"""
import glob
import os
import re
import time

STORE = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "Simulated Reality", "Devices")


class TargetError(RuntimeError):
    pass


def config_dirs():
    """config folders of every SR device on this machine."""
    return sorted(d for d in glob.glob(os.path.join(STORE, "*", "config")) if os.path.isdir(d))


def read_status(config_dir):
    """tracking_target.status as a dict, or None when the tracker has not written one."""
    path = os.path.join(config_dir, "tracking_target.status")
    try:
        with open(path, encoding="utf-8") as f:
            return dict(line.strip().split("=", 1) for line in f if "=" in line)
    except OSError:
        return None


def _set_service_keys(text, values):
    """Set keys in [Service] of an ini text (adding the section or keys as needed)."""
    lines = text.splitlines()
    out, in_service, done, seen_service = [], False, set(), False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if in_service:
                out += [f"{k} = {v}" for k, v in values.items() if k not in done]
                done |= set(values)
            in_service = stripped.lower() == "[service]"
            seen_service = seen_service or in_service
            out.append(line)
            continue
        key = re.match(r"\s*([A-Za-z0-9_]+)\s*=", line)
        if in_service and key and key.group(1) in values:
            if key.group(1) not in done:
                out.append(f"{key.group(1)} = {values[key.group(1)]}")
                done.add(key.group(1))
            continue
        out.append(line)
    if in_service:
        out += [f"{k} = {v}" for k, v in values.items() if k not in done]
    elif not seen_service:
        out += ["", "[Service]"] + [f"{k} = {v}" for k, v in values.items()]
    return "\n".join(out) + "\n"


class StereoCameraTarget:
    """Context manager: tracking_target = stereo_camera with a deadline, restored on exit."""

    def __init__(self, minutes=10.0, config_dir=None, timeout_s=15.0, log=print):
        dirs = [config_dir] if config_dir else config_dirs()
        if not dirs:
            raise TargetError(f"no SR device under {STORE}")
        if len(dirs) > 1:
            log(f"several SR devices; using {dirs[0]} (pass config_dir to choose)")
        self.dir = dirs[0]
        self.ini = os.path.join(self.dir, "sr_config.ini")
        self.minutes = minutes
        self.timeout_s = timeout_s
        self.log = log
        self._saved = None

    def _write(self, text):
        tmp = self.ini + ".weave_qa.tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, self.ini)

    def _wait_for(self, target, since):
        deadline = time.time() + self.timeout_s
        while time.time() < deadline:
            st = read_status(self.dir)
            if st and st.get("target") == target and float(st.get("since", 0)) >= since - 1.0:
                return st
            time.sleep(0.1)
        st = read_status(self.dir)
        raise TargetError(f"the tracker did not report target={target} within {self.timeout_s:.0f} s "
                          f"(status: {st})")

    def __enter__(self):
        if read_status(self.dir) is None:
            raise TargetError(
                f"no {os.path.join(self.dir, 'tracking_target.status')}: SR Eye Tracker is not following "
                "tracking_target. It needs LeiaSR with LeiaInc/LeiaSR#948 and the system environment "
                "variable LEIASR_LIVE_TRACKING_TARGET=1, then one tracker restart.")
        try:
            with open(self.ini, encoding="utf-8") as f:
                self._saved = f.read()
        except FileNotFoundError:
            self._saved = ""
        until = int(time.time() + self.minutes * 60)
        start = time.time()
        self._write(_set_service_keys(self._saved, {"tracking_target": "stereo_camera",
                                                    "tracking_target_until": str(until)}))
        st = self._wait_for("stereo_camera", start)
        self.log(f"tracker follows the stereo camera ({st.get('algorithm')}) until "
                 f"{time.strftime('%H:%M:%S', time.localtime(until))}")
        return self

    def __exit__(self, *exc):
        start = time.time()
        self._write(self._saved if self._saved is not None else _set_service_keys("", {"tracking_target": "face"}))
        try:
            self._wait_for("face", start)
            self.log("tracker follows faces again")
        except TargetError as e:
            self.log(f"WARNING: {e}")
        return False
