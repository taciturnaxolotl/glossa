#!/usr/bin/env python3
"""
Grimoire — inject handwriting-style text as native ink into reMarkable .rm v6 pages.

Reads an existing .rm v6 page, renders text using an SVG single-stroke font,
and appends SceneLineItemBlocks into Layer 1. Original blocks are untouched.

Usage:
    python grimoire.py input.rm output.rm               # default text
    python grimoire.py input.rm output.rm "hello"      # custom text
    python grimoire.py --preview "Sample"               # SVG preview
"""

import argparse
import math
import random
import sys
import xml.etree.ElementTree as ET
from io import BytesIO
from pathlib import Path
from typing import Optional

from rmscene import (
    CrdtId,
    CrdtSequenceItem,
    read_blocks,
    write_blocks,
    scene_items as si,
)
from rmscene.scene_stream import (
    SceneLineItemBlock,
)


# ───────────────────── Font loading ─────────────────────

def parse_svg_font(path: str) -> tuple[dict, dict[str, float]]:
    """Parse SVG single-stroke font. Returns (glyphs, advances)."""
    tree = ET.parse(path)
    root_e = tree.getroot()
    ns = {'svg': 'http://www.w3.org/2000/svg'}
    font_elem = root_e.find('.//svg:font', ns) or root_e.find('.//font')
    if font_elem is None:
        raise ValueError(f"No <font> in {path}")

    default_adv = float(font_elem.attrib.get('horiz-adv-x', 500))
    glyphs = {}
    advances = {' ': default_adv}

    for g in font_elem.findall('svg:glyph', ns) or font_elem.findall('glyph'):
        uni = g.attrib.get('unicode')
        if not uni:
            continue
        advances[uni] = float(g.attrib.get('horiz-adv-x', default_adv))
        d = g.attrib.get('d', '')
        if d:
            glyphs[uni] = _parse_d(d)
        else:
            glyphs[uni] = []
    return glyphs, advances


def _parse_d(d: str) -> list[list[tuple[float, float]]]:
    """Parse M/L path into polylines. Bezier commands are skipped."""
    polylines = []
    current = []
    tokens = d.replace(',', ' ').split()
    i = 0
    while i < len(tokens):
        cmd = tokens[i]
        if cmd in ('M', 'L'):
            i += 1
            if i + 1 >= len(tokens):
                break
            x, y = float(tokens[i]), float(tokens[i+1])
            i += 2
            if cmd == 'M':
                if len(current) >= 2:
                    polylines.append(current)
                current = [(x, y)]
            else:
                current.append((x, y))
        elif cmd in ('C', 'Q'):
            i += 1
            while i < len(tokens) and tokens[i] not in ('M', 'L', 'C', 'Q', 'Z', 'z'):
                i += 1
        elif cmd in ('Z', 'z'):
            if current and len(current) >= 2:
                current.append(current[0])
            i += 1
        else:
            i += 1
    if len(current) >= 2:
        polylines.append(current)
    return polylines


# ─── Global font cache
_FONT_CACHE: Optional[tuple[dict, dict[str, float]]] = None


def load_font(name: str = "EMSAllure") -> tuple[dict, dict[str, float]]:
    global _FONT_CACHE
    if _FONT_CACHE is not None:
        return _FONT_CACHE
    font_dir = Path(__file__).parent / "fonts"
    svg_path = font_dir / f"{name}.svg"
    if not svg_path.exists():
        raise FileNotFoundError(f"Font not found: {svg_path}")
    _FONT_CACHE = parse_svg_font(str(svg_path))
    return _FONT_CACHE


# ───────────────────── Text → strokes ─────────────────────

DEFAULT_SCALE = 0.065
DEFAULT_X = -550.0        # 80% of -702..701
DEFAULT_Y = 750.0
LINE_HEIGHT = 1.4         # multiplier on EM ascent (~800 units)
MAX_LINE_WIDTH = 1100     # 80% of 1404 = ~1120


def text_to_strokes(
    text: str,
    origin_x: float,
    origin_y: float,
    scale: float = DEFAULT_SCALE,
    max_width: float = MAX_LINE_WIDTH,
    line_spacing: float = LINE_HEIGHT,
) -> list[si.Line]:
    """Render text as reMarkable Line objects with word wrapping."""
    glyphs, advances = load_font()
    result = []
    cursor_x = origin_x
    cursor_y = origin_y
    rng = random.Random(text + str(origin_x))

    space_adv = advances.get(' ', 500) * scale

    for word in text.split():
        # measure word width
        word_width = sum(advances.get(ch, 500) for ch in word) * scale

        # wrap if needed
        if cursor_x + word_width > origin_x + max_width and cursor_x > origin_x:
            cursor_x = origin_x
            cursor_y += 800 * scale * line_spacing

        for ch in word:
            glyph = glyphs.get(ch)
            adv = advances.get(ch, 500) * scale
            if glyph is None:
                cursor_x += adv
                continue

            x_jitter = rng.uniform(-2, 2)
            y_jitter = rng.uniform(-1.5, 0.5)
            slant = rng.uniform(-0.015, 0.015)

            for polyline in glyph:
                if len(polyline) < 2:
                    continue
                points = []
                for j, (gx, gy) in enumerate(polyline):
                    sx = gx * scale
                    sy = -gy * scale  # SVG Y-up → rm Y-down
                    sx += sy * slant
                    wobble = math.sin(cursor_x * 0.01 + j * 0.3) * 0.8
                    px = cursor_x + sx + x_jitter
                    py = cursor_y + sy + y_jitter + wobble
                    points.append(si.Point(
                        x=px, y=py,
                        speed=4 + rng.randint(0, 8),
                        direction=80 + rng.randint(0, 30),
                        width=10 + rng.randint(0, 3),
                        pressure=160 + rng.randint(0, 30),
                    ))
                result.append(si.Line(
                    color=si.PenColor.BLACK,
                    tool=si.Pen.BALLPOINT_2,
                    points=points,
                    thickness_scale=2.0,
                    starting_length=0.0,
                ))

            cursor_x += adv + max(-2, x_jitter * 0.1) + abs(rng.uniform(-1, 1))

        # space between words
        cursor_x += space_adv + rng.uniform(-2, 2)

    return result


# ───────────────────── .rm injection ─────────────────────

def splice_reply(
    input_path: str,
    output_path: str,
    reply_text: str,
) -> None:
    """Read .rm v6 page, append reply strokes, write to output."""
    LAYER_1 = CrdtId(0, 11)
    with open(input_path, "rb") as f:
        original_data = f.read()

    strokes = text_to_strokes(reply_text, DEFAULT_X, DEFAULT_Y)
    blocks = []
    for i, line in enumerate(strokes):
        blocks.append(SceneLineItemBlock(
            parent_id=LAYER_1,
            item=CrdtSequenceItem(
                item_id=CrdtId(1, 200 + i),
                left_id=CrdtId(0, 0),
                right_id=CrdtId(0, 0),
                deleted_length=0,
                value=line,
            ),
        ))

    buf = BytesIO()
    write_blocks(buf, blocks, options={"version": "3.4"})
    reply_bytes = buf.getvalue()

    HEADER_SIZE = 43
    if reply_bytes[:HEADER_SIZE] == original_data[:HEADER_SIZE]:
        reply_bytes = reply_bytes[HEADER_SIZE:]

    output_data = original_data + reply_bytes
    with open(output_path, "wb") as f:
        f.write(output_data)

    print(f"Wrote {output_path} ({len(strokes)} strokes, "
          f"{len(original_data)}+{len(reply_bytes)}={len(output_data)} bytes)")


# ───────────────────── SVG preview ─────────────────────

def preview_svg(text: str, out_path: str = "preview.svg") -> None:
    strokes = text_to_strokes(text, DEFAULT_X, DEFAULT_Y)
    all_x = [p.x for line in strokes for p in line.points]
    all_y = [p.y for line in strokes for p in line.points]
    if not all_x:
        return
    min_x, max_x = min(all_x), max(all_x)
    min_y, max_y = min(all_y), max(all_y)
    pad = 20
    w, h = max_x - min_x + 2*pad, max_y - min_y + 2*pad

    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{min_x-pad} {min_y-pad} {w} {h}" width="{w*2}" height="{h*2}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<line x1="{min_x}" y1="0" x2="{max_x}" y2="0" stroke="#eee" stroke-width="0.5" stroke-dasharray="4,4"/>',
    ]
    for line in strokes:
        pts = line.points
        d = f'M{pts[0].x:.1f},{pts[0].y:.1f}'
        for p in pts[1:]:
            d += f'L{p.x:.1f},{p.y:.1f}'
        svg.append(
            f'<path d="{d}" fill="none" stroke="black" '
            f'stroke-width="{line.points[0].width*0.3:.1f}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>'
        )
    svg.append('</svg>')

    with open(out_path, "w") as f:
        f.write('\n'.join(svg))
    print(f"Preview {out_path} {len(strokes)} strokes")


# ───────────────────── CLI ─────────────────────

def main():
    parser = argparse.ArgumentParser(description="Grimoire — inject text as ink into .rm v6 pages")
    parser.add_argument("input", nargs="?", help="Input .rm file")
    parser.add_argument("output", nargs="?", help="Output .rm file (default: overwrite input)")
    parser.add_argument("text", nargs="?", default="Grimoire says hello",
                        help="Text to render")
    parser.add_argument("--preview", nargs="?", const="preview.svg",
                        help="Render SVG preview instead")
    args = parser.parse_args()

    if args.preview:
        out = args.preview if isinstance(args.preview, str) else "preview.svg"
        preview_svg(args.text, out)
        return

    if not args.input:
        parser.print_help()
        return

    splice_reply(args.input, args.output or args.input, args.text)


if __name__ == "__main__":
    main()
