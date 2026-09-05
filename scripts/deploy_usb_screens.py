#!/usr/bin/env python3
"""Upload espRaw80211 to every screen session named usb<N> and restart logging.

For each matching screen session this sends Ctrl-C to the current ``make cat``
job, then runs the upload and, only after a successful upload, starts an
append-mode serial logger in the same screen session.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


SESSION_RE = re.compile(r"^(?P<id>\S+\.usb(?P<index>\d+))\s+\(")


@dataclass(frozen=True)
class UsbSession:
    screen_id: str
    index: int

    @property
    def name(self) -> str:
        return f"usb{self.index}"

    @property
    def port(self) -> str:
        return f"/dev/ttyUSB{self.index}"

    @property
    def logfile(self) -> str:
        return f"cat.usb{self.index}.out"


def find_sessions() -> list[UsbSession]:
    result = subprocess.run(
        ["screen", "-ls"], capture_output=True, text=True, check=False
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.strip() or "screen -ls failed")

    sessions: list[UsbSession] = []
    for line in result.stdout.splitlines():
        match = SESSION_RE.search(line.strip())
        if match:
            sessions.append(UsbSession(match.group("id"), int(match.group("index"))))
    return sorted(sessions, key=lambda session: session.index)


def screen_stuff(session: UsbSession, text: str, dry_run: bool) -> None:
    command = ["screen", "-S", session.screen_id, "-X", "stuff", text]
    if dry_run:
        print("$", " ".join(command))
        return
    subprocess.run(command, check=True)


def deploy(session: UsbSession, project: Path, dry_run: bool) -> None:
    upload = (
        f"make -C {project} BOARD=esp32 UPLOAD_PORT={session.port} upload"
    )
    monitor = (
        f"make -C {project} BOARD=esp32 UPLOAD_PORT={session.port} cat "
        f"| tee -a {project / session.logfile}"
    )
    # Ctrl-C stops the foreground make/cat pipeline without destroying the
    # screen session or its shell. Send it separately from the command: some
    # terminals consume the first command character when both arrive in one
    # screen "stuff" payload.
    print(f"{session.name}: {session.port} -> {session.logfile}")
    screen_stuff(session, "\003", dry_run)
    if dry_run:
        print("  (wait 0.25s)")
    else:
        time.sleep(0.25)
    # The shell's && ensures logging starts only after esptool reports a
    # successful upload.
    screen_stuff(session, f"{upload} && {monitor}\n", dry_run)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="espRaw80211 project directory (default: script's project)",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="print screen commands without sending them"
    )
    args = parser.parse_args()

    sessions = find_sessions()
    if not sessions:
        print("No screen sessions named usb<N> found.", file=sys.stderr)
        return 1

    print("Found:", ", ".join(session.name for session in sessions))
    for session in sessions:
        deploy(session, args.project.resolve(), args.dry_run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
