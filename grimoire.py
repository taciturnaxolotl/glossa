#!/usr/bin/env python3
"""
grimoire.py — Appends reply text as native ink to the user's handwriting.

Reads the user's .rm page, appends a NEW LAYER (Layer 2) with reply strokes
rendered with EMS Allure font.

Usage:
    python grimoire.py source.rm output.rm "reply text"
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
    LwwValue,
    read_blocks,
    write_blocks,
    scene_items as si,
)
from rmscene.scene_stream import (
    SceneLineItemBlock,
    SceneTreeBlock,
    TreeNodeBlock,
    SceneGroupItemBlock,
)


def parse_svg_font(path: str) -> tuple[dict, dict[str, float]]:
    tree = ET.parse(path)
    root_e = tree.getroot()
    ns = {'svg': 'http://www.w3.org/2000/svg'}
    font_elem = root_e.find('.//svg:font', ns) or root_e.find('.//font')
    if font_elem is None:
        raise ValueError(f"Missing <font> in {path}")
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
            # skip bezier control points in batch
            i += 1
            while i < len(tokens) and tokens[i] not in ('M', 'L', 'C', 'Q', 'Z', 'z'):
                i += 1
            if current and len(current) >= 2:
                current.append(current[0])
            i += 1
        else:
            i += 1
    if len(current) >= 2:
        polylines.append(current)
    return polylines


def _densify(
    pts: list[tuple[float, float]], min_seg_len: float = 120.0
) -> list[tuple[float, float]]:
    """Subdivide a polyline so no segment is longer than min_seg_len.

    Short subpaths (single line segments) render unreliably on device —
    the renderer needs several samples to draw a visible stroke. This walks
    each segment in glyph-space (pre-scale) and inserts evenly spaced
    midpoints, guaranteeing at least one interior point per segment.
    """
    if len(pts) < 2:
        return pts
    out = [pts[0]]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        dist = math.hypot(x1 - x0, y1 - y0)
        steps = max(2, int(math.ceil(dist / min_seg_len)))
        for s in range(1, steps + 1):
            t = s / steps
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
    return out


_FONT_CACHE: Optional[tuple[dict, dict[str, float]]] = None
DEFAULT_SCALE = 0.07
DEFAULT_X = -550.0
DEFAULT_Y = 200.0
LINE_HEIGHT = 1.4
MAX_LINE_WIDTH = 1100


def load_font(name: str = "EMSAllure") -> tuple[dict, dict[str, float]]:
    global _FONT_CACHE
    if _FONT_CACHE is not None:
        return _FONT_CACHE
    font_dir = Path(__file__).parent / "fonts"
    svg_path = font_dir / f"{name}.svg"
    _FONT_CACHE = parse_svg_font(str(svg_path))
    return _FONT_CACHE


def text_to_strokes(
    text: str,
    origin_x: float,
    origin_y: float,
    scale: float = DEFAULT_SCALE,
    max_width: float = MAX_LINE_WIDTH,
    line_spacing: float = LINE_HEIGHT,
) -> list[si.Line]:
    glyphs, advances = load_font()
    result = []
    cursor_x = origin_x
    cursor_y = origin_y
    rng = random.Random(text + str(origin_x))
    space_adv = advances.get(' ', 500) * scale

    for word in text.split():
        word_width = sum(advances.get(ch, 500) for ch in word) * scale
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
                # Densify: subdivide each segment so even short subpaths
                # (e.g. the arms of '*', the bowl of 'p') carry enough
                # points for the device renderer, which collapses or drops
                # 2-point strokes. Insert midpoints until every stroke has
                # at least a few samples per segment.
                dense = _densify(polyline, min_seg_len=120.0)
                points = []
                for j, (gx, gy) in enumerate(dense):
                    sx = gx * scale
                    sy = -gy * scale
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
        cursor_x += space_adv + rng.uniform(-2, 2)

    return result


def _find_content_bottom(data: bytes) -> float:
    """Scan existing strokes and return the max Y coordinate + margin.

    Used to position reply text below the user's handwriting instead of
    overwriting it. Returns DEFAULT_Y if no strokes are found.
    """
    max_y = 0.0
    found = False
    try:
        with BytesIO(data) as f:
            for block in read_blocks(f):
                if isinstance(block, SceneLineItemBlock):
                    line = block.item.value
                    if hasattr(line, 'points'):
                        for pt in line.points:
                            if pt.y > max_y:
                                max_y = pt.y
                            found = True
    except Exception:
        pass
    if not found:
        return DEFAULT_Y
    # Place reply below existing content with generous spacing
    return max_y + 120.0


def _find_last_root_child(data: bytes) -> CrdtId:
    """Find the last SceneGroupItemBlock value under root for left_id chaining."""
    from io import BytesIO
    ROOT = CrdtId(0, 1)
    last_value = CrdtId(0, 0)
    try:
        with BytesIO(data) as f:
            for block in read_blocks(f):
                if isinstance(block, SceneGroupItemBlock) and block.parent_id == ROOT:
                    last_value = block.item.value
    except Exception:
        pass
    return last_value


def splice_reply_new_layer(
    input_path: str,
    output_path: str,
    reply_text: str,
) -> None:
    """
    Read .rm v6 page, APPEND a new layer with reply strokes.
    Original blocks are untouched. Matches real xochitl layer structure.
    """
    with open(input_path, "rb") as f:
        original_data = f.read()

    # Build reply as strokes, positioned below existing content
    reply_y = _find_content_bottom(original_data)
    strokes = text_to_strokes(reply_text, DEFAULT_X, reply_y)

    # CRDT IDs — author 2 for programmatic content, starting at seq 500
    # to avoid collision with existing data (real Layer 2 uses ~422-427)
    LAYER_TREE = CrdtId(2, 500)   # tree_id for the new layer
    ROOT = CrdtId(0, 1)

    # Find the last group item under root to chain left_id correctly
    last_root_child = _find_last_root_child(original_data)

    # 1. Content tree root: SceneTree(tree_id=layer, node_id=(0,0), is_update=True)
    content_tree = SceneTreeBlock(
        tree_id=LAYER_TREE,
        node_id=CrdtId(0, 0),
        is_update=True,
        parent_id=ROOT,
    )

    # 2. Layer metadata: TreeNodeBlock with label and visibility
    label_block = TreeNodeBlock(si.Group(
        node_id=LAYER_TREE,
        label=LwwValue(CrdtId(2, 501), "grimoire"),
        visible=LwwValue(CrdtId(2, 502), True),
    ))

    # 3. Add layer to root's children list (SceneGroupItemBlock)
    group_item_block = SceneGroupItemBlock(
        parent_id=ROOT,
        item=CrdtSequenceItem(
            item_id=CrdtId(2, 503),
            left_id=last_root_child,
            right_id=CrdtId(0, 0),
            deleted_length=0,
            value=LAYER_TREE,
        ),
    )

    reply_blocks = [content_tree, label_block, group_item_block]

    # 4. Line items parented to the layer tree_id
    for i, line in enumerate(strokes):
        reply_blocks.append(SceneLineItemBlock(
            parent_id=LAYER_TREE,
            item=CrdtSequenceItem(
                item_id=CrdtId(2, 600 + i),
                left_id=CrdtId(0, 0) if i == 0 else CrdtId(2, 600 + i - 1),
                right_id=CrdtId(0, 0),
                deleted_length=0,
                value=line,
            ),
        ))

    # Serialize reply blocks
    buf = BytesIO()
    write_blocks(buf, reply_blocks, options={"version": "3.4"})
    reply_bytes = buf.getvalue()

    HEADER_SIZE = 43
    if reply_bytes[:HEADER_SIZE] == original_data[:HEADER_SIZE]:
        reply_bytes = reply_bytes[HEADER_SIZE:]

    output_data = original_data + reply_bytes

    with open(output_path, "wb") as f:
        f.write(output_data)

    print(f"Wrote {output_path} ({len(strokes)} reply strokes, "
          f"{len(original_data)}+{len(reply_bytes)}={len(output_data)} bytes)")


def strokes_to_json(strokes: list[si.Line], output_path: str) -> None:
    """Export strokes as JSON for the xovi grimoire-injector extension."""
    import json
    items = []
    for line in strokes:
        points = []
        for pt in line.points:
            points.append([pt.x, pt.y, pt.speed, pt.width, pt.direction, pt.pressure])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        bounds = [min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)]
        items.append({
            "points": points,
            "rgba": 4278190080,  # 0xFF000000
            "color": line.color.value if hasattr(line.color, 'value') else 0,
            "bounds": bounds,
            "tool": line.tool.value if hasattr(line.tool, 'value') else 15,
            "maskScale": line.thickness_scale,
            "thickness": line.thickness_scale,
        })
    with open(output_path, "w") as f:
        json.dump(items, f)
    print(f"Wrote {output_path} ({len(items)} strokes)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", nargs="?", default=None)
    parser.add_argument("output", nargs="?", default=None)
    parser.add_argument("text", nargs="?", default=None)
    parser.add_argument("--json", dest="json_output", default=None,
                        help="Export strokes as JSON for xovi injector")
    parser.add_argument("--y", type=float, default=None,
                        help="Y coordinate to start rendering at (overrides default)")
    args = parser.parse_args()

    if args.json_output:
        # --json <output.json> [text]
        text = args.input or "Grimoire says hello"
        y = args.y if args.y is not None else DEFAULT_Y
        strokes = text_to_strokes(text, DEFAULT_X, y)
        strokes_to_json(strokes, args.json_output)
    elif args.input:
        text = args.text or "Grimoire says hello"
        output = args.output or args.input
        splice_reply_new_layer(args.input, output, text)
    else:
        parser.error("Provide input.rm or --json output.json")
