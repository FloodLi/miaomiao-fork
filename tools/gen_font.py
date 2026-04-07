#!/usr/bin/env python3
"""Generate bitmap font data for EPD display.
Uses GNU Unifont (.hex) for pixel-perfect 16x16 Chinese glyphs,
and Consolas TrueType for 8x16 ASCII glyphs.
Outputs main/font_data.h.
"""
import sys, os, gzip, io

UNIFONT_URL = "https://unifoundry.com/pub/unifont/unifont-16.0.02/font-builds/unifont-16.0.02.hex.gz"

def get_gb2312_level1():
    """Generate all 3755 GB2312 Level-1 Chinese characters."""
    chars = []
    for row in range(0xB0, 0xD8):
        col_end = 0xFA if row == 0xD7 else 0xFF
        for col in range(0xA1, col_end):
            try:
                ch = bytes([row, col]).decode('gb2312')
                chars.append(ch)
            except (UnicodeDecodeError, ValueError):
                pass
    return ''.join(chars)

EXTRA_CHARS = "浏览器页态址钟盘轮播墨升级"

def get_gb2312_level2():
    """Generate all GB2312 Level-2 Chinese characters (rows D8-F7)."""
    chars = []
    for row in range(0xD8, 0xF8):
        for col in range(0xA1, 0xFF):
            try:
                ch = bytes([row, col]).decode('gb2312')
                chars.append(ch)
            except (UnicodeDecodeError, ValueError):
                pass
    return ''.join(chars)

CHINESE_CHARS = get_gb2312_level1() + get_gb2312_level2() + EXTRA_CHARS


def download_unifont(cache_path):
    """Download GNU Unifont .hex.gz and cache locally."""
    import urllib.request
    print(f"Downloading GNU Unifont from {UNIFONT_URL} ...")
    try:
        urllib.request.urlretrieve(UNIFONT_URL, cache_path)
        print(f"Saved to {cache_path}")
        return True
    except Exception as e:
        print(f"Download failed: {e}")
        return False


def load_unifont_hex(hex_gz_path, needed_cps):
    """Parse unifont .hex.gz, return dict { codepoint: [32 bytes] } for 16-wide glyphs."""
    needed = set(needed_cps)
    glyphs = {}
    opener = gzip.open if hex_gz_path.endswith('.gz') else open
    with opener(hex_gz_path, 'rt', encoding='ascii') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp_str, hex_data = line.split(':', 1)
            cp = int(cp_str, 16)
            if cp not in needed:
                continue
            raw = bytes.fromhex(hex_data)
            if len(raw) == 32:
                glyphs[cp] = list(raw)
            elif len(raw) == 16:
                wide = []
                for b in raw:
                    wide.append(b)
                    wide.append(0)
                glyphs[cp] = wide
            if len(glyphs) == len(needed):
                break
    return glyphs


def ensure_pillow():
    try:
        from PIL import Image, ImageDraw, ImageFont
        return True
    except ImportError:
        print("Pillow not found, installing...")
        import subprocess
        subprocess.check_call([sys.executable, "-m", "pip", "install", "Pillow", "-q"])
        return True


def find_font(candidates):
    from PIL import ImageFont
    for fp in candidates:
        if os.path.exists(fp):
            try:
                ImageFont.truetype(fp, 14)
                return fp
            except Exception:
                continue
    return None


def render_8x16(font, ch):
    from PIL import Image, ImageDraw
    img = Image.new('L', (8, 16), 0)
    draw = ImageDraw.Draw(img)
    bb = draw.textbbox((0, 0), ch, font=font)
    x = (8 - (bb[2] - bb[0])) // 2 - bb[0]
    y = (16 - (bb[3] - bb[1])) // 2 - bb[1]
    draw.text((x, y), ch, font=font, fill=255)
    data = []
    for row in range(16):
        b = 0
        for col in range(8):
            if img.getpixel((col, row)) > 100:
                b |= 0x80 >> col
        data.append(b)
    return data


def render_16x16_ttf(font, ch):
    """Fallback: render Chinese char via TrueType (if Unifont glyph unavailable)."""
    from PIL import Image, ImageDraw
    img = Image.new('L', (16, 16), 0)
    draw = ImageDraw.Draw(img)
    bb = draw.textbbox((0, 0), ch, font=font)
    x = (16 - (bb[2] - bb[0])) // 2 - bb[0]
    y = (16 - (bb[3] - bb[1])) // 2 - bb[1]
    draw.text((x, y), ch, font=font, fill=255)
    data = []
    for row in range(16):
        hi = lo = 0
        for col in range(8):
            if img.getpixel((col, row)) > 80:
                hi |= 0x80 >> col
        for col in range(8, 16):
            if img.getpixel((col, row)) > 80:
                lo |= 0x80 >> (col - 8)
        data.append(hi)
        data.append(lo)
    return data


def main():
    if not ensure_pillow():
        sys.exit(1)
    from PIL import ImageFont

    tools_dir = os.path.dirname(os.path.abspath(__file__))

    # --- ASCII via TrueType ---
    mono_path = find_font([
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
    ])
    if not mono_path:
        print("WARNING: No mono font, using default")
    else:
        print(f"Mono font: {mono_path}")

    mono_font = ImageFont.truetype(mono_path, 13) if mono_path else ImageFont.load_default()

    ascii_data = []
    for code in range(32, 127):
        ascii_data.append((code, chr(code), render_8x16(mono_font, chr(code))))

    # --- Chinese via GNU Unifont (pixel-perfect 16x16 bitmaps) ---
    unique = list(dict.fromkeys(CHINESE_CHARS))
    needed_cps = [ord(ch) for ch in unique]

    hex_gz = os.path.join(tools_dir, "unifont.hex.gz")
    unifont_glyphs = {}

    if not os.path.exists(hex_gz):
        download_unifont(hex_gz)

    if os.path.exists(hex_gz):
        print(f"Loading Unifont from {hex_gz} ...")
        unifont_glyphs = load_unifont_hex(hex_gz, needed_cps)
        print(f"  Found {len(unifont_glyphs)}/{len(needed_cps)} glyphs in Unifont")

    # Fallback to TrueType for any missing glyphs
    zh_font = None
    missing = [ch for ch in unique if ord(ch) not in unifont_glyphs]
    if missing:
        zh_path = find_font([
            "C:/Windows/Fonts/simhei.ttf",
            "C:/Windows/Fonts/simsun.ttc",
            "C:/Windows/Fonts/msyh.ttc",
        ])
        if zh_path:
            zh_font = ImageFont.truetype(zh_path, 14)
            print(f"  TTF fallback for {len(missing)} chars: {zh_path}")
        else:
            print(f"  WARNING: {len(missing)} chars have no glyph source!")

    zh_data = []
    for ch in unique:
        cp = ord(ch)
        if cp in unifont_glyphs:
            zh_data.append((cp, ch, unifont_glyphs[cp]))
        elif zh_font:
            zh_data.append((cp, ch, render_16x16_ttf(zh_font, ch)))
        else:
            zh_data.append((cp, ch, [0] * 32))

    # --- Output C header ---
    out = os.path.join(tools_dir, "..", "main", "font_data.h")
    with open(out, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated bitmap font data. Do not edit. */\n")
        f.write(f"/* ASCII: {len(ascii_data)} glyphs (8x16)  Chinese: {len(zh_data)} glyphs (16x16) */\n")
        f.write(f"/* Chinese source: GNU Unifont (pixel-perfect bitmap) */\n\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")

        f.write(f"static const uint8_t font_ascii[{len(ascii_data)}][16] = {{\n")
        for code, ch, bmp in ascii_data:
            h = ", ".join(f"0x{b:02X}" for b in bmp)
            safe = ch if ch not in ("\\", "'", '"') else f"0x{code:02X}"
            f.write(f"    {{ {h} }}, /* '{safe}' */\n")
        f.write("};\n\n")

        f.write("typedef struct { uint32_t cp; uint8_t bmp[32]; } zh_glyph_t;\n\n")
        f.write(f"static const zh_glyph_t font_zh[{len(zh_data)}] = {{\n")
        for cp, ch, bmp in zh_data:
            h = ", ".join(f"0x{b:02X}" for b in bmp)
            f.write(f"    {{ 0x{cp:04X}, {{ {h} }} }}, /* {ch} */\n")
        f.write("};\n\n")
        f.write(f"#define FONT_ZH_COUNT {len(zh_data)}\n")

    print(f"\nGenerated {out}")
    print(f"  ASCII:   {len(ascii_data)} chars (TrueType)")
    print(f"  Chinese: {len(zh_data)} chars (Unifont: {len(unifont_glyphs)}, TTF fallback: {len(zh_data) - len(unifont_glyphs)})")


if __name__ == "__main__":
    main()
