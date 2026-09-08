#!/usr/bin/env python3
"""Prefix serial lines with host wall-clock and monotonic timestamps."""

from __future__ import annotations

import argparse
from datetime import datetime
import os
import socket
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--board", required=True)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()

    host = socket.gethostname()
    session = f"{host}-{os.getpid()}"
    def emit(payload: str) -> None:
        wall = datetime.now().astimezone().isoformat(timespec="microseconds")
        mono = time.monotonic_ns()
        sys.stdout.write(
            f"{wall} host_mono_ns={mono} board={args.board} port={args.port} "
            f"session={session} | {payload}"
        )
        sys.stdout.flush()

    emit(f"logger-session host={host} pid={os.getpid()}\n")
    for raw in sys.stdin.buffer:
        emit(raw.decode("utf-8", errors="replace"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
