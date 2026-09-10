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


MAX_SERIAL_LINE_BYTES = 4096


def clean_serial_line(raw):
    """Return printable ASCII firmware text, or None for boot/baud noise."""
    if len(raw) > MAX_SERIAL_LINE_BYTES:
        return None
    try:
        text = raw.decode('ascii').rstrip('\r')
    except UnicodeDecodeError:
        return None
    if any(character not in '\t' and not 32 <= ord(character) <= 126
           for character in text):
        return None
    return text


def default_reset_request_file(board):
    return Path('/tmp') / f'espRaw80211-{board}.reset-request'


def requested_reset(path):
    """Return the request token, leaving the flag in place until reset succeeds."""
    try:
        return path.read_text(encoding='utf-8').strip() or 'requested'
    except FileNotFoundError:
        return None


def pulse_hard_reset(source):
    """Pulse ESP32 EN while keeping IO0 high for normal flash boot."""
    source.setDTR(False)  # IO0 high
    source.setRTS(True)   # EN low
    time.sleep(0.1)
    source.setRTS(False)  # EN high


def reconnect_log(board, port, logfile, retry=1.0, reset_request_file=None):
    """Append across reconnects. Socket paths survive ttyUSB renumbering.

    A new session prevents analyzers joining partial exchanges across a gap.
    Open the log for each emission so rotation/recreation is also tolerated.
    """
    import serial
    host = socket.gethostname()
    reset_request_file = Path(reset_request_file or default_reset_request_file(board))
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
                dropped_bytes = dropped_lines = 0
                while True:
                    token = requested_reset(reset_request_file)
                    if token is not None:
                        emit(f'logger-reset-request id={token}')
                        pulse_hard_reset(source)
                        reset_request_file.unlink(missing_ok=True)
                        emit(f'logger-reset-pulsed id={token}')
                    chunk = source.read(4096)
                    pending += chunk
                    while b'\n' in pending:
                        raw, pending = pending.split(b'\n', 1)
                        text = clean_serial_line(raw)
                        if text is None:
                            dropped_bytes += len(raw) + 1
                            dropped_lines += 1
                            continue
                        if dropped_bytes:
                            emit(f'logger-noise bytes={dropped_bytes} lines={dropped_lines}')
                            dropped_bytes = dropped_lines = 0
                        emit(text)
                    if len(pending) > 65536:
                        dropped_bytes += len(pending)
                        dropped_lines += 1
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
    parser.add_argument('--reset-request-file',
                        help='flag file whose contents request one ESP32 EN reset')
    args = parser.parse_args()
    if args.logfile:
        reconnect_log(args.board, args.port, args.logfile,
                      reset_request_file=args.reset_request_file)
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
