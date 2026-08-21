#!/usr/bin/env python3
"""Regenerate lv_font_cn_16.c adding missing glyphs.

Extracts the --symbols list recorded in the generated file's header, ensures a
space glyph (U+0020) is present, appends any missing characters (e.g. 贵), and
re-runs lv_font_conv.
"""
import re
import sys
import subprocess
import shutil
import os

FONT_FILE = "main/lv_font_cn_16.c"
FONT_NAME = "lv_font_cn_16"

# 追加到字库的缺失字符 (屏幕显示缺字时加到这里)
MISSING_CHARS = "贵"

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

    # 确保空格 glyph 存在 (否则空格渲染成豆腐块)
    if chr(0x20) not in symbols:
        symbols = " " + symbols

    # 追加缺失字符 (去重)
    added = []
    for ch in MISSING_CHARS:
        if ch not in symbols:
            symbols += ch
            added.append(ch)
    print(f"Added missing chars: {added if added else '(none — all present)'}")

    if "--dry-run" in sys.argv:
        print("Dry-run: not regenerating.")
        return

    # lv_font_conv derives the font symbol + include-guard name from the output
    # file's basename.  It must therefore be written to a temp file whose
    # basename is EXACTLY "lv_font_cn_16.c" (a ".new" suffix would produce a
    # broken name like "LV_FONT_CN_16.C").
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
        "--symbols", symbols,
        "--lv-include", "lvgl.h",
        "--format", "lvgl",
        "-o", out_tmp,
    ]
    print("Running lv_font_conv ...")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print("lv_font_conv failed")
        sys.exit(r.returncode)

    # Verify the new file contains the space glyph (U+0020) comment.
    with open(out_tmp, "r", encoding="utf-8") as f:
        newtxt = f.read()
    if 'U+0020' not in newtxt:
        print("ERROR: regenerated file does not contain U+0020 space glyph")
        os.remove(out_tmp)
        sys.exit(1)
    # Verify each newly-added char actually got a glyph (U+8D35 for 贵 etc.).
    for ch in added:
        if f'U+{ord(ch):04X}' not in newtxt:
            print(f"ERROR: regenerated file missing glyph for {ch!r} (U+{ord(ch):04X})")
            os.remove(out_tmp)
            sys.exit(1)
    if re.search(r'lv_font_cn_16\.c\s*=', newtxt) or 'LV_FONT_CN_16.C' in newtxt:
        print("ERROR: regenerated file has broken font symbol/guard name (bad output basename)")
        os.remove(out_tmp)
        sys.exit(1)

    # Backup and replace.
    shutil.copyfile(FONT_FILE, FONT_FILE + ".bak")
    shutil.move(out_tmp, FONT_FILE)
    print("OK: font regenerated with missing glyphs (backup at .bak)")

if __name__ == "__main__":
    main()
