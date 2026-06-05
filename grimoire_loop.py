#!/usr/bin/env python3
"""
grimoire_loop.py — Continuous closed loop daemon

Watches for pen-idle signal from xovi extension, then:
  capture → OCR → LLM → render → inject → repeat

Usage:
    python grimoire_loop.py [--model MODEL] [--debounce SECONDS]

The xovi extension writes /tmp/grimoire_idle when the pen has been
up for 2 seconds. This daemon watches that file via a persistent SSH
connection, pulls the framebuffer only when idle is detected, and
runs the full pipeline.
"""

import argparse
import base64
import hashlib
import io
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import requests
from dotenv import load_dotenv

load_dotenv()


# ─── Persistent SSH session ────────────────────────────────────────

class SSHSession:
    """Persistent SSH connection using ControlMaster multiplexing.

    Opens one SSH connection and reuses it for all subsequent commands,
    eliminating the ~1s handshake overhead per call.
    """

    def __init__(self, host="remarkable"):
        self.host = host
        self.socket = f"/tmp/grimoire_ssh_{os.getpid()}"
        self._open()

    def _open(self):
        """Establish master connection."""
        cmd = [
            "ssh", "-fN",
            "-o", "ControlMaster=yes",
            "-o", f"ControlPath={self.socket}",
            "-o", "ControlPersist=600",
            "-o", "ServerAliveInterval=30",
            "-o", "ServerAliveCountMax=3",
            self.host,
        ]
        result = subprocess.run(cmd, capture_output=True, timeout=15)
        if result.returncode != 0:
            raise RuntimeError(
                f"SSH master failed: {result.stderr.decode().strip()}"
            )
        print(f"[ssh] Master connection open ({self.socket})")

    def run(self, cmd, timeout=30, capture=True):
        """Run command on remote via multiplexed connection."""
        full_cmd = [
            "ssh",
            "-o", f"ControlPath={self.socket}",
            self.host,
            cmd,
        ]
        return subprocess.run(
            full_cmd,
            capture_output=capture,
            text=True,
            timeout=timeout,
        )

    def scp_from(self, remote_path, local_path, timeout=15):
        """Pull file from device via multiplexed connection."""
        cmd = [
            "scp",
            "-o", f"ControlPath={self.socket}",
            f"{self.host}:{remote_path}",
            local_path,
        ]
        return subprocess.run(cmd, capture_output=True, timeout=timeout)

    def scp_to(self, local_path, remote_path, timeout=15):
        """Push file to device via multiplexed connection."""
        cmd = [
            "scp",
            "-o", f"ControlPath={self.socket}",
            local_path,
            f"{self.host}:{remote_path}",
        ]
        return subprocess.run(cmd, capture_output=True, timeout=timeout)

    def close(self):
        """Tear down master connection."""
        subprocess.run(
            ["ssh", "-o", f"ControlPath={self.socket}", "-O", "exit", self.host],
            capture_output=True, timeout=5,
        )
        print("[ssh] Master connection closed")


def _ts():
    """Timestamp with millisecond precision for profiling."""
    return time.strftime('%H:%M:%S.') + f'{int(time.time() * 1000) % 1000:03d}'


def _elapsed(t0):
    """Seconds since t0, formatted for display."""
    return f'{time.time() - t0:.1f}s'


# ─── Framebuffer capture ──────────────────────────────────────────

def capture_framebuffer(ssh):
    """Trigger screenshot on device and pull raw framebuffer."""
    # Trigger via xovi watch thread
    ssh.run("touch /tmp/grimoire_screenshot", timeout=5)
    time.sleep(1)  # watch thread dumps FB nearly instantly

    result = ssh.scp_from("/tmp/grimoire_fb.raw", "/tmp/grimoire_fb.raw")
    if result.returncode != 0:
        raise RuntimeError(f"FB pull failed: {result.stderr.decode()}")

    from PIL import Image

    raw = open("/tmp/grimoire_fb.raw", "rb").read()
    img = Image.frombytes("RGBA", (1404, 1872), raw)
    img_rgb = img.convert("RGB")

    # Crop toolbar (top 80px)
    img_rgb = img_rgb.crop((0, 80, img_rgb.width, img_rgb.height))
    return img_rgb


# ─── Gemini Vision (OCR + LLM in one call) ────────────────────────

SYSTEM_PROMPT = (
    "You are an ink familiar bound to a paper notebook. The user writes "
    "questions by hand on a reMarkable tablet. You receive a screenshot of "
    "the current page.\n\n"
    "IMPORTANT CONTEXT:\n"
    "- Your previous replies appear as neat, uniform handwriting near the "
    "bottom of the page. IGNORE these completely.\n"
    "- The toolbar at the very top has printed icons. IGNORE these.\n"
    "- Background speckles and faint dots are e-ink artifacts. IGNORE these.\n"
    "- Only respond to NEW handwritten text that looks like a question or "
    "prompt from the user.\n\n"
    "RULES:\n"
    "- If there is no new user question, respond with exactly: [NO_NEW_TEXT]\n"
    "- If there IS a new question, answer briefly in plain prose. One or two "
    "short sentences max. No markdown, no emoji, no formatting, no bullet "
    "points. Just write your answer as if writing by hand.\n"
    "- Do not greet the user unless they greeted you first.\n"
    "- Do not narrate your reasoning."
)


def _image_to_base64(image):
    """Encode PIL Image as base64 PNG for Gemini API."""
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("utf-8")


def ask_gemini(image, conversation_history=None, model="gemini-2.5-flash-lite"):
    """Send cropped handwriting image to Gemini, return response text.

    Combines OCR and LLM into a single API call — Gemini reads the
    handwriting directly from the image. Includes conversation history
    for context across multiple exchanges.
    """
    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        raise RuntimeError("GEMINI_API_KEY not set in .env")

    img_b64 = _image_to_base64(image)

    # Build contents array with history + current turn
    contents = []
    if conversation_history:
        for role, text in conversation_history:
            contents.append({
                "role": role,
                "parts": [{"text": text}],
            })

    # Current turn: full page image
    contents.append({
        "role": "user",
        "parts": [
            {"text": "Here is the current page:"},
            {
                "inline_data": {
                    "mime_type": "image/png",
                    "data": img_b64,
                }
            },
        ],
    })

    r = requests.post(
        f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent",
        headers={"Content-Type": "application/json"},
        params={"key": api_key},
        json={
            "system_instruction": {
                "parts": [{"text": SYSTEM_PROMPT}],
            },
            "contents": contents,
        },
        timeout=60,
    )

    if r.status_code != 200:
        raise RuntimeError(f"Gemini API error {r.status_code}: {r.text[:200]}")

    data = r.json()
    try:
        content = data["candidates"][0]["content"]["parts"][0]["text"].strip()
    except (KeyError, IndexError):
        content = "..."

    # Strip any markdown/emoji that slipped through
    content = _strip_formatting(content)
    return content if content else "..."


def _framebuffer_hash(img):
    """Quick hash of downsampled image for page-change detection."""
    # Resize to small thumbnail for fast comparison
    thumb = img.resize((64, 85))
    import io
    buf = io.BytesIO()
    thumb.save(buf, format="PNG")
    return hashlib.md5(buf.getvalue()).hexdigest()


def _strip_formatting(text):
    """Remove markdown syntax and emoji from LLM output."""
    # Strip common markdown
    text = re.sub(r'\*\*(.+?)\*\*', r'\1', text)  # bold
    text = re.sub(r'\*(.+?)\*', r'\1', text)       # italic
    text = re.sub(r'`(.+?)`', r'\1', text)         # inline code
    text = re.sub(r'^#+\s*', '', text, flags=re.MULTILINE)  # headings
    text = re.sub(r'^\s*[-*+]\s+', '', text, flags=re.MULTILINE)  # list items
    # Strip emoji (broad unicode ranges)
    text = re.sub(
        r'[\U0001F300-\U0001FAFF\U00002702-\U000027B0'
        r'\U0001F600-\U0001F64F\U0001F680-\U0001F6FF'
        r'\U00002600-\U000026FF\U0000FE00-\U0000FE0F]+',
        '', text,
    )
    return text.strip()


# ─── Render + Inject ──────────────────────────────────────────────

def render_and_inject(ssh, text, reply_y=None):
    """Render text as strokes and inject via uinject."""
    json_path = "/tmp/grimoire_reply.json"

    cmd = [".venv/bin/python3", "grimoire.py", "--json", json_path]
    if reply_y is not None:
        cmd.extend(["--y", str(reply_y)])
    cmd.append(text)

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        raise RuntimeError(f"Render failed: {result.stderr.strip()}")
    print(f"    {result.stdout.strip()}")

    r = ssh.scp_to(json_path, "/tmp/grimoire_strokes.json")
    if r.returncode != 0:
        raise RuntimeError(f"SCP failed: {r.stderr.decode().strip()}")

    result = ssh.run("/home/root/uinject /tmp/grimoire_strokes.json", timeout=60)
    out = result.stdout.strip() if result.stdout else ""
    err = result.stderr.strip() if result.stderr else ""
    if result.returncode != 0:
        raise RuntimeError(f"Inject failed: {err}")
    last_line = out.split('\n')[-1] if out else "(no output)"
    print(f"    Inject: {last_line}")


# ─── Idle watching ────────────────────────────────────────────────

def watch_for_idle(ssh, last_hash):
    """Block until /tmp/grimoire_idle changes on device.

    Returns the new file hash, or None on error.
    Polls every 500ms via the persistent SSH connection.
    """
    while True:
        result = ssh.run("cat /tmp/grimoire_idle 2>/dev/null", timeout=5)
        if result.returncode == 0:
            content = result.stdout.strip()
            h = hashlib.md5(content.encode()).hexdigest()
            if h != last_hash:
                return h
        time.sleep(0.5)


# ─── Pixel diff for new content detection ──────────────────────────

def _find_new_region(current_img, last_img, ignore_below_y=None):
    """Compare two framebuffer images, return crop of changed region.

    Returns (cropped_image, content_bottom_y) or (None, 0) if no
    significant change. Uses simple pixel difference to find where
    new handwriting appeared.

    ignore_below_y: if set, ignore all changes below this Y coordinate
    in the cropped image. Used to skip grimoire's own injected strokes.
    """
    import numpy as np

    curr = np.array(current_img.convert('L'))

    if last_img is None:
        # First capture — find bounding box of existing handwriting.
        # Use a strict threshold to ignore background noise/dots.
        dark = curr < 128
        if not dark.any():
            return None, 0
        rows = np.any(dark, axis=1)
        cols = np.any(dark, axis=0)
        y1, y2 = np.where(rows)[0][[0, -1]]
        x1, x2 = np.where(cols)[0][[0, -1]]
        pad = 20
        y1 = max(0, y1 - pad)
        y2 = min(curr.shape[0] - 1, y2 + pad)
        x1 = max(0, x1 - pad)
        x2 = min(curr.shape[1] - 1, x2 + pad)
        # Sanity check: if the bounding box covers >80% of the screen,
        # it's probably noise, not real content.
        if (y2 - y1) > curr.shape[0] * 0.8 and (x2 - x1) > curr.shape[1] * 0.8:
            return None, 0
        crop = current_img.crop((x1, y1, x2 + 1, y2 + 1))
        return crop, y2

    prev = np.array(last_img.convert('L'))

    # Build mask: ignore edges (e-ink artifacts) and previously injected area
    mask = np.zeros_like(curr, dtype=bool)
    top_margin = 10
    bottom_margin = 20
    mask[top_margin:curr.shape[0] - bottom_margin, :] = True

    if ignore_below_y is not None:
        # Convert device Y back to cropped-image Y (subtract toolbar offset)
        cutoff = ignore_below_y - 80
        if 0 < cutoff < curr.shape[0]:
            mask[cutoff:, :] = False

    # Find pixels that changed significantly
    diff = (np.abs(curr.astype(int) - prev.astype(int)) > 50) & mask

    if not diff.any():
        return None, 0

    # Find bounding box of changed pixels
    rows = np.any(diff, axis=1)
    cols = np.any(diff, axis=0)
    y1, y2 = np.where(rows)[0][[0, -1]]
    x1, x2 = np.where(cols)[0][[0, -1]]

    pad = 30
    y1 = max(0, y1 - pad)
    y2 = min(curr.shape[0] - 1, y2 + pad)
    x1 = max(0, x1 - pad)
    x2 = min(curr.shape[1] - 1, x2 + pad)

    # Minimum size check
    if (y2 - y1) < 10 or (x2 - x1) < 10:
        return None, 0

    crop = current_img.crop((x1, y1, x2 + 1, y2 + 1))
    return crop, y2


# ─── Main loop ────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Grimoire continuous loop")
    parser.add_argument(
        "--model", type=str, default="gemini-2.5-flash-lite",
        help="Gemini model",
    )
    parser.add_argument(
        "--once", action="store_true",
        help="Run one cycle then exit (for testing)",
    )
    args = parser.parse_args()

    print("=== Grimoire Loop ===")
    print(f"Model: {args.model}")
    print("Waiting for pen idle signal...")
    print()

    ssh = SSHSession("remarkable")
    last_idle_hash = ""
    last_page_hash = None # hash for page-change detection
    last_fb_hash = None   # quick hash to skip unchanged frames
    conversation = []     # list of (role, text) tuples for context
    last_inject_y = None  # device Y where we last injected

    try:
        while True:
            # Wait for idle signal
            new_hash = watch_for_idle(ssh, last_idle_hash)
            if new_hash is None:
                continue
            last_idle_hash = new_hash

            t0 = time.time()
            print(f"[{_ts()}] Pen idle detected!")

            try:
                # Capture
                print(f"  [{_ts()}] Capturing...")
                img = capture_framebuffer(ssh)
                print(f"  [{_ts()}] Captured ({_elapsed(t0)})")

                # Quick hash check — skip if framebuffer unchanged
                fb_hash = _framebuffer_hash(img)
                if fb_hash == last_fb_hash:
                    print(f"  [{_ts()}] Framebuffer unchanged, skipping.")
                    print()
                    continue
                last_fb_hash = fb_hash

                # Page change detection — only reset if we haven't just
                # injected (our own strokes change the hash too).
                is_own_injection = (last_inject_y is not None and
                                    last_page_hash is not None and
                                    fb_hash != last_page_hash)
                if last_page_hash is not None and fb_hash != last_page_hash:
                    if is_own_injection:
                        print(f"  [{_ts()}] Hash changed (own injection), keeping context.")
                    else:
                        print(f"  [{_ts()}] Page changed, resetting context.")
                        conversation = []
                        last_inject_y = None
                last_page_hash = fb_hash

                # Send full page to Gemini — it handles OCR + relevance filtering
                print(f"  [{_ts()}] Asking Gemini ({len(conversation)} history turns)...")
                answer = ask_gemini(img, conversation, args.model)
                print(f"  [{_ts()}] Reply ({_elapsed(t0)}): {answer[:80]}{'...' if len(answer) > 80 else ''}")

                # Skip if Gemini says no new text
                if "[NO_NEW_TEXT]" in answer:
                    print(f"  [{_ts()}] No new text detected by Gemini, skipping.")
                    last_img = img
                    print()
                    continue

                # Add to conversation history
                conversation.append(("user", f"[handwritten question]"))
                conversation.append(("model", answer))

                # Position reply below existing content.
                # The reMarkable display has a uniform grid artifact:
                # isolated 2-row pairs with exactly 68 dark pixels each.
                # Real handwriting has variable pixel counts across many
                # consecutive rows. Filter by requiring non-68 counts or
                # clusters taller than 4 rows.
                import numpy as np
                gray = np.array(img.convert('L'))
                dark_per_row = np.sum(gray < 128, axis=1)

                # Mark rows as "real content" if they have dark pixels
                # AND the count isn't exactly 68 (grid artifact), OR
                # they're part of a cluster > 4 rows tall.
                has_dark = dark_per_row > 5
                not_grid = dark_per_row != 68
                candidate_rows = np.where(has_dark & not_grid)[0]

                content_bottom = None
                if len(candidate_rows) > 0:
                    # Group into contiguous clusters
                    gaps = np.diff(candidate_rows)
                    clusters = []
                    start = candidate_rows[0]
                    for i, g in enumerate(gaps):
                        if g > 10:
                            clusters.append((start, candidate_rows[i]))
                            start = candidate_rows[i + 1]
                    clusters.append((start, candidate_rows[-1]))

                    # Use bottom of last real cluster
                    if clusters:
                        content_bottom = clusters[-1][1]

                if content_bottom is not None:
                    reply_y = min(content_bottom + 80 + 150, 1750)
                else:
                    reply_y = 300
                print(f"  [{_ts()}] Content bottom: {content_bottom}, reply_y={reply_y}")

                # Render + Inject
                print(f"  [{_ts()}] Rendering + injecting (reply_y={reply_y})...")
                render_and_inject(ssh, answer, reply_y=reply_y)
                last_inject_y = reply_y
                print(f"  [{_ts()}] Injected ({_elapsed(t0)})")

                elapsed = time.time() - t0
                print(f"  [{_ts()}] Done in {elapsed:.1f}s")

            except Exception as e:
                print(f"  ERROR: {e}")

            print()

            if args.once:
                break

    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        ssh.close()


if __name__ == "__main__":
    main()
