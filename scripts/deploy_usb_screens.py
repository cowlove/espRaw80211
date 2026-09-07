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
DEFAULT_ESPTOOL = Path.home() / ".arduino15/packages/esp8266/hardware/esp8266/3.1.2/tools/esptool/esptool.py"


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


def deploy(
    session: UsbSession,
    project: Path,
    dry_run: bool,
    erase_flash: bool,
    esptool: Path,
) -> None:
    upload = (
        f"make -C {project} BOARD=esp32 UPLOAD_PORT={session.port} upload"
    )
    erase = (
        f"python3 {esptool} --chip esp32 --port {session.port} erase_flash"
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
    # The shell's && ensures upload and logging start only after an optional
    # erase and the upload both succeed. Erasing is deliberately opt-in.
    command = f"{erase} && {upload}" if erase_flash else upload
    screen_stuff(session, f"{command} && {monitor}\n", dry_run)


def build_firmware(project: Path, dry_run: bool) -> None:
    command = ["make", "-C", str(project), "BOARD=esp32"]
    if dry_run:
        print("$", " ".join(command))
        return
    print("Building ESP32 firmware once before starting uploads...")
    subprocess.run(command, check=True)
    print("ESP32 firmware build completed; starting concurrent uploads.")


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
    parser.add_argument(
        "--erase-flash",
        action="store_true",
        help="erase each ESP32 flash completely before uploading (destructive)",
    )
    parser.add_argument(
        "--esptool",
        type=Path,
        default=DEFAULT_ESPTOOL,
        help=f"esptool.py used by --erase-flash (default: {DEFAULT_ESPTOOL})",
    )
    args = parser.parse_args()

    sessions = find_sessions()
    if not sessions:
        print("No screen sessions named usb<N> found.", file=sys.stderr)
        return 1

    print("Found:", ", ".join(session.name for session in sessions))
    esptool = args.esptool.expanduser().resolve()
    if args.erase_flash:
        if not esptool.is_file():
            print(f"esptool.py not found: {esptool}", file=sys.stderr)
            return 1
        print(f"Flash erase enabled; using {esptool}")
    # Build before touching any screen session. A failed build leaves the
    # existing serial monitors running and prevents concurrent make processes
    # from fighting over the shared build directory.
    build_firmware(args.project.resolve(), args.dry_run)
    for session in sessions:
        deploy(
            session,
            args.project.resolve(),
            args.dry_run,
            args.erase_flash,
            esptool,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
