#!/usr/bin/env python3
"""Print a compact rendezvous timing dashboard from local and Miner6 logs."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import json
import shlex
import time
from datetime import datetime
from dataclasses import dataclass, field
from pathlib import Path
import rendezvous_evidence as evidence
from test_swarm_config import swarm_board_count

STAMP = re.compile(r"^(\d+\.\d+)")
START = "ESP-NOW exchange phase started"
END = "deep sleep "
GOSSIP = re.compile(r"gossip .*? exchange (healthy|incomplete).*? home ([0-9a-f]+) listeners (\d+)")
CONSENSUS = re.compile(r"test consensus (\d+)/10")
DEFAULT_TAIL_BYTES = 1_000_000


@dataclass
class Cycle:
    start: float
    end: float
    home: str = "-"
    listeners: str = "-"
    healthy: bool = False
    consensus: str = "-"
    reset: str = ""
    capture: int = field(default=0, compare=False)

    @property
    def seconds(self) -> float:
        return self.end - self.start


def clean(data: bytes) -> list[str]:
    return [line.replace(b"\x00", b"").decode("utf-8", "replace") for line in data.splitlines()]


def parse(data: bytes, limit: int) -> list[Cycle]:
    cycles: list[Cycle] = []
    current: Cycle | None = None
    session_start = 0
    capture = 0
    for line in clean(data):
        # Logger sessions delimit capture continuity, not device resets.
        # Never let a partial cycle or its trailing counters span sessions.
        if ' | ' in line and ' host_mono_ns=' in line:
            line = line.split(' | ', 1)[1]
        if line.startswith('logger-session '):
            current = None
            session_start = len(cycles)
            capture += 1
            continue
        m = STAMP.match(line)
        if not m:
            continue
        t = float(m.group(1))
        if START in line:
            if current is not None:
                current = None
            current = Cycle(t, t, capture=capture)
        elif current is not None and (END in line or 'exchange complete interval ' in line):
            current.end = t
            if current.end >= current.start:
                cycles.append(current)
            current = None
        elif "TEST RESET EXECUTED" in line and current is not None:
            current.reset = 'EXECUTED'
        elif "TEST RESET COMMITTED" in line and current is not None:
            current.reset = 'commit'
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
    quoted = ('"$HOME"/' + shlex.quote(path[2:])) if path.startswith('~/') else shlex.quote(path)
    cmd = ["ssh", "-x", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
           host, f"tail -c {tail_bytes} -- {quoted}"]
    try:
        return subprocess.run(cmd, check=True, capture_output=True, timeout=30).stdout
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"remote read failed ({host}:{path}): {(exc.stderr or b'').decode(errors='replace').strip()}", file=sys.stderr)
        return b""


def read_local_tail(path: Path, tail_bytes: int) -> bytes:
    """Read at most the requested suffix without loading the whole log."""
    size = path.stat().st_size
    with path.open('rb') as stream:
        stream.seek(max(0, size - tail_bytes))
        return stream.read(tail_bytes)


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
    capture = None
    for c in cycles:
        if c.capture != capture:
            started, count, elapsed = False, 0, 0.0
            capture = c.capture
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
    if cycles:
        cycles = [c for c in cycles if c.capture == cycles[-1].capture]
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


def human_age(seconds: float) -> str:
    seconds = max(0, int(seconds))
    days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    if days:
        return f"{days}d{hours}h"
    if hours:
        return f"{hours}h{minutes}m"
    if minutes:
        return f"{minutes}m{seconds}s"
    return f"{seconds}s"


def global_home_convergences(datasets, required: int, max_skew: float = 90):
    """Return transitions where every logged board's recent home agrees.

    This is an observational test oracle. It does not use listener counts and
    cannot account for unlogged boards, even when the artificial swarm size is
    larger than the number of serial logs.
    """
    observations = []
    board_names = {name for name, _ in datasets}
    for name, data in datasets:
        for cycle in evidence.parse_evidence(data)[0]:
            if cycle.wall is not None and cycle.home:
                observations.append((cycle.wall, name, cycle.home))
    observations.sort()
    latest = {}
    active_bssid = None
    events = []
    for wall, name, home in observations:
        latest[name] = (wall, home)
        homes = {value[1] for value in latest.values()}
        times = [value[0] for value in latest.values()]
        converged = (len(board_names) >= required and
                     len(latest) == len(board_names) and len(homes) == 1 and
                     max(times) - min(times) <= max_skew)
        if converged:
            bssid = next(iter(homes))
            if active_bssid != bssid:
                events.append({'host_time': wall, 'bssid': bssid})
            active_bssid = bssid
        else:
            active_bssid = None
    return events


def rendezvous_dashboard(name: str, data: bytes) -> bool:
    """Apply an ARTIFICIAL test oracle, not a discoverable global membership."""
    required = swarm_board_count()
    cycles = parse(data, 10_000)
    qualified = [c for c in cycles if c.healthy and c.home != "-" and
                 c.listeners.isdigit() and int(c.listeners) >= required]
    if not cycles:
        print(f"{name:<12} rendezvous=none")
        return False
    latest = cycles[-1]
    state = "QUALIFIED" if qualified and qualified[-1] is latest else "not-qualified"
    print(f"{name:<12} rendezvous={state:<12} artificial-test-board-count={required} qualified={len(qualified):3d}  latest-home={latest.home}  latest-listeners={latest.listeners}  latest-exchange={'healthy' if latest.healthy else 'incomplete'}")
    return state == "QUALIFIED"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--recent", type=int, default=10, help="complete cycles per board; 0 shows only latest state (default: 10)")
    ap.add_argument("--log-dir", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--remote-host", default="miner6.local")
    ap.add_argument("--remote-dir", default="~/src/espRaw80211")
    ap.add_argument("--tail-bytes", type=int, default=DEFAULT_TAIL_BYTES,
                    help="maximum suffix read per log (default: 1000000)")
    ap.add_argument("--convergence", action="store_true", help="show cycles from reset execution to next 10/10 consensus")
    ap.add_argument("--session", default="latest", help="latest (default), all, or an exact logger session ID")
    ap.add_argument("--since", help="inclusive ISO host timestamp with timezone")
    ap.add_argument("--until", help="inclusive ISO host timestamp with timezone")
    ap.add_argument("--evidence", action="store_true", help="summarize reception, merges and incarnations")
    ap.add_argument("--overlaps", action="store_true", help="compare same-BSSID planned exchange windows")
    ap.add_argument("--json", action="store_true", help="machine-readable evidence and optional overlaps")
    ap.add_argument("--local-only", action="store_true", help="do not read Miner6")
    args = ap.parse_args()
    if args.recent < 0 or args.tail_bytes <= 0:
        ap.error('--recent must be nonnegative and --tail-bytes positive')
    try:
        since = evidence.timestamp(args.since) if args.since else None
        until = evidence.timestamp(args.until) if args.until else None
    except ValueError as exc:
        ap.error(str(exc))
    if since is not None and until is not None and since > until:
        ap.error('--since must not be later than --until')
    datasets, raw, warnings = [], [], {}
    for path in sorted(args.log_dir.glob('cat.usb*.out')):
        match = re.fullmatch(r'cat\.usb(\d+)\.out', path.name)
        if not match:
            continue
        i = int(match.group(1))
        data = b''
        if path.exists():
            data = read_local_tail(path, args.tail_bytes)
        raw.append((f'local usb{i}', data))
    if not args.local_only:
        for i in range(2):
            data = read_remote(args.remote_host,
                               f'{args.remote_dir}/cat.usb{i}.out', args.tail_bytes)
            raw.append((f'miner6 usb{i}', data))
    for name, data in raw:
        selected, notes = evidence.select(data, args.session, since, until)
        warnings[name] = notes
        datasets.append((name, selected))
        for note in notes:
            print(f'{name}: {note}', file=sys.stderr)
    if args.evidence or args.overlaps or args.json:
        summaries = {name: evidence.summarize(data) for name, data in datasets}
        pairs = evidence.overlaps(datasets) if args.overlaps else []
        caveats = ['Overlap is same-BSSID planned-window evidence, not proof of radio delivery.',
                   'Host clocks must be approximately aligned; 15-second host-time gate applied.',
                   'Epoch rejection totals include normal self-origin relay rejection.',
                   'Absence of a sampled packet is not proof that no packet arrived.']
        if args.json:
            print(json.dumps({'boards': summaries, 'overlaps': pairs,
                              'warnings': warnings, 'caveats': caveats}, indent=2))
        else:
            for name, summary in summaries.items():
                latest = summary['latest'] or {}
                print(f"{name:<12} cycles={summary['complete_cycles']} partial={summary['partial_cycles']} "
                      f"zero-rx={summary['zero_rawrx_cycles']} valid={summary['valid_reports']} "
                      f"bad-length={summary['bad_length']} incarnation-changes={summary['observed_incarnation_changes']}")
                print(f"  latest: wake={latest.get('wake')} home={latest.get('home')} "
                      f"listeners={latest.get('listeners')} health={latest.get('health')} "
                      f"rawrx={latest.get('rawrx')} session={latest.get('session')}")
                print(f"  merges={summary['merge']} epoch-rejections(includes-self)={summary['epoch_rejections_including_self']}")
            if args.overlaps:
                print(f'Same-BSSID planned overlaps: {len(pairs)} (showing latest 20)')
                for pair in pairs[-20:]:
                    print(json.dumps(pair, sort_keys=True))
            for note in caveats:
                print('Note: ' + note)
        return 0
    if args.convergence:
        print("Convergence dashboard | KPI = complete wake/sleep cycles after flush until 10/10 consensus")
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
        global_events = global_home_convergences(datasets, swarm_board_count())
        print("Global 7/7 home convergence events | same BSSID, listeners ignored")
        if not global_events:
            print("  none in retained tails")
        for event in global_events:
            stamp = datetime.fromtimestamp(event['host_time']).astimezone().isoformat(timespec='seconds')
            print(f"  {stamp}  age={human_age(time.time()-event['host_time'])}  home={event['bssid']}")
        print(f"LATEST RECORDS {'all qualified (not necessarily simultaneous)' if all(observed) else 'not all qualified'}")
    else:
        print(f"Rendezvous dashboard | last {args.recent} complete cycles | KPI = exchange-start → deep-sleep")
        for name, data in datasets:
            dashboard(name, data, args.recent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
