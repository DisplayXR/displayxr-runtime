#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: BSL-1.0
"""Assert every native library in an Android APK / AAR is 16 KB page-aligned.

Android 15+ devices may run a 16 KB page-size kernel. On those, the dynamic
linker refuses any .so whose PT_LOAD segments are aligned below the page size
(the classic 4 KB 0x1000), so a 4 KB-aligned library simply fails to dlopen --
for the Vulkan queue-lock layer that means the layer silently never loads
(DisplayXR/displayxr-unity#418). The build links with
-Wl,-z,max-page-size=16384 (CompilerFlags.cmake); this is the from-the-artifact
proof that it reached every library we ship.

Also checks the APK's zip alignment of each *stored* (uncompressed) .so: a
library loaded straight from the APK (extractNativeLibs=false) must start on a
16 KB file offset as well.

Self-contained ELF64 / zip parsing -- no NDK needed, so it runs on any runner.

Usage:
  check_android_page_align.py [--min-align 16384] [--warn-only GLOB ...] FILE...
    FILE  .apk, .aar, .zip or a bare .so
    --warn-only GLOB  basename glob of libraries we do not build (third-party
                      payloads); reported, but they do not fail the check.
Exit 0 = all enforced libraries pass.
"""

import argparse
import fnmatch
import struct
import sys
import zipfile

PT_LOAD = 1


def load_aligns(data):
    """Return the p_align of every PT_LOAD segment of an ELF image."""
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    ei_class = data[4]
    endian = "<" if data[5] == 1 else ">"
    if ei_class == 2:  # ELF64
        e_phoff, = struct.unpack_from(endian + "Q", data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x36)
        out = []
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, = struct.unpack_from(endian + "I", data, off)
            if p_type == PT_LOAD:
                p_align, = struct.unpack_from(endian + "Q", data, off + 0x30)
                out.append(p_align)
        return out
    if ei_class == 1:  # ELF32
        e_phoff, = struct.unpack_from(endian + "I", data, 0x1C)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x2A)
        out = []
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, = struct.unpack_from(endian + "I", data, off)
            if p_type == PT_LOAD:
                p_align, = struct.unpack_from(endian + "I", data, off + 0x1C)
                out.append(p_align)
        return out
    raise ValueError("unknown ELF class %d" % ei_class)


def data_offset(zf, info):
    """File offset of a zip member's data (past its local header)."""
    zf.fp.seek(info.header_offset)
    hdr = zf.fp.read(30)
    name_len, extra_len = struct.unpack_from("<HH", hdr, 26)
    return info.header_offset + 30 + name_len + extra_len


def iter_libs(path):
    """Yield (display_name, bytes, stored_offset_or_None)."""
    if path.endswith(".so"):
        with open(path, "rb") as f:
            yield path, f.read(), None
        return
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            if not info.filename.endswith(".so"):
                continue
            stored = None
            if info.compress_type == zipfile.ZIP_STORED and path.endswith(".apk"):
                stored = data_offset(zf, info)
            yield "%s!%s" % (path, info.filename), zf.read(info), stored


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--min-align", type=int, default=16384)
    ap.add_argument("--warn-only", action="append", default=[], metavar="GLOB")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    failed = 0
    seen = 0
    for path in args.files:
        for name, data, stored in iter_libs(path):
            seen += 1
            base = name.rsplit("/", 1)[-1]
            warn_only = any(fnmatch.fnmatch(base, g) for g in args.warn_only)
            try:
                aligns = load_aligns(data)
            except ValueError as e:
                print("::error::%s: %s" % (name, e))
                failed += 1
                continue
            low = min(aligns) if aligns else 0
            problems = []
            if low < args.min_align:
                problems.append("PT_LOAD align 0x%x < 0x%x" % (low, args.min_align))
            if stored is not None and stored % args.min_align:
                problems.append("stored in the APK at offset 0x%x, not %d-aligned (zipalign -P 16)"
                                % (stored, args.min_align // 1024))
            if not problems:
                print("  ok: %s (PT_LOAD align 0x%x)" % (name, low))
            elif warn_only:
                print("::warning::%s: %s -- third-party payload, not built here; "
                      "will not load on a 16 KB-page device" % (name, "; ".join(problems)))
            else:
                print("::error::%s: %s -- will not load on a 16 KB-page "
                      "Android 15+ device (DisplayXR/displayxr-unity#418)" % (name, "; ".join(problems)))
                failed += 1
    if seen == 0:
        print("::error::no .so found in %s" % " ".join(args.files))
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
