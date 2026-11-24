#!/usr/bin/env python3
"""Generate LVGL v8 (8.4.x) compatible Korean bitmap fonts.

The thermostat UI uses Malgun Gothic (맑은 고딕) rendered with Pillow.  Every
glyph is stored as an uncompressed 4 bpp LVGL ``lv_font_fmt_txt`` bitmap, which
is exactly the format produced by ``lv_font_conv --no-compress --bpp 4``.

Outputs (committed to the repository):
    src/ui/waveshare43b/kr_font_20.c   -> LV_FONT_DECLARE(kr_font_20)
    src/ui/waveshare43b/kr_font_28.c   -> LV_FONT_DECLARE(kr_font_28)
    src/ui/waveshare43b/kr_font_48.c   -> LV_FONT_DECLARE(kr_font_48)

Pillow is not a project dependency.  Run the script through uv:

    uv run --with pillow python tools/gen_kr_font.py

Only the Python standard library is used outside of the optional Pillow import.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_DIR = REPO_ROOT / "src" / "ui" / "waveshare43b"

# Every string the thermostat UI and the 03_ui demo can display.  The union of
# their characters (plus ASCII and symbols below) is embedded into each font.
UI_TEXT_CORPUS = [
    # thermostat_ui.cpp labels
    "현재 온도",
    "목표 온도",
    "반복 시간",
    "난방 중",
    "대기",
    "꺼짐",
    "온도",
    "반복",
    "설정",
    "반복 모드 구동 시간",
    "최저온도 보호",
    "분",
    "닫기",
    # 03_ui fake temperature-source badges
    "거실 센서 · 3분 전",
    "평균(거실, 안방) · 6분 전",
    "보드 센서 · 원격 센서 응답 없음",
    "센서 오류 · 온도 확인 불가",
    # Plan sample character set (keeps future UI strings renderable)
    "꺼짐온도반복난방중대기현재목표설정최저보호거실안방보드원격센서"
    "평균최근방금분전응답없음오류주의상태시간주기저장닫기확인",
]

SYMBOLS = "℃·±°"

# Fonts tried in order when --font is not given.  Windows is the primary
# development host; the Linux/macOS candidates keep the script usable after the
# build server moves to Linux.
FONT_CANDIDATES = [
    os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "malgun.ttf"),
    os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "malgunbd.ttf"),
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
]

FONT_SIZES = {20: "kr_font_20", 28: "kr_font_28", 48: "kr_font_48"}

# LVGL lv_font_fmt_txt_glyph_dsc_t uses 4 bpp plain (uncompressed) bitmaps.
BPP = 4


def build_charset() -> list[int]:
    """Return every Unicode code point the fonts must contain, sorted."""
    codepoints = set(range(0x20, 0x7F))
    codepoints.update(ord(ch) for ch in SYMBOLS)
    for text in UI_TEXT_CORPUS:
        codepoints.update(ord(ch) for ch in text if ch != " ")
    return sorted(codepoints)


def load_pillow():
    try:
        from PIL import Image, ImageDraw, ImageFont  # noqa: PLC0415
    except ImportError:
        print(
            "Pillow가 필요합니다. 다음 명령으로 실행하세요:\n"
            "  uv run --with pillow python tools/gen_kr_font.py",
            file=sys.stderr,
        )
        return None
    return Image, ImageDraw, ImageFont


def find_font(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit).expanduser()
        return path if path.is_file() else None
    env_font = os.environ.get("THERMOSTAT_KR_FONT")
    candidates = ([env_font] if env_font else []) + FONT_CANDIDATES
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def render_glyph(image_module, draw_module, font, char: str):
    """Return (box_w, box_h, ofs_x, ofs_y, packed_bitmap, adv_w)."""
    bbox = font.getbbox(char, anchor="ls")
    x0, y0, x1, y1 = (int(v) for v in bbox)
    adv_w = int(round(font.getlength(char) * 16))

    box_w = x1 - x0
    box_h = y1 - y0
    if box_w <= 0 or box_h <= 0:
        return 0, 0, 0, 0, b"", adv_w

    image = image_module.new("L", (box_w, box_h), 0)
    draw = draw_module.Draw(image)
    draw.text((-x0, -y0), char, font=font, fill=255, anchor="ls")

    # LVGL plain 4bpp bitmaps pack two pixels per byte with no row padding:
    # the high nibble is the left pixel.  0 = transparent, 15 = opaque.
    packed = bytearray()
    pixels = image.tobytes()
    for index in range(0, len(pixels), 2):
        high = (pixels[index] * 15 + 127) // 255
        low = 0
        if index + 1 < len(pixels):
            low = (pixels[index + 1] * 15 + 127) // 255
        packed.append((high << 4) | low)

    # LVGL draws the glyph top at
    #   baseline - box_h - ofs_y  =>  bottom = baseline - ofs_y
    # and Pillow's baseline anchor reports the bottom as y1 below the baseline.
    ofs_y = -y1
    return box_w, box_h, x0, ofs_y, bytes(packed), adv_w


def c_char_comment(char: str) -> str:
    if char == " ":
        return 'space'
    if char == '"':
        return '\\"'
    return char


def format_byte_array(data: bytes, indent: str = "    ", per_line: int = 12) -> str:
    lines = []
    for index in range(0, len(data), per_line):
        chunk = data[index : index + per_line]
        lines.append(indent + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(lines)


def format_uint16_array(values: list[int], indent: str = "    ", per_line: int = 12) -> str:
    lines = []
    for index in range(0, len(values), per_line):
        chunk = values[index : index + per_line]
        lines.append(indent + ", ".join(f"0x{value:04x}" for value in chunk) + ",")
    return "\n".join(lines)


def generate_font_c(image_module, draw_module, font_class, font_path: Path,
                    pixel_size: int, symbol_name: str, codepoints: list[int]) -> str:
    font = font_class.truetype(str(font_path), pixel_size)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    base_line = descent

    range_start = codepoints[0]
    max_codepoint = codepoints[-1]
    range_length = max_codepoint - range_start + 1
    if range_length > 0xFFFF:
        raise RuntimeError(
            f"문자 범위가 너무 넓습니다: 0x{range_start:x}..0x{max_codepoint:x}"
        )

    bitmap = bytearray()
    bitmap_blocks = []
    glyphs = []
    unicode_offsets = []

    for codepoint in codepoints:
        char = chr(codepoint)
        box_w, box_h, ofs_x, ofs_y, packed, adv_w = render_glyph(
            image_module, draw_module, font, char
        )
        if not (-128 <= ofs_x <= 127 and -128 <= ofs_y <= 127):
            raise RuntimeError(f"ofs 범위 초과: U+{codepoint:04X}")
        if box_w > 255 or box_h > 255:
            raise RuntimeError(f"glyph 박스 범위 초과: U+{codepoint:04X}")
        if adv_w > 0xFFF:
            raise RuntimeError(f"adv_w 범위 초과: U+{codepoint:04X}")

        glyphs.append(
            f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {adv_w}, "
            f".box_w = {box_w}, .box_h = {box_h}, "
            f".ofs_x = {ofs_x}, .ofs_y = {ofs_y}}},"
        )
        comment = f"    /* U+{codepoint:04X} \"{c_char_comment(char)}\" */"
        if packed:
            bitmap_blocks.append(comment + "\n" + format_byte_array(packed))
        else:
            bitmap_blocks.append(comment)
        bitmap.extend(packed)
        unicode_offsets.append(codepoint - range_start)

    guard = symbol_name.upper()

    return f"""\
/*
 * Korean font for the thermostat UI, generated by tools/gen_kr_font.py.
 * Do not edit manually — re-run the generator instead.
 *
 * Source font: {font_path.name}
 * Pixel size:  {pixel_size} px
 * Bpp:         {BPP}
 * Glyphs:      {len(glyphs)} (+1 reserved glyph at index 0)
 * Format:      LVGL 8.4 lv_font_fmt_txt, plain (uncompressed) 4bpp
 */

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include <lvgl.h>
#endif

#ifndef {guard}
    #define {guard} 1
#endif

#if {guard}

/*-----------------
 *    BITMAPS
 *----------------*/

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{chr(10).join(bitmap_blocks)}
}};

/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
    {{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}} /* id = 0 reserved */,
{chr(10).join(glyphs)}
}};

/*-------------
 *  CHARACTER MAPPING
 *-------------*/

/* Sparse list of code point offsets from the first character (0x{range_start:02x}). */
static const uint16_t unicode_list_0[] = {{
{format_uint16_array(unicode_offsets)}
}};

/* Collect the character maps in one place. */
static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{
        .range_start = {range_start}, .range_length = {range_length}, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = {len(unicode_offsets)},
        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }}
}};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/* Store all the custom data of the font. */
static lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {{
#else
static lv_font_fmt_txt_dsc_t font_dsc = {{
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = {BPP},
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
}};

/*-----------------
 *  PUBLIC FONT
 *----------------*/

#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t {symbol_name} = {{
#else
lv_font_t {symbol_name} = {{
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc
}};

#endif /* {guard} */
"""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate LVGL 8.4 Korean 4bpp fonts for the thermostat UI"
    )
    parser.add_argument(
        "--font",
        help="TTF/TTC 경로 (기본: Windows malgun.ttf, Linux/macOS는 Noto 계열 자동 탐색)",
    )
    parser.add_argument(
        "--out-dir",
        default=str(DEFAULT_OUT_DIR),
        help=f"출력 폴더 (기본: {DEFAULT_OUT_DIR})",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    pillow = load_pillow()
    if pillow is None:
        return 2
    image_module, draw_module, font_class = pillow

    font_path = find_font(args.font)
    if font_path is None:
        print(
            "한글 폰트를 찾지 못했습니다. --font 또는 THERMOSTAT_KR_FONT로 경로를 지정하세요.",
            file=sys.stderr,
        )
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    codepoints = build_charset()
    print(f"font={font_path}")
    print(f"glyph count={len(codepoints)}")

    for pixel_size, symbol_name in FONT_SIZES.items():
        content = generate_font_c(
            image_module, draw_module, font_class, font_path, pixel_size,
            symbol_name, codepoints,
        )
        out_path = out_dir / f"{symbol_name}.c"
        with out_path.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
        print(f"wrote {out_path} ({len(content)} bytes)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
