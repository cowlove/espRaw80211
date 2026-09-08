#!/usr/bin/env python3
"""Prefix serial lines with host wall-clock and monotonic timestamps."""

from __future__ import annotations

import argparse
from datetime import datetime
import os
import socket
import sys
import time
import uuid
from pathlib import Path


def reconnect_log(board, port, logfile, retry=1.0):
    """Append across reconnects. Socket paths survive ttyUSB renumbering.

    A new session prevents analyzers joining partial exchanges across a gap.
    Open the log for each emission so rotation/recreation is also tolerated.
    """
    import serial
    host = socket.gethostname()
    while True:
        session = f"{host}-{uuid.uuid4().hex}"
        def emit(payload):
            wall = datetime.now().astimezone().isoformat(timespec='microseconds')
            line = (f'{wall} host_mono_ns={time.monotonic_ns()} board={board} '
                    f'port={port} session={session} | {payload}\n')
            with Path(logfile).open('a', encoding='utf-8') as output:
                output.write(line)
            print(line, end='', flush=True)
        try:
            source = serial.Serial(port=None, baudrate=115200, timeout=1, exclusive=True)
            try:
                # Do not deliberately pulse the ESP reset/boot control lines.
                source.dtr = False
                source.rts = False
                source.port = port
                source.open()
                emit(f'logger-session host={host} pid={os.getpid()} reconnect=1')
                pending = b''
                while True:
                    chunk = source.read(4096)
                    pending += chunk
                    while b'\n' in pending:
                        raw, pending = pending.split(b'\n', 1)
                        emit(raw.decode('utf-8', errors='replace').rstrip('\r'))
                    if len(pending) > 65536:
                        emit('logger-partial ' + pending.decode('utf-8', errors='replace'))
                        pending = b''
            finally:
                source.close()
        except (serial.SerialException, OSError) as error:
            # Never carry an unfinished serial line into the next connection.
            print(f'logger disconnected/retrying {port}: {error}', file=sys.stderr, flush=True)
            time.sleep(retry)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--board", required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument('--logfile', help='own serial port and reconnect; append to this log')
    args = parser.parse_args()
    if args.logfile:
        reconnect_log(args.board, args.port, args.logfile)
        return 0

    host = socket.gethostname()
    session = f"{host}-{uuid.uuid4().hex}"
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
