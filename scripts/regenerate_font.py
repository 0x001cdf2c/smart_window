#!/usr/bin/env python3
"""Regenerate lv_font_cn_16.c adding the missing U+0020 space glyph.

The font was originally generated without a space glyph (cmap starts at U+0021),
which makes every space in UI strings render as a tofu box [].  This script
extracts the exact --symbols list recorded in the generated file's header,
prepends a space, and re-runs lv_font_conv.
"""
import re
import sys
import subprocess
import shutil
import os

FONT_FILE = "main/lv_font_cn_16.c"
FONT_NAME = "lv_font_cn_16"

def main():
    with open(FONT_FILE, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Header line 4 contains: * Opts: ... --symbols <SYMBOLS> --lv-include ...
    header = lines[3]
    m = re.search(r"--symbols (.*?) --lv-include", header)
    if not m:
        print("ERROR: could not find --symbols in header")
        sys.exit(1)

    symbols = m.group(1)
    print(f"Extracted symbols length: {len(symbols)} chars")
    print(f"First 60 chars: {symbols[:60]!r}")
    print(f"Contains space already: {chr(0x20) in symbols}")

    # Prepend a real U+0020 space.
    new_symbols = " " + symbols

    if "--dry-run" in sys.argv:
        print("Dry-run: not regenerating.")
        return

    # lv_font_conv derives the font symbol + include-guard name from the output
    # file's basename.  It must therefore be written to a temp file whose
    # basename is EXACTLY "lv_font_cn_16.c" (a ".new" suffix would produce a
    # broken name like "lv_font_cn_16.c" → guard "LV_FONT_CN_16.C").
    tmp_dir = "build/font_tmp"
    os.makedirs(tmp_dir, exist_ok=True)
    out_tmp = os.path.join(tmp_dir, "lv_font_cn_16.c")

    # Call lv_font_conv.js directly via node to avoid npx.cmd path-with-space issues.
    lv_conv = "C:/Users/28145/AppData/Roaming/npm/node_modules/lv_font_conv/lv_font_conv.js"
    cmd = [
        "node", lv_conv,
        "--no-compress", "--no-prefilter",
        "--bpp", "4", "--size", "16",
        "--font", "C:/Windows/Fonts/simhei.ttf",
        "--symbols", new_symbols,
        "--lv-include", "lvgl.h",
        "--format", "lvgl",
        "-o", out_tmp,
    ]
    print("Running lv_font_conv ...")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print("lv_font_conv failed")
        sys.exit(r.returncode)

    # Verify the new file contains a space glyph (U+0020) comment.
    with open(out_tmp, "r", encoding="utf-8") as f:
        newtxt = f.read()
    if 'U+0020' not in newtxt:
        print("ERROR: regenerated file does not contain U+0020 space glyph")
        os.remove(out_tmp)
        sys.exit(1)
    if re.search(r'lv_font_cn_16\.c\s*=', newtxt) or 'LV_FONT_CN_16.C' in newtxt:
        print("ERROR: regenerated file has broken font symbol/guard name (bad output basename)")
        os.remove(out_tmp)
        sys.exit(1)

    # Backup and replace.
    shutil.copyfile(FONT_FILE, FONT_FILE + ".bak")
    shutil.move(out_tmp, FONT_FILE)
    print("OK: font regenerated with space glyph (backup at .bak)")

if __name__ == "__main__":
    main()
