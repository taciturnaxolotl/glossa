#!/usr/bin/env python3
"""
ocr_pipeline.py — Capture framebuffer region → OCR → return text

Usage:
    python ocr_pipeline.py [--region x,y,w,h] [--output text.txt]

Captures the reMarkable framebuffer via SSH, crops to the specified region,
runs PaddleOCR, and prints/saves the recognized text.
"""

import argparse
import subprocess
import tempfile
import struct
from pathlib import Path

# Framebuffer constants for rM2
FB_WIDTH = 1404
FB_HEIGHT = 1872
FB_BPP = 4  # RGBA
FB_DEVICE = "/dev/fb0"


def capture_framebuffer(region=None):
    """Capture framebuffer from device via SSH, return as raw bytes."""
    if region:
        x, y, w, h = region
        # Use dd to capture only the needed rows, then crop columns
        cmd = f"dd if={FB_DEVICE} bs={FB_WIDTH * FB_BPP} skip={y} count={h} 2>/dev/null"
    else:
        cmd = f"cat {FB_DEVICE}"
    
    result = subprocess.run(
        ["ssh", "remarkable", cmd],
        capture_output=True, timeout=10
    )
    
    if result.returncode != 0:
        raise RuntimeError(f"Failed to capture framebuffer: {result.stderr.decode()}")
    
    raw = result.stdout
    
    if region:
        x, y, w, h = region
        # Crop columns from each row
        cropped = b""
        row_size = FB_WIDTH * FB_BPP
        for row_y in range(h):
            row_start = row_y * row_size + x * FB_BPP
            cropped += raw[row_start:row_start + w * FB_BPP]
        return cropped, w, h
    else:
        return raw, FB_WIDTH, FB_HEIGHT


def raw_to_image(raw_bytes, width, height):
    """Convert raw framebuffer bytes to PIL Image."""
    from PIL import Image
    # rM2 framebuffer is BGR (not RGBA), 32bpp but only 24 bits used
    # Try BGR first, fall back to RGBA
    try:
        img = Image.frombytes("RGB", (width, height), raw_bytes, "raw", "BGR")
    except Exception:
        img = Image.frombytes("RGBA", (width, height), raw_bytes)
        img = img.convert("RGB")
    
    # E-ink displays often have inverted or low-contrast content
    # Auto-enhance contrast for better OCR
    from PIL import ImageEnhance
    enhancer = ImageEnhance.Contrast(img)
    img = enhancer.enhance(3.0)
    
    return img


def ocr_image(image):
    """Run PaddleOCR on a PIL Image, return recognized text."""
    from paddleocr import PaddleOCR
    
    ocr = PaddleOCR(use_textline_orientation=True, lang="en")
    
    # Save to temp file for PaddleOCR
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
        image.save(f.name)
        result = ocr.predict(f.name)
    
    # Extract text from results
    texts = []
    for item in result:
        if hasattr(item, 'rec_texts'):
            texts.extend(item.rec_texts)
        elif isinstance(item, dict) and 'rec_texts' in item:
            texts.extend(item['rec_texts'])
    
    return " ".join(texts)


def main():
    parser = argparse.ArgumentParser(description="OCR pipeline for reMarkable")
    parser.add_argument("--region", type=str, default=None,
                        help="Crop region as x,y,w,h (pixels)")
    parser.add_argument("--output", type=str, default=None,
                        help="Save recognized text to file")
    parser.add_argument("--image", type=str, default=None,
                        help="Use local image instead of capturing framebuffer")
    args = parser.parse_args()
    
    if args.image:
        from PIL import Image
        img = Image.open(args.image).convert("RGB")
        text = ocr_image(img)
    else:
        region = None
        if args.region:
            region = tuple(int(x) for x in args.region.split(","))
        
        print(f"Capturing framebuffer (region={region})...")
        raw, w, h = capture_framebuffer(region)
        print(f"Captured {w}x{h} ({len(raw)} bytes)")
        
        img = raw_to_image(raw, w, h)
        text = ocr_image(img)
    
    print(f"Recognized text: {text}")
    
    if args.output:
        Path(args.output).write_text(text)
        print(f"Saved to {args.output}")


if __name__ == "__main__":
    main()
