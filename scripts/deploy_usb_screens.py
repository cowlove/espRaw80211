#!/usr/bin/env python3
"""Upload espRaw80211 to USB boards and own their screen logger lifecycle.

By default, boards are inferred from ``/dev/ttyUSB<N>``. Existing project
screen sessions are terminated and recreated, so deployment does not depend
on a healthy foreground shell or a manually started logger.
"""

from __future__ import annotations

import argparse
import concurrent.futures
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
        if candidate.is_file() and (candidate.suffix == '.py' or os.access(candidate, os.X_OK)):
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
    subprocess.run(command, check=True)


def start_screen(session: UsbSession, command: str, dry_run: bool) -> None:
    screen_command = ["screen", "-dmS", f"esp.{session.name}", "bash", "-c", command]
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
    upload_template: list[str] | None = None,
) -> None:
    env = os.environ.copy()
    env['PATH'] = tool_path()
    commands = []
    if erase_flash:
        commands.append(shlex.split(esptool_command(esptool)) +
                        ['--chip', 'esp32', '--port', session.port, 'erase_flash'])
    if upload_template is None:
        commands.append(['make', '-C', str(project), 'BOARD=esp32',
                         f'UPLOAD_PORT={session.port}', 'upload-only'])
    else:
        commands.append([session.port if part == '__UPLOAD_PORT__' else part
                         for part in upload_template])
    # Flash synchronously: detached-screen success is not upload success.
    for command in commands:
        print('$', shlex.join(command))
        if not dry_run:
            subprocess.run(command, check=True, env=env)
    start_logger(session, project, dry_run)


def logger_port(port: str) -> str:
    # Many cheap adapters share serial IDs. Bind to the physical USB socket.
    return next((str(path) for path in sorted(Path('/dev/serial/by-path').glob('*'))
                 if path.resolve() == Path(port).resolve()), port)


def start_logger(session: UsbSession, project: Path, dry_run: bool) -> None:
    monitor = shlex.join(['python3', str(project / 'scripts/timestamp_serial.py'),
                          '--board', session.name, '--port', logger_port(session.port),
                          '--logfile', str(project / session.logfile)])
    print(f"{session.name}: {session.port} -> {session.logfile}")
    start_screen(session, f"export PATH={shlex.quote(tool_path())}; set -o pipefail; {monitor}", dry_run)
    if not dry_run:
        time.sleep(0.5)
        if not any(s.index == session.index for s in find_sessions()):
            raise RuntimeError(f'{session.name}: logger screen exited')
        print(f'{session.name}: logger screen alive (serial reception not yet verified)')


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
    print("ESP32 firmware build completed; starting uploads.")


def upload_command_template(project: Path) -> list[str]:
    """Resolve Make's upload recipe once, before parallel workers start.

    makeEspArduino includes UPLOAD_PORT in its build-state bookkeeping, so
    concurrent ``make upload`` processes can invalidate each other's shared
    artifacts.  Workers therefore execute the already-resolved esptool command
    directly and never enter Make concurrently.
    """
    probe_port = '/dev/ttyUSB0'
    result = subprocess.run(
        ['make', '-n', '-C', str(project), 'BOARD=esp32',
         f'UPLOAD_PORT={probe_port}', 'upload-only'],
        check=True, capture_output=True, text=True,
        env={**os.environ, 'PATH': tool_path()},
    )
    lines = [line.strip() for line in result.stdout.splitlines()]
    commands = [shlex.split(line) for line in lines
                if ' write_flash ' in f' {line} ']
    if len(commands) != 1 or probe_port not in commands[0]:
        raise RuntimeError('could not resolve a unique esptool upload command')
    return ['__UPLOAD_PORT__' if part == probe_port else part
            for part in commands[0]]


def deploy_parallel(
    sessions: list[UsbSession],
    project: Path,
    dry_run: bool,
    erase_flash: bool,
    esptool: Path,
    jobs: int,
) -> None:
    """Upload independently in parallel, then report every board failure."""
    if dry_run or jobs == 1:
        for session in sessions:
            deploy(session, project, dry_run, erase_flash, esptool)
        return

    upload_template = upload_command_template(project)
    failures: list[tuple[UsbSession, Exception]] = []
    print(f"Uploading {len(sessions)} boards with {jobs} parallel workers...")
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(deploy, session, project, False, erase_flash,
                            esptool, upload_template): session
            for session in sessions
        }
        for future in concurrent.futures.as_completed(futures):
            session = futures[future]
            try:
                future.result()
            except Exception as exc:
                failures.append((session, exc))
                print(f"{session.name}: deployment failed: {exc}", file=sys.stderr)
    if failures:
        names = ", ".join(session.name for session, _ in failures)
        raise RuntimeError(f"Deployment failed for: {names}")


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
        "--jobs",
        type=int,
        default=0,
        metavar="N",
        help="parallel upload workers; default is one per selected board, use 1 for sequential",
    )
    parser.add_argument('--log-only', action='store_true', help='restart loggers without building or flashing')
    args = parser.parse_args()
    if args.log_only and args.erase_flash:
        parser.error('--log-only cannot be combined with --erase-flash')
    if args.jobs < 0:
        parser.error('--jobs must be zero or a positive integer')

    indices = args.boards if args.boards is not None else discover_indices()
    if not indices:
        print("No /dev/ttyUSBN boards found; use --boards N[,N...] to specify them.", file=sys.stderr)
        return 1
    existing = find_sessions()
    sessions = [UsbSession(f"esp.usb{index}", index) for index in indices]
    if not args.dry_run:
        for session in sessions:
            if not Path(session.port).exists():
                raise RuntimeError(f'Missing serial port: {session.port}')

    print("Boards:", ", ".join(f"{session.name}={session.port}" for session in sessions))
    esptool = find_esptool(args.esptool)
    if args.erase_flash:
        if not esptool.is_file():
            print(f"esptool.py not found: {esptool}", file=sys.stderr)
            return 1
        print(f"Flash erase enabled; using {esptool}")
    # Build before touching any screen session. A failed build leaves the
    # existing serial monitors running.
    if not args.log_only:
        build_firmware(args.project.resolve(), args.dry_run)
    for session in sessions:
        for stale in (s for s in existing if s.index == session.index):
            print(f"Stopping {stale.screen_id}")
            screen_quit(stale, args.dry_run)
        if not args.dry_run:
            deadline = time.monotonic() + 5
            while any(s.index == session.index for s in find_sessions()):
                if time.monotonic() >= deadline:
                    raise RuntimeError(f'{session.name}: screen did not stop')
                time.sleep(0.1)
        if args.log_only:
            start_logger(session, args.project.resolve(), args.dry_run)
    if not args.log_only:
        jobs = min(args.jobs or len(sessions), len(sessions))
        deploy_parallel(sessions, args.project.resolve(), args.dry_run,
                        args.erase_flash, esptool, jobs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
