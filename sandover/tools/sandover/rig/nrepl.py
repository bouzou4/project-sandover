"""Minimal client for the goalc nREPL server.

Wire format (common/repl/nrepl/ReplServer.{h,cpp}): an 8-byte little-endian header
``{u32 length, u32 type}`` followed by ``length`` bytes of GOAL source. ``type`` is
PING=0, EVAL=10, SHUTDOWN=20. On connect the server writes a greeting; evaluation
results are never sent back, so callers observe effects through the game's stdout.

Usage (from sandover/tools):
    uv run python -m sandover.rig.nrepl "(lt)"
    uv run python -m sandover.rig.nrepl --file sandover/rig/rig.gc
"""

import argparse
import socket
import struct
import sys
import time

EVAL = 10
DEFAULT_PORT = 8181


def connect(host: str = "127.0.0.1", port: int = DEFAULT_PORT, timeout: float = 30.0) -> socket.socket:
    """Connect, retrying until ``timeout``, and consume the server greeting."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            sock = socket.create_connection((host, port), timeout=5)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.5)
    try:
        sock.recv(256)
    except TimeoutError:
        pass
    return sock


def send(text: str, sock: socket.socket | None = None, port: int = DEFAULT_PORT) -> None:
    """Send one EVAL message. Opens (and closes) a connection if ``sock`` is None."""
    body = text.encode("utf-8")
    own = sock is None
    s = sock or connect(port=port)
    try:
        s.sendall(struct.pack("<II", len(body), EVAL) + body)
    finally:
        if own:
            # server polls every 50 ms; give it time to read before we hang up
            time.sleep(0.3)
            s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="Send GOAL forms to goalc's nREPL.")
    ap.add_argument("form", nargs="?", help="GOAL text to evaluate")
    ap.add_argument("--file", help="read GOAL text from this file instead")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = ap.parse_args()
    text = open(args.file, encoding="utf-8").read() if args.file else args.form
    if not text:
        ap.error("give a form or --file")
    send(text, port=args.port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
