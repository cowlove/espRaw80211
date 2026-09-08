#!/usr/bin/env python3
"""Upload espRaw80211 to USB boards and own their screen logger lifecycle.

By default, boards are inferred from ``/dev/ttyUSB<N>``. Existing project
screen sessions are terminated and recreated, so deployment does not depend
on a healthy foreground shell or a manually started logger.
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


def esptool_command(esptool: Path) -> str:
    """Build a shell command for either Arduino's binary or Python script."""
    command = ["python3", str(esptool)] if esptool.suffix == ".py" else [str(esptool)]
    return " ".join(shlex.quote(part) for part in command)


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


def screen_quit(session: UsbSession, dry_run: bool) -> None:
    command = ["screen", "-S", session.screen_id, "-X", "quit"]
    if dry_run:
        print("$", " ".join(command))
        return
    subprocess.run(command, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def start_screen(session: UsbSession, command: str, dry_run: bool) -> None:
    screen_command = ["screen", "-dmS", session.screen_id, "bash", "-lc", command]
    if dry_run:
        print("$", " ".join(shlex.quote(part) for part in screen_command))
        return
    subprocess.run(screen_command, check=True)


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
        f"{esptool_command(esptool)} --chip esp32 --port {session.port} erase_flash"
    )
    monitor = (
        f"make -C {shlex.quote(str(project))} BOARD=esp32 UPLOAD_PORT={session.port} cat "
        f"| python3 {shlex.quote(str(project / 'scripts/timestamp_serial.py'))} "
        f"--board {shlex.quote(session.name)} --port {shlex.quote(session.port)} "
        f"| tee -a {shlex.quote(str(project / session.logfile))}"
    )
    print(f"{session.name}: {session.port} -> {session.logfile}")
    command = f"{erase} && {upload}" if erase_flash else upload
    # A fresh shell owns the whole lifecycle. The logger starts only after a
    # successful erase/upload and remains in the new screen session.
    start_screen(session, f"{command} && {monitor}", dry_run)


def discover_indices() -> list[int]:
    return sorted(
        int(match.group(1))
        for path in Path("/dev").glob("ttyUSB*")
        if (match := re.fullmatch(r"ttyUSB(\d+)", path.name))
    )


def parse_indices(value: str) -> list[int]:
    try:
        indices = sorted({int(item.strip()) for item in value.split(",") if item.strip()})
    except ValueError as exc:
        raise argparse.ArgumentTypeError("board indices must be comma-separated integers") from exc
    if not indices or any(index < 0 for index in indices):
        raise argparse.ArgumentTypeError("board indices must contain at least one nonnegative integer")
    return indices


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
    parser.add_argument(
        "--boards",
        type=parse_indices,
        default=None,
        metavar="N[,N...]",
        help="USB board indices; default is every detected /dev/ttyUSBN",
    )
    parser.add_argument(
        "--keep-screens",
        action="store_true",
        help="reuse matching screen sessions instead of terminating/recreating them",
    )
    args = parser.parse_args()

    indices = args.boards if args.boards is not None else discover_indices()
    if not indices:
        print("No /dev/ttyUSBN boards found; use --boards N[,N...] to specify them.", file=sys.stderr)
        return 1
    existing = find_sessions()
    sessions = [
        next((session for session in existing if session.index == index),
             UsbSession(f"esp.usb{index}", index))
        for index in indices
    ]

    print("Boards:", ", ".join(f"{session.name}={session.port}" for session in sessions))
    esptool = find_esptool(args.esptool)
    if args.erase_flash:
        if not esptool.is_file():
            print(f"esptool.py not found: {esptool}", file=sys.stderr)
            return 1
        print(f"Flash erase enabled; using {esptool}")
    # Build before touching any screen session. A failed build leaves the
    # existing serial monitors running.
    build_firmware(args.project.resolve(), args.dry_run)
    if not args.keep_screens:
        for stale in (session for session in existing if session.index in indices):
            print(f"Stopping {stale.screen_id}")
            screen_quit(stale, args.dry_run)
        if not args.dry_run:
            time.sleep(0.25)
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
