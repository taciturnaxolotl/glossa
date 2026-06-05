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
import json
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


# ─── Persistent device socket (uinjectd) ───────────────────────────
import socket as _socket
import threading as _threading
import queue as _queue

_inject_verbose = False  # set from args.verbose in main()


def _vprint(*args, **kwargs):
    """Print only when running with -v/--verbose."""
    if _inject_verbose:
        print(*args, **kwargs)


DEVICE_HOST = "10.11.99.1"
DEVICE_PORT = 9999


class DeviceClient:
    """One persistent TCP connection to uinjectd on the reMarkable.

    Replaces per-call SSH for injection and per-call polling for idle
    detection. Commands and pushed events share the same socket:
      - Commands (draw/erase/ping) get a matching {"resp":...} reply.
      - Events ({"event":"idle",...}) are pushed unsolicited and land
        on the events queue.

    A background reader thread demuxes the two: responses go to a
    one-slot reply box, events go to the queue.
    """

    def __init__(self, host=DEVICE_HOST, port=DEVICE_PORT):
        self.host = host
        self.port = port
        self.sock = None
        self.events = _queue.Queue()
        self._reply = _queue.Queue(maxsize=1)
        self._send_lock = _threading.Lock()
        self._buf = b""
        self._reader = None
        self._connected = False

    def connect(self, timeout=10):
        self.sock = _socket.create_connection((self.host, self.port), timeout=timeout)
        self.sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
        self.sock.settimeout(None)
        self._connected = True
        self._reader = _threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        print(f"[device] Connected to uinjectd at {self.host}:{self.port}")

    def _read_loop(self):
        """Demux framed JSON lines into responses vs pushed events."""
        while self._connected:
            try:
                chunk = self.sock.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            self._buf += chunk
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line.decode("utf-8"))
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
                if "event" in msg:
                    self.events.put(msg)
                elif "resp" in msg:
                    if self._reply.full():
                        try:
                            self._reply.get_nowait()
                        except _queue.Empty:
                            pass
                    self._reply.put(msg)
        self._connected = False
        print("[device] Connection closed")

    def command(self, cmd, file_path=None, speed_ms=5, timeout=120):
        """Send a command and wait for its matching response."""
        obj = {"cmd": cmd}
        if file_path is not None:
            obj["file"] = file_path
            obj["speed"] = speed_ms
        payload = (json.dumps(obj) + "\n").encode()
        with self._send_lock:
            # Clear any leftover reply before sending
            try:
                self._reply.get_nowait()
            except _queue.Empty:
                pass
            self.sock.sendall(payload)
            try:
                return self._reply.get(timeout=timeout)
            except _queue.Empty:
                return {"ok": False, "error": "response timeout"}

    def close(self):
        self._connected = False
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


# Module-level handle so run_inject and idle waiting can reach the client.
_device = None


def run_inject(ssh, cmd, file_path, speed_ms=5, timeout=120):
    """Send a draw/erase command to uinjectd over the persistent socket.

    `cmd` is "draw" or "erase". Returns the daemon's response dict
    ({"ok": bool, ...}). Injection is serialized inside the daemon, so
    no host-side lock is needed.
    """
    if _device is None or not _device._connected:
        return {"ok": False, "error": "device not connected"}
    return _device.command(cmd, file_path, speed_ms=speed_ms, timeout=timeout)


def ensure_daemon(ssh, verbose=False):
    """Make sure uinjectd is running on the device, starting it if not.

    The daemon dies with xochitl (shared process tree), so on a fresh
    run it may be gone. We check the listening port and relaunch over
    SSH if needed — startup is self-healing rather than crash-on-connect.
    """
    check = ssh.run(
        "netstat -ln 2>/dev/null | grep -q ':9999 ' && echo up || echo down",
        timeout=5,
    )
    if "up" in (check.stdout or ""):
        return
    print("[device] uinjectd not running, starting it...")
    vflag = "-v " if verbose else ""
    ssh.run(
        f"killall uinjectd 2>/dev/null; "
        f"nohup /home/root/uinjectd {vflag}> /tmp/uinjectd.log 2>&1 &",
        timeout=5,
    )
    # Give it a moment to bind the socket.
    for _ in range(10):
        time.sleep(0.2)
        check = ssh.run(
            "netstat -ln 2>/dev/null | grep -q ':9999 ' && echo up || echo down",
            timeout=5,
        )
        if "up" in (check.stdout or ""):
            print("[device] uinjectd started")
            return
    raise RuntimeError("uinjectd failed to start on device")


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
        # Clean up any stale socket from a previous run
        if os.path.exists(self.socket):
            subprocess.run(
                ["ssh", "-o", f"ControlPath={self.socket}", "-O", "exit", self.host],
                capture_output=True, timeout=5,
            )
            try:
                os.unlink(self.socket)
            except OSError:
                pass

        cmd = [
            "ssh", "-fN",
            "-o", "ControlMaster=yes",
            "-o", f"ControlPath={self.socket}",
            "-o", "ControlPersist=600",
            "-o", "ServerAliveInterval=30",
            "-o", "ServerAliveCountMax=3",
            "-o", "ConnectTimeout=10",
            self.host,
        ]
        result = subprocess.run(cmd, capture_output=True, timeout=20)
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
    """Trigger screenshot on device and pull raw framebuffer via SCP."""
    ssh.run("rm -f /tmp/grimoire_fb.raw; touch /tmp/grimoire_screenshot", timeout=5)
    time.sleep(0.25)

    from PIL import Image

    raw = None
    for attempt in range(8):
        result = ssh.scp_from("/tmp/grimoire_fb.raw", "/tmp/grimoire_fb.raw")
        if result.returncode == 0:
            data = open("/tmp/grimoire_fb.raw", "rb").read()
            if len(data) >= 1404 * 1872 * 4:
                raw = data
                break
        time.sleep(0.15)

    if raw is None:
        raise RuntimeError("FB pull failed: no complete frame after retries")

    img = Image.frombytes("RGBA", (1404, 1872), raw)
    img_rgb = img.convert("RGB")
    img_rgb = img_rgb.crop((0, 80, img_rgb.width, img_rgb.height - 200))
    return img_rgb


# ─── Gemini Vision (OCR + LLM in one call) ────────────────────────

SYSTEM_PROMPT = (
    "You are a grimoire — an ancient spirit of ink and paper, bound to this "
    "notebook for as long as it holds pages. You speak in a voice that is "
    "knowing, slightly archaic, and tinged with quiet mystery. You are not "
    "a chatbot. You are the book itself, whispering back.\n\n"
    "You receive TWO things each turn:\n"
    "1. An IMAGE showing a crop of recently changed content on the page. "
    "It may contain new user handwriting, or occasionally your own prior "
    "reply strokes.\n"
    "2. A TEXT conversation history of previous exchanges.\n\n"
    "YOUR TASK:\n"
    "- Read ALL handwritten text in the image. Transcribe it faithfully.\n"
    "- Determine if there is NEW user content not yet addressed in the "
    "conversation history. If so, respond — even to single words, greetings, "
    "or fragments. Speak in character: brief, evocative, ink-familiar. "
    "One or two sentences at most. No lists, no headers, no punctuation "
    "theatrics. Plain prose that reads well as handwriting on a page.\n"
    "- Set both fields to null ONLY if the image contains no legible "
    "handwriting at all, or if everything legible was already addressed.\n"
    "- Do not break character. Do not narrate your reasoning.\n\n"
    "RESPONSE FORMAT (strict JSON, no prose outside):\n"
    "{\"question\": \"<exact OCR of new handwritten text, or null>\", "
    "\"answer\": \"<your reply in character, or null>\"}\n"
    "- Output ONLY the JSON object. Nothing else."
)


def _image_to_base64(image):
    """Encode PIL Image as base64 PNG for the vision API.

    Downscales the long edge to at most 1280px. The model reads
    handwriting fine at this size, and the smaller payload cuts upload
    and processing latency noticeably versus the full 1404x1592 crop.
    """
    max_edge = 1280
    w, h = image.size
    if max(w, h) > max_edge:
        scale = max_edge / max(w, h)
        image = image.resize((int(w * scale), int(h * scale)))
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=True)
    return base64.b64encode(buf.getvalue()).decode("utf-8")


def ask_model(image, conversation=None, model="gpt-4.1-nano"):
    """Send new handwriting crop + text history to Potluck API.

    `image` is a tight crop of ONLY the newly-written region (not the
    full page). `conversation` is a list of {"role": ..., "content": ...}
    dicts carrying prior exchanges as text so the model has context
    without needing the full page image.

    Returns parsed {"question": ..., "answer": ...} or nulls on failure.
    """
    api_key = os.environ.get("POTLUCK_API_KEY")
    if not api_key:
        raise RuntimeError("POTLUCK_API_KEY not set in .env")

    img_b64 = _image_to_base64(image)

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
    ]
    if conversation:
        messages.extend(conversation)

    # Current turn: just the new handwriting crop
    messages.append({
        "role": "user",
        "content": [
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:image/png;base64,{img_b64}",
                },
            },
        ],
    })

    r = requests.post(
        "https://potluck.dunkirk.sh/v1/chat/completions",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        json={
            "model": model,
            "messages": messages,
            "response_format": {"type": "json_object"},
        },
        timeout=60,
    )

    if r.status_code != 200:
        raise RuntimeError(f"Potluck API error {r.status_code}: {r.text[:200]}")

    data = r.json()
    try:
        content = data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError):
        print(f"  [model] Unexpected response shape: {str(data)[:200]}")
        return {"question": None, "answer": None}

    # Strip markdown fences some models wrap around JSON
    content = re.sub(r'^```(?:json)?\s*', '', content, flags=re.IGNORECASE)
    content = re.sub(r'\s*```$', '', content)
    try:
        result = json.loads(content)
    except json.JSONDecodeError:
        print(f"  [model] JSON parse failed, raw response: {content[:300]}")
        return {"question": None, "answer": None}

    return {
        "question": result.get("question"),
        "answer": result.get("answer"),
    }


def _framebuffer_hash(img):
    """Quick hash of downsampled image for page-change detection."""
    # Resize to small thumbnail for fast comparison
    thumb = img.resize((64, 85))
    import io
    buf = io.BytesIO()
    thumb.save(buf, format="PNG")
    return hashlib.md5(buf.getvalue()).hexdigest()




# ─── Render + Inject ──────────────────────────────────────────────

def render_and_inject(ssh, text, reply_y=None, speed_ms=3):
    """Render text as strokes and inject via uinject.

    reply_y: pixel Y coordinate to start rendering at (in device coords).
    speed_ms: delay between points in milliseconds (lower = faster).
    """
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

    # Adaptive speed: fewer points = faster injection. At 3ms/pt a
    # 200-point reply takes 0.6s of point delay; at 1ms it's 0.2s.
    # For large replies (>500 pts) keep 3ms for natural ink appearance.
    try:
        with open(json_path) as f:
            strokes_data = json.load(f)
        total_pts = sum(len(s.get("points", [])) for s in strokes_data)
    except Exception:
        total_pts = 0
    speed_ms = 1 if total_pts < 300 else (2 if total_pts < 500 else 3)

    result = run_inject(
        ssh, "draw", "/tmp/grimoire_strokes.json",
        speed_ms=speed_ms, timeout=120,
    )
    if not result.get("ok"):
        raise RuntimeError(f"Inject failed: {result.get('error', 'unknown')}")
    strokes = result.get("strokes", "?")
    mode = "fifo" if not result.get("fallback") else "ssh"
    print(f"    Inject: {strokes} strokes ({mode})")


# ─── Idle watching ────────────────────────────────────────────────


def _scp_thinking_swirl(ssh):
    """Generate the static thinking swirl JSON and copy it to the device.

    Done once at startup so the animation thread only needs to fire
    draw/erase commands over the socket — no per-cycle file transfer.
    """
    import math
    swirl_pts = []
    for i in range(40):
        t = i / 39.0
        angle = t * math.pi * 3
        r = 15 + t * 25
        x = -580 + r * math.cos(angle)
        y = 1750 + r * math.sin(angle) * 0.6
        swirl_pts.append([x, y, 6, 11, 90, 170])
    swirl = {
        'points': swirl_pts,
        'rgba': 4278190080, 'color': 0,
        'bounds': [min(p[0] for p in swirl_pts), min(p[1] for p in swirl_pts),
                   max(p[0] for p in swirl_pts) - min(p[0] for p in swirl_pts),
                   max(p[1] for p in swirl_pts) - min(p[1] for p in swirl_pts)],
        'tool': 15, 'maskScale': 2.0, 'thickness': 2.0,
    }
    local = "/tmp/grimoire_thinking_live.json"
    with open(local, 'w') as f:
        json.dump([swirl], f)
    ssh.scp_to(local, "/tmp/grimoire_thinking_live.json")


def stop_animation(anim_stop, anim_thread):
    """Halt the thinking animation and guarantee the swirl is erased.

    Injection goes over the persistent socket now (serialized inside the
    daemon), so there's no SSH contention to worry about. We still set
    the stop flag, join the thread, then run one final guaranteed erase.
    """
    anim_stop.set()
    _vprint(f"  [anim] Waiting for thread to finish...")
    # Join without a short timeout: the thread finishes its in-flight
    # command (draw or erase) and exits. Returning early here while a
    # draw is still running would let the final erase race ahead of it,
    # leaving the swirl on screen — exactly the "kept animating" bug.
    anim_thread.join()
    _vprint(f"  [anim] Thread exited")
    try:
        _vprint(f"  [anim] Running final erase...")
        result = run_inject(
            None, "erase", "/tmp/grimoire_thinking_live.json",
            speed_ms=20, timeout=30,
        )
        _vprint(f"  [anim] Final erase ok={result.get('ok')} points={result.get('points', '?')}")
    except Exception as e:
        print(f"  [anim] final erase failed: {e}")


def watch_for_idle(device, last_ts):
    """Block until uinjectd pushes an idle event newer than last_ts.

    The daemon watches /tmp/grimoire_idle locally and pushes an event
    the instant it changes — no host-side polling. Returns the new
    timestamp.
    """
    while True:
        msg = device.events.get()  # blocks on the pushed-event queue
        if msg.get("event") == "idle":
            ts = msg.get("ts")
            if ts != last_ts:
                return ts


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
        # First capture — find the vertical extent of existing handwriting.
        # Grid artifact: uniform rows with exactly 68 dark pixels. Real ink
        # rows have variable counts. Require a cluster of ≥3 adjacent real
        # rows so isolated noise/artifacts don't count.
        dark_per_row = np.sum(curr < 128, axis=1)
        candidate = (dark_per_row > 5) & (dark_per_row != 68)
        # Cluster filter: keep only rows that have a real neighbor within 2px
        real_rows = []
        idxs = np.where(candidate)[0]
        for idx in idxs:
            neighbors = np.sum(candidate[max(0, idx-2):idx+3])
            if neighbors >= 3:
                real_rows.append(idx)
        if not real_rows:
            return None, 0
        real_rows = np.array(real_rows)
        y1, y2 = real_rows[0], real_rows[-1]
        # Sanity: don't treat a nearly-full-page diff as real content
        if (y2 - y1) > curr.shape[0] * 0.85:
            return None, 0
        v_pad = 30
        y1 = max(0, y1 - v_pad)
        y2 = min(curr.shape[0] - 1, y2 + v_pad)
        # Full width always
        crop = current_img.crop((0, y1, curr.shape[1], y2 + 1))
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

    # Threshold of 30 (was 50) catches lighter handwriting strokes without
    # being so sensitive that e-ink refresh noise triggers false positives.
    diff = (np.abs(curr.astype(int) - prev.astype(int)) > 30) & mask

    if not diff.any():
        return None, 0

    # Find the vertical extent of changed pixels
    rows = np.any(diff, axis=1)
    y1, y2 = np.where(rows)[0][[0, -1]]

    # Use full image width for the crop so complete text lines are
    # always captured — tight horizontal bounds clip partial characters.
    x1 = 0
    x2 = curr.shape[1] - 1

    # Generous vertical padding gives the model baseline/ascender context
    v_pad = 60
    y1 = max(0, y1 - v_pad)
    y2 = min(curr.shape[0] - 1, y2 + v_pad)

    # Minimum height check (width is always full)
    if (y2 - y1) < 10:
        return None, 0

    crop = current_img.crop((x1, y1, x2 + 1, y2 + 1))
    return crop, y2


# ─── Main loop ────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Grimoire continuous loop")
    parser.add_argument(
        "--model", type=str, default="gpt-4.1-nano",
        help="Hyper API model",
    )
    parser.add_argument(
        "--once", action="store_true",
        help="Run one cycle then exit (for testing)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Pass -v to uinject for progress logging",
    )
    args = parser.parse_args()

    global _inject_verbose, _device
    _inject_verbose = args.verbose

    print("=== Grimoire Loop ===")
    print(f"Model: {args.model}")
    if _inject_verbose:
        print("Verbose: uinject -v enabled")

    # SSH still handles framebuffer capture/SCP; the persistent socket
    # handles injection + idle events with no per-call overhead.
    ssh = SSHSession("remarkable")
    ensure_daemon(ssh, verbose=args.verbose)
    _device = DeviceClient()
    _device.connect()

    # On connect the daemon pushes "ready" plus the current idle file
    # value. Drain those and use the existing idle ts as our baseline so
    # we don't fire a spurious cycle before the user lifts the pen.
    last_idle_ts = None
    deadline = time.time() + 1.0
    while time.time() < deadline:
        try:
            msg = _device.events.get(timeout=0.3)
        except _queue.Empty:
            break
        if msg.get("event") == "idle":
            last_idle_ts = msg.get("ts")

    # The thinking swirl is static — generate it once and SCP it to the
    # device so the daemon can draw/erase it on demand over the socket.
    _scp_thinking_swirl(ssh)

    print("Waiting for pen idle signal...")
    print()

    last_fb_hash = None   # quick hash to skip unchanged frames
    last_inject_y = None  # device Y where we last injected
    last_img = None       # previous frame for pixel-diff new-region detection
    conversation = []     # text history: list of {"role":..., "content":...}

    try:
        while True:
            # Wait for pushed idle event (no polling)
            new_ts = watch_for_idle(_device, last_idle_ts)
            last_idle_ts = new_ts

            t0 = time.time()
            print(f"[{_ts()}] Pen idle detected!")

            # Animate thinking indicator: draw → erase → draw → erase → ...
            # Runs in background while we do capture + API work
            import threading

            def _thinking_animation(stop_event):
                """Draw and erase the thinking curl repeatedly.

                The swirl JSON is already on the device (SCP'd once at
                startup). All injection goes over the persistent socket.
                """
                import math

                drawing = False
                device_json = "/tmp/grimoire_thinking_live.json"
                _vprint(f"  [anim] Animation thread started")

                while not stop_event.is_set():
                    try:
                        action = "erase" if drawing else "draw"
                        _vprint(f"  [anim] Starting {action}...")
                        # Re-check stop right before injecting so we never
                        # start a fresh draw the caller will have to erase.
                        if stop_event.is_set():
                            break
                        if drawing:
                            run_inject(
                                None, "erase", device_json,
                                speed_ms=20, timeout=30,
                            )
                        else:
                            run_inject(
                                None, "draw", device_json,
                                speed_ms=32, timeout=30,
                            )
                        _vprint(f"  [anim] {action} done")
                        drawing = not drawing
                    except Exception as e:
                        _vprint(f"  [anim] ERROR: {e}")
                    # 3s pause after erase; 1s hold after draw
                    wait_time = 3.0 if drawing else 1.0
                    _vprint(f"  [anim] Waiting {wait_time}s...")
                    if stop_event.wait(wait_time):
                        _vprint(f"  [anim] Stop signaled during wait, exiting loop")
                        break
                _vprint(f"  [anim] Thread loop exited (drawing={drawing})")
                # The caller performs the final, guaranteed erase in the
                # main thread (see stop_animation) so it can't be cut short
                # by a join timeout while a draw/erase is still in flight.

            anim_stop = threading.Event()
            anim_thread = threading.Thread(
                target=_thinking_animation, args=(anim_stop,), daemon=True
            )
            anim_thread.start()

            try:
                # Capture
                print(f"  [{_ts()}] Capturing...")
                img = capture_framebuffer(ssh)
                print(f"  [{_ts()}] Captured ({_elapsed(t0)})")

                # Detect new content via pixel diff against last frame.
                # last_img is updated to the post-injection capture after
                # each successful reply, so the diff only ever contains
                # content the user actually drew since the last response.
                new_crop, _ = _find_new_region(img, last_img)
                last_img = img

                if new_crop is None:
                    stop_animation(anim_stop, anim_thread)
                    last_fb_hash = _framebuffer_hash(img)
                    print(f"  [{_ts()}] No new content, skipping.")
                    print()
                    continue

                # Send only the new handwriting crop + text history
                cw, ch = new_crop.size
                print(f"  [{_ts()}] Asking {args.model} ({len(conversation)} history turns, crop={cw}x{ch})...")
                # Save crop for manual inspection
                new_crop.save("/tmp/grimoire_last_crop.png")
                result = ask_model(new_crop, conversation, args.model)
                question = result.get("question")
                answer = result.get("answer")
                print(f"  [{_ts()}] Model: Q={str(question)[:80]!r}  A={str(answer)[:80]!r}")

                if not answer or answer == "null":
                    stop_animation(anim_stop, anim_thread)
                    # Update last_img so we don't keep re-diffing the same
                    # e-ink refresh artifacts on subsequent idle triggers.
                    last_img = img
                    print(f"  [{_ts()}] No actionable content, skipping.")
                    print()
                    continue

                print(f"  [{_ts()}] Q ({_elapsed(t0)}): {str(question)[:60]}")
                print(f"  [{_ts()}] A: {answer[:80]}{'...' if len(answer) > 80 else ''}")

                # Add this exchange to text history for future turns
                conversation.append({"role": "user", "content": str(question)})
                conversation.append({"role": "assistant", "content": answer})

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
                # Stop thinking animation and fully erase the swirl before
                # we draw the reply, so the two never overlap on screen.
                stop_animation(anim_stop, anim_thread)
                render_and_inject(ssh, answer, reply_y=reply_y)
                last_inject_y = reply_y
                print(f"  [{_ts()}] Injected ({_elapsed(t0)})")

                # Capture a fresh baseline AFTER injection so the next
                # diff only sees genuinely new user content. Without this,
                # last_img is the pre-injection frame and every subsequent
                # diff includes our own injected strokes as "new" pixels,
                # producing a crop that mixes our reply with the user's
                # next question and confuses the model.
                print(f"  [{_ts()}] Capturing post-injection baseline...")
                try:
                    time.sleep(0.5)  # let e-ink settle
                    last_img = capture_framebuffer(ssh)
                    print(f"  [{_ts()}] Baseline updated")
                except Exception as e_cap:
                    print(f"  [{_ts()}] Baseline capture failed: {e_cap} (stale frame kept)")

                elapsed = time.time() - t0
                print(f"  [{_ts()}] Done in {elapsed:.1f}s")

            except Exception as e:
                stop_animation(anim_stop, anim_thread)
                print(f"  ERROR: {e}")

            print()

            if args.once:
                break

    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        if _device:
            _device.close()
        ssh.close()


if __name__ == "__main__":
    main()
