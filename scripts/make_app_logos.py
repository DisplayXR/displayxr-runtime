#!/usr/bin/env python3
"""
Generate DisplayXR workspace logos + manifest from a captured atlas PNG.

Turns a projection-only atlas capture (the per-tile multi-view image the app
rendered) into the two workspace launcher logos and a schema-v1
`displayxr.json` manifest:

  - 2D logo   (icon.png / <name>.png)      512x512    -- the centre-left tile
  - 3D logo   (icon_sbs.png / <name>_sbs.png) 1024x512 -- centre two tiles, sbs-lr
  - manifest  (<stem>.displayxr.json)                 -- references the logos

This is the image+manifest half of the /make-app-logos skill; the skill drives
the capture (launch app -> wait FOCUSED -> file-trigger projection capture ->
%TEMP%\\displayxr_atlas.projection.png) and then calls this script.

Output is validated by scripts/check_displayxr_app.py (INV-9.x).

Modes:
  sidecar    (default)        manifest has NO exe_path; icons named icon.png /
                              icon_sbs.png; lives next to the exe.
  registered (--exe-path P)   manifest carries exe_path (forward slashes);
                              app-specific icon names <name>.png / <name>_sbs.png
                              because the apps\\ drop-in dir is shared.

Requires Pillow. Python 3.6+.

Exit codes: 0 ok | 1 error | 2 bad invocation.
"""

import argparse
import json
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.stderr.write(
        "ERROR: Pillow is required (pip install Pillow).\n")
    sys.exit(1)

ICON_2D = (512, 512)
ICON_3D = (1024, 512)
TILE_PX = 512  # each view in the 3D logo

# Geometry tokens the runtime appends, newest first:
#   <stem>-<N>_atlas_<viewCount>_<cols>x<rows>.png   (XR_EXT_atlas_capture v2)
#   <stem>-<N>_<cols>x<rows>.png                     (legacy readback)
#   displayxr_atlas.projection.png                   (file-trigger; no tokens)
_GRID_RE = re.compile(r"_(?:atlas_\d+_)?(\d+)x(\d+)\.png$", re.IGNORECASE)


def infer_grid(atlas_path, cols, rows):
    """Resolve (cols, rows): explicit args win, else parse the filename, else 2x1."""
    if cols and rows:
        return cols, rows
    m = _GRID_RE.search(atlas_path.name)
    if m:
        return int(m.group(1)), int(m.group(2))
    return 2, 1


def square_crop(img, dx, dy):
    """Centre-crop `img` to a square, shifted by (dx, dy) px, clamped in-bounds."""
    w, h = img.size
    side = min(w, h)
    cx, cy = w // 2 + dx, h // 2 + dy
    left = max(0, min(cx - side // 2, w - side))
    top = max(0, min(cy - side // 2, h - side))
    return img.crop((left, top, left + side, top + side))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate DisplayXR logos + manifest from an atlas capture.")
    ap.add_argument("--atlas", required=True, type=Path,
                    help="Captured projection atlas PNG.")
    ap.add_argument("--out-dir", required=True, type=Path,
                    help="Directory to write the manifest + icons into.")
    ap.add_argument("--name", required=True,
                    help="Display name (1-64 chars).")
    ap.add_argument("--type", default="3d", choices=("2d", "3d"),
                    help="Manifest app type (default 3d).")
    ap.add_argument("--category", default="app",
                    help="Manifest category tag (default app).")
    ap.add_argument("--description", default="",
                    help="One-line tooltip (max 256 chars).")
    ap.add_argument("--display-mode", default="auto",
                    help="Preferred launch display mode (default auto).")
    ap.add_argument("--manifest-name", default=None,
                    help="Manifest filename stem -> <stem>.displayxr.json "
                         "(default: derived from --name).")
    ap.add_argument("--exe-path", default=None,
                    help="Set => REGISTERED mode: write exe_path + app-specific "
                         "icon names. Absent => sidecar mode.")
    ap.add_argument("--cols", type=int, default=0, help="Tile columns (else inferred).")
    ap.add_argument("--rows", type=int, default=0, help="Tile rows (else inferred).")
    ap.add_argument("--crop-nudge", default="0,0",
                    help="Shift the square crop centre, 'DX,DY' px (default 0,0).")
    args = ap.parse_args(argv)

    if not (1 <= len(args.name) <= 64):
        sys.stderr.write("ERROR: --name must be 1-64 characters.\n")
        return 2
    if len(args.description) > 256:
        sys.stderr.write("ERROR: --description must be <= 256 characters.\n")
        return 2
    if not args.atlas.is_file():
        sys.stderr.write("ERROR: atlas not found: {}\n".format(args.atlas))
        return 1
    try:
        dx, dy = (int(v) for v in args.crop_nudge.split(","))
    except ValueError:
        sys.stderr.write("ERROR: --crop-nudge must be 'DX,DY' integers.\n")
        return 2

    # 1. Load + force opaque. Alpha-blended renderers leave the atlas alpha << 1;
    #    the display ignores alpha but RGBA viewers/compose paths would dim the
    #    logo, so flatten to fully opaque regardless of the app's blend.
    atlas = Image.open(args.atlas).convert("RGBA")
    atlas.putalpha(255)
    aw, ah = atlas.size

    # 2. Resolve the tile grid.
    cols, rows = infer_grid(args.atlas, args.cols, args.rows)
    if cols < 1 or rows < 1:
        sys.stderr.write("ERROR: invalid tile grid {}x{}.\n".format(cols, rows))
        return 1
    if cols <= 1 and rows <= 1:
        sys.stderr.write(
            "WARN: atlas resolved to 1x1 (mono) -- the app likely wasn't in a 3D "
            "mode when captured; the 3D logo will duplicate one view.\n")

    # 3. Centre two tiles on the top row.
    tile_w, tile_h = aw // cols, ah // rows
    col_l = max(0, cols // 2 - 1)
    col_r = min(cols - 1, cols // 2)

    def tile(col):
        box = (col * tile_w, 0, col * tile_w + tile_w, tile_h)
        return atlas.crop(box).convert("RGB")

    left = square_crop(tile(col_l), dx, dy).resize(ICON_2D, Image.LANCZOS)
    right = square_crop(tile(col_r), dx, dy).resize((TILE_PX, TILE_PX), Image.LANCZOS)

    # 4. Compose the logos.
    icon_2d = left
    icon_3d = Image.new("RGB", ICON_3D)
    icon_3d.paste(left.resize((TILE_PX, TILE_PX), Image.LANCZOS), (0, 0))  # sbs-lr
    icon_3d.paste(right, (TILE_PX, 0))

    # 5. Names + mode.
    registered = bool(args.exe_path)
    stem = args.manifest_name or re.sub(r"[^A-Za-z0-9._-]+", "_", args.name).strip("_")
    if registered:
        icon_2d_name = "{}.png".format(stem)
        icon_3d_name = "{}_sbs.png".format(stem)
    else:
        icon_2d_name = "icon.png"
        icon_3d_name = "icon_sbs.png"

    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    icon_2d.save(out / icon_2d_name)
    icon_3d.save(out / icon_3d_name)

    # 6. Manifest -- real serializer, UTF-8, forward-slash exe_path.
    manifest = {
        "schema_version": 1,
        "name": args.name,
        "type": args.type,
        "category": args.category,
        "display_mode": args.display_mode,
        "description": args.description,
        "icon": icon_2d_name,
        "icon_3d": icon_3d_name,
        "icon_3d_layout": "sbs-lr",
    }
    if registered:
        manifest["exe_path"] = str(args.exe_path).replace("\\", "/")
    manifest_path = out / "{}.displayxr.json".format(stem)
    with manifest_path.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print("make_app_logos: wrote ({} mode, grid {}x{}, tiles {},{})".format(
        "registered" if registered else "sidecar", cols, rows, col_l, col_r))
    print("  {}".format(out / icon_2d_name))
    print("  {}".format(out / icon_3d_name))
    print("  {}".format(manifest_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
