# Weave QA

Checks that the 3D weave is right, as a viewer's two eyes would see it, with no person in
front of the panel. A stereo camera (Acer SpatialLabs Eyes) stands in for the viewer, Leia
SR's eye tracker is pointed at the camera's lenses instead of a face, and the panel shows
test patterns through DisplayXR. Each lens should see only its own view. The tool measures
how much of the other view leaks in (crosstalk), per lens and per colour, and whether the
eyes are swapped.

```
python tools/weave_qa/weave_qa.py --ffmpeg C:\path\to\ffmpeg.exe
python tools/weave_qa/weave_qa.py --positioner grbl:COM5 --grbl-axes x=X --home \
                                  --positions "x=-150;x=0;x=150"
```

Exit code 0 = pass, 1 = fail (crosstalk above `--max-crosstalk`, 15 % by default, eyes
swapped, or no separation), 2 = could not run. Everything is written under
`%TEMP%\weave_qa\<time>\`: the patterns, the camera frames per position, a `report.json`
per position and an overall `report.json`.

## What a run does

1. `pattern.py` makes two side-by-side images, A and B. Each eye has ArUco markers in the
   corners (so the camera can find the screen) and four rows: lit for the left eye, black,
   lit for the right eye, lit for both (the reference). A swaps which rows are left and
   right in B.
2. `leiasr_target.py` sets `[Service] tracking_target = stereo_camera` with a
   `tracking_target_until` deadline in the device's `config\sr_config.ini`, then waits
   until `config\tracking_target.status` says the tracker has switched. At the end, even
   after an error, it puts the file back as it was. If this process dies, the tracker goes
   back to faces by itself at the deadline.
3. `cube_handle_d3d11_win` shows A and B in turn, full screen, one half per view
   (`DISPLAYXR_WEAVE_PATTERN`, `test_apps/common/weave_pattern_test.h`).
4. For each camera position, `positioner.py` moves the camera, then the run waits for the
   tracker and weave to settle, captures frames (`capture.py`) and analyses them
   (`analyse.py`). The analysis calibrates the camera from each frame's own reference row,
   so the camera's exposure and gamma do not matter.

## What the machine needs

- **Leia SR with live tracking-target switching**: LeiaInc/LeiaSR#948, and the system
  environment variable `LEIASR_LIVE_TRACKING_TARGET=1`, set once, then one restart of the
  SR Eye Tracker. Without it the tool stops with that message and changes nothing. Setting
  it up is the only step that needs admin rights; the runs themselves need none.
- **The stereo camera**, switched on and connected for data (DirectShow name
  `SpatialLabs Eyes`; `--camera` for another), facing the panel at about viewing distance.
- **ffmpeg** (`--ffmpeg`, `WEAVE_QA_FFMPEG` or PATH), Python 3 with `opencv-python` (4.7 or
  newer, for `cv2.aruco`) and `numpy`, and `pyserial` for a GRBL positioner.
- **The test app**: `scripts\build_windows.bat test-apps`. `--runtime-json` runs it against
  a dev runtime instead of the installed one.

## Moving the camera

Positions are in millimetres in the display's frame: x to the right, y up, z out of the
screen, with the origin at the screen centre. `positioner.py` is the Python API:
`move_to(x=, y=, z=)`, `home()`, `close()`.

| `--positioner` | moves the camera with |
|---|---|
| `none` | nothing: one position, wherever the camera is |
| `manual` | you: it prints where to put the camera and waits for Enter |
| `sim` | nothing: it logs the moves (dry runs) |
| `grbl:COM5` | a GRBL 1.1 controller over USB serial (an Arduino with a CNC shield drives most stepper rails and XY gantries). `--grbl-axes x=X,z=Y` maps display axes to machine axes; `--grbl-origin` gives the display position of machine zero; `--home` runs `$H` first |

A robot arm is one more `Positioner` subclass with the same three methods.

## Tests

- `python3 scripts/tests/test_weave_qa.py`: the ini edit, the switch against a fake
  tracker, and GRBL G-code against a fake serial port.
- `python tools/weave_qa/analyse.py --selftest [--levels dim|normal]`: a fake camera with
  known leaks (none, 10 %, 2D, swapped) must be read back correctly.
