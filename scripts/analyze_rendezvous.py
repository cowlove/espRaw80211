#!/usr/bin/env python3
"""Print a compact rendezvous timing dashboard from local and Miner6 logs."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

STAMP = re.compile(r"^(\d+\.\d+)")
START = "ESP-NOW exchange phase started"
END = "deep sleep "
GOSSIP = re.compile(r"gossip .*? exchange (healthy|incomplete).*? home ([0-9a-f]+) listeners (\d+)")
CONSENSUS = re.compile(r"test consensus (\d+)/10")


@dataclass
class Cycle:
    start: float
    end: float
    home: str = "-"
    listeners: str = "-"
    healthy: bool = False
    consensus: str = "-"
    reset: str = ""

    @property
    def seconds(self) -> float:
        return self.end - self.start


def clean(data: bytes) -> list[str]:
    return [line.replace(b"\x00", b"").decode("utf-8", "replace") for line in data.splitlines()]


def parse(data: bytes, limit: int) -> list[Cycle]:
    cycles: list[Cycle] = []
    current: Cycle | None = None
    session_start = 0
    for line in clean(data):
        # Logger sessions delimit capture continuity, not device resets.
        # Never let a partial cycle or its trailing counters span sessions.
        if ' | ' in line and ' host_mono_ns=' in line:
            line = line.split(' | ', 1)[1]
        if line.startswith('logger-session '):
            current = None
            session_start = len(cycles)
            continue
        m = STAMP.match(line)
        if not m:
            continue
        t = float(m.group(1))
        if START in line:
            if current is not None:
                current = None
            current = Cycle(t, t)
        elif current is not None and END in line:
            current.end = t
            if current.end >= current.start:
                cycles.append(current)
            current = None
        elif "TEST RESET EXECUTED" in line and len(cycles) > session_start:
            cycles[-1].reset = "EXECUTED"
        elif "TEST RESET COMMITTED" in line and len(cycles) > session_start:
            cycles[-1].reset = "commit"
        elif current is not None:
            m = GOSSIP.search(line)
            if m:
                exchange, current.home, current.listeners = m.groups()
                current.healthy = exchange == "healthy"
            m = CONSENSUS.search(line)
            if m:
                current.consensus = m.group(1) + "/10"
        elif "test consensus" in line and len(cycles) > session_start:
            m = CONSENSUS.search(line)
            if m:
                cycles[-1].consensus = m.group(1) + "/10"
    return cycles[-limit:]


def read_remote(host: str, path: str, tail_bytes: int) -> bytes:
    # tail avoids transferring multi-day serial logs while preserving recent cycles.
    cmd = ["ssh", host, "tail", "-c", str(tail_bytes), "--", path]
    try:
        return subprocess.run(cmd, check=True, capture_output=True).stdout
    except subprocess.CalledProcessError as exc:
        print(f"remote read failed ({host}:{path}): {exc.stderr.decode(errors='replace').strip()}", file=sys.stderr)
        return b""


def dashboard(name: str, data: bytes, limit: int) -> None:
    cycles = parse(data, 1 if limit == 0 else limit)
    if not cycles:
        print(f"{name:<12} no complete rendezvous cycles")
        return
    if limit == 0:
        latest = cycles[-1]
        exchange = "healthy" if latest.healthy else "incomplete"
        consensus = latest.consensus if latest.consensus != "-" else "-"
        reset = f" reset={latest.reset}" if latest.reset else ""
        print(
            f"{name:<12} latest home={latest.home} listeners={latest.listeners} "
            f"exchange={exchange} consensus={consensus}{reset}"
        )
        return
    values = [c.seconds for c in cycles]
    avg = sum(values) / len(values)
    print(f"{name:<12} n={len(values):2d}  last={values[-1]:5.2f}s  avg={avg:5.2f}s  min={min(values):5.2f}s  max={max(values):5.2f}s  home={cycles[-1].home}  listeners={cycles[-1].listeners}")
    for i, c in enumerate(cycles, 1):
        extra = f" consensus={c.consensus}" if c.consensus != "-" else ""
        if c.reset:
            extra += f" reset={c.reset}"
        print(f"  {i:2d}  {c.seconds:5.2f}s  home={c.home} listeners={c.listeners}{extra}")


def convergence_dashboard(name: str, data: bytes) -> tuple[list[int], list[float]]:
    """Report cycles from one executed flush to the next 10/10 commitment.

    This deliberately counts complete wake/sleep cycles, which remains meaningful
    even though the MCU's millisecond clock resets after every deep-sleep reboot.
    """
    cycles = parse(data, 10_000)
    episodes: list[int] = []
    active_seconds: list[float] = []
    started = False
    count = 0
    elapsed = 0.0
    for c in cycles:
        if c.reset == "EXECUTED":
            started, count, elapsed = True, 0, 0.0
            continue
        if not started:
            continue
        count += 1
        elapsed += c.seconds
        if c.consensus == "10/10":
            episodes.append(count)
            active_seconds.append(elapsed)
            started = False
    if episodes:
        # The endpoint cycle is the tenth consecutive consensus cycle.  Make
        # the fixed 10-cycle streak visible instead of hiding it in `cycles`.
        pre = [max(0, n - 10) for n in episodes]
        print(f"{name:<12} episodes={len(episodes):2d}  since-reset={','.join(map(str, episodes))}  pre-streak={','.join(map(str, pre))}  streak=10  avg={sum(episodes)/len(episodes):.1f}")
    else:
        print(f"{name:<12} no complete post-reset convergence episode")
    return episodes, active_seconds


def current_dashboard(name: str, data: bytes) -> None:
    """Show the currently active reset-to-consensus attempt, if any."""
    cycles = parse(data, 10_000)
    resets = [i for i, c in enumerate(cycles) if c.reset == "EXECUTED"]
    if not resets:
        print(f"{name:<12} current=none")
        return
    start = resets[-1]
    active = cycles[start + 1:]
    if not active:
        print(f"{name:<12} current=started  cycles=0")
        return
    latest = active[-1]
    status = "counting"
    if latest.reset == "commit":
        status = "committed/delaying"
    elif latest.consensus == "10/10":
        status = "committed/delaying"
    print(f"{name:<12} current={status:<18} cycles={len(active):2d}  consensus={latest.consensus:<5}  home={latest.home} listeners={latest.listeners}")


def rendezvous_dashboard(name: str, data: bytes) -> bool:
    """Measure observed rendezvous evidence independently of reset decisions."""
    cycles = parse(data, 10_000)
    qualified = [c for c in cycles if c.healthy and c.home != "-" and
                 c.listeners.isdigit() and int(c.listeners) >= 6]
    if not cycles:
        print(f"{name:<12} rendezvous=none")
        return False
    latest = cycles[-1]
    state = "QUALIFIED" if qualified and qualified[-1] is latest else "not-qualified"
    print(f"{name:<12} rendezvous={state:<12} qualified={len(qualified):3d}  latest-home={latest.home}  latest-listeners={latest.listeners}  latest-exchange={'healthy' if latest.healthy else 'incomplete'}")
    return state == "QUALIFIED"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--recent", type=int, default=10, help="complete cycles per board; 0 shows only latest state (default: 10)")
    ap.add_argument("--log-dir", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--remote-host", default="miner6.local")
    ap.add_argument("--remote-dir", default="~/src/espRaw80211")
    ap.add_argument("--tail-bytes", type=int, default=8_000_000)
    ap.add_argument("--convergence", action="store_true", help="show cycles from reset execution to next 10/10 consensus")
    args = ap.parse_args()
    if args.convergence:
        print("Convergence dashboard | KPI = complete wake/sleep cycles after flush until 10/10 consensus")
        datasets = []
        for i in range(4):
            path = args.log_dir / f"cat.usb{i}.out"
            datasets.append((f"local usb{i}", path.read_bytes() if path.exists() else b""))
        for i in range(2):
            datasets.append((f"miner6 usb{i}", read_remote(args.remote_host, f"{args.remote_dir}/cat.usb{i}.out", args.tail_bytes)))
        all_cycles = []
        for name, data in datasets:
            all_cycles.extend(convergence_dashboard(name, data)[0])
        if all_cycles:
            print(f"ALL BOARDS   episodes={len(all_cycles):2d}  typical={sum(all_cycles)/len(all_cycles):.1f} cycles  range={min(all_cycles)}–{max(all_cycles)}")
        print("Current attempts | cycles since latest TEST RESET EXECUTED")
        for name, data in datasets:
            current_dashboard(name, data)
        print("Observed rendezvous | independent of TEST RESET decisions")
        observed = [rendezvous_dashboard(name, data) for name, data in datasets]
        print(f"GLOBAL        {'ALL SIX QUALIFIED' if all(observed) else 'not all six currently qualified'}")
    else:
        print(f"Rendezvous dashboard | last {args.recent} complete cycles | KPI = exchange-start → deep-sleep")
        for i in range(4):
            path = args.log_dir / f"cat.usb{i}.out"
            dashboard(f"local usb{i}", path.read_bytes() if path.exists() else b"", args.recent)
        for i in range(2):
            path = f"{args.remote_dir}/cat.usb{i}.out"
            dashboard(f"miner6 usb{i}", read_remote(args.remote_host, path, args.tail_bytes), args.recent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
