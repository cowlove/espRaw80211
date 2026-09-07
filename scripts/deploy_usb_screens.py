#!/usr/bin/env python3
"""Upload espRaw80211 to every screen session named usb<N> and restart logging.

For each matching screen session this sends Ctrl-C to the current ``make cat``
job, then runs the upload and, only after a successful upload, starts an
append-mode serial logger in the same screen session.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


SESSION_RE = re.compile(r"^(?P<id>\S+\.usb(?P<index>\d+))\s+\(")
LEGACY_ESPTOOL = Path.home() / ".arduino15/packages/esp8266/hardware/esp8266/3.1.2/tools/esptool/esptool.py"


def tool_path() -> str:
    """Return a PATH that works from non-interactive SSH/screen shells."""
    private_bin = Path.home() / "bin"
    return f"{private_bin}:{os.environ.get('PATH', '')}"


def find_esptool(explicit: Path | None) -> Path:
    """Find Arduino's installed ESP32 esptool, with legacy fallbacks."""
    if explicit is not None:
        return explicit.expanduser().resolve()

    arduino = Path.home() / ".arduino15/packages"
    candidates = sorted(arduino.glob("esp32/tools/esptool_py/*/esptool"), reverse=True)
    candidates += sorted(arduino.glob("esp32/tools/esptool_py/*/esptool.py"), reverse=True)
    candidates += [LEGACY_ESPTOOL]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    for name in ("esptool", "esptool.py"):
        found = shutil.which(name, path=tool_path())
        if found:
            return Path(found).resolve()
    return candidates[0] if candidates else arduino / "esp32/tools/esptool_py/esptool"


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
    prefix = f"export PATH={shlex.quote(tool_path())}; "
    upload = prefix + (
        f"make -C {shlex.quote(str(project))} BOARD=esp32 UPLOAD_PORT={session.port} upload"
    )
    erase = (
        f"{shlex.quote(str(esptool))} --chip esp32 --port {session.port} erase_flash"
    )
    monitor = (
        f"make -C {shlex.quote(str(project))} BOARD=esp32 UPLOAD_PORT={session.port} cat "
        f"| tee -a {shlex.quote(str(project / session.logfile))}"
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
    env = os.environ.copy()
    env["PATH"] = tool_path()
    subprocess.run(command, check=True, env=env)
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
        default=None,
        help="esptool executable override; otherwise discover Arduino ESP32 esptool",
    )
    args = parser.parse_args()

    sessions = find_sessions()
    if not sessions:
        print("No screen sessions named usb<N> found.", file=sys.stderr)
        return 1

    print("Found:", ", ".join(session.name for session in sessions))
    esptool = find_esptool(args.esptool)
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
