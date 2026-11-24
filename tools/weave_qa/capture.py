# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Grab frames from the stereo camera (DirectShow, through ffmpeg) as cam_NNNN.png.

The Acer SpatialLabs Eyes gives 3840x2160 side by side (left lens = left half) as MJPEG.
Its exposure cannot be locked, which is why the analysis calibrates every frame from the
pattern itself. ffmpeg comes from --ffmpeg, WEAVE_QA_FFMPEG or PATH.
"""
import glob
import os
import shutil
import subprocess


def find_ffmpeg(explicit=None):
    for candidate in (explicit, os.environ.get("WEAVE_QA_FFMPEG"), shutil.which("ffmpeg")):
        if candidate and os.path.isfile(candidate):
            return candidate
    raise FileNotFoundError("ffmpeg not found: pass --ffmpeg or set WEAVE_QA_FFMPEG")


def capture(out_dir, seconds, camera="SpatialLabs Eyes", fps=4, ffmpeg=None, size="3840x2160"):
    """Writes cam_0001.png ... into out_dir (old ones removed); returns how many."""
    os.makedirs(out_dir, exist_ok=True)
    for f in glob.glob(os.path.join(out_dir, "cam_*.png")):
        os.remove(f)
    cmd = [find_ffmpeg(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-f", "dshow", "-rtbufsize", "512M",
           "-video_size", size, "-framerate", "30", "-i", f"video={camera}",
           "-t", str(seconds), "-vf", f"fps={fps}", os.path.join(out_dir, "cam_%04d.png")]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 60)
    frames = len(glob.glob(os.path.join(out_dir, "cam_*.png")))
    if r.returncode != 0 or frames == 0:
        raise RuntimeError(f"camera capture failed ({camera!r}): {r.stderr.strip()[-400:]}")
    return frames
