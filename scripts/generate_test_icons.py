#!/usr/bin/env python3
"""Generate placeholder test icons for cube_handle apps (Phase 7.5)."""

import os
from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

APPS = [
    ("cube_handle_d3d11_win", "D", (40, 100, 200)),   # blue
    ("cube_handle_d3d12_win", "12", (40, 160, 80)),    # green
    ("cube_handle_gl_win",    "G",  (220, 140, 30)),   # orange
    ("cube_handle_vk_win",    "V",  (200, 40, 40)),    # red
]

SIZE = 512
SBS_W, SBS_H = 1024, 512
DISPARITY = 12  # pixels of horizontal shift per eye


def find_font(size):
    """Try to load a bold system font; fall back to default."""
    for name in ["arialbd.ttf", "arial.ttf", "calibrib.ttf", "segoeui.ttf"]:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def draw_icon(img, text, bg_color, offset_x=0):
    """Draw a colored background with centered white text."""
    draw = ImageDraw.Draw(img)
    w, h = img.size
    draw.rectangle([0, 0, w, h], fill=bg_color)

    font = find_font(int(h * 0.55))
    bbox = draw.textbbox((0, 0), text, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    tx = (w - tw) / 2 + offset_x
    ty = (h - th) / 2 - bbox[1]
    draw.text((tx, ty), text, fill=(255, 255, 255), font=font)


def main():
    for app_dir, letter, color in APPS:
        out_dir = os.path.join(REPO, "test_apps", app_dir, "displayxr")
        os.makedirs(out_dir, exist_ok=True)

        # 2D icon
        img = Image.new("RGBA", (SIZE, SIZE))
        draw_icon(img, letter, color)
        icon_path = os.path.join(out_dir, "icon.png")
        img.save(icon_path)
        print(f"  {icon_path}")

    # SBS 3D icon for D3D11 app — left half shifted left, right half shifted right
    app_dir = "cube_handle_d3d11_win"
    out_dir = os.path.join(REPO, "test_apps", app_dir, "displayxr")
    sbs = Image.new("RGBA", (SBS_W, SBS_H))

    # Left eye (left half of SBS image)
    left = Image.new("RGBA", (SBS_W // 2, SBS_H))
    draw_icon(left, "D", APPS[0][2], offset_x=-DISPARITY)
    sbs.paste(left, (0, 0))

    # Right eye (right half of SBS image)
    right = Image.new("RGBA", (SBS_W // 2, SBS_H))
    draw_icon(right, "D", APPS[0][2], offset_x=DISPARITY)
    sbs.paste(right, (SBS_W // 2, 0))

    sbs_path = os.path.join(out_dir, "icon_sbs.png")
    sbs.save(sbs_path)
    print(f"  {sbs_path} (SBS 3D)")

    print("Done.")


if __name__ == "__main__":
    main()
