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
import statistics
from collections import defaultdict
from itertools import combinations
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
SCOUT_LINK_MIN_WIRE_VERSION = 7


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
        if START in line or '@e begin=' in line:
            if current is not None:
                current = None
            current = Cycle(t, t, capture=capture)
        elif current is not None and (END in line or
                                      'exchange complete interval ' in line or
                                      '@e end=' in line):
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
    # A nonpositive limit is an explicit, potentially expensive full-history read.
    quoted = ('"$HOME"/' + shlex.quote(path[2:])) if path.startswith('~/') else shlex.quote(path)
    reader = f"cat -- {quoted}" if tail_bytes <= 0 else f"tail -c {tail_bytes} -- {quoted}"
    cmd = ["ssh", "-C", "-x", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
           host, reader]
    try:
        timeout = 300 if tail_bytes <= 0 else 30
        return subprocess.run(cmd, check=True, capture_output=True, timeout=timeout).stdout
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"remote read failed ({host}:{path}): {(exc.stderr or b'').decode(errors='replace').strip()}", file=sys.stderr)
        return b""


def read_local_tail(path: Path, tail_bytes: int) -> bytes:
    """Read at most the requested suffix without loading the whole log."""
    if tail_bytes <= 0:
        return path.read_bytes()
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


def reset_recovery_events(datasets, required: int, max_skew: float = 90):
    """Pair reset waves with the next observed all-board home consensus.

    Reset waves are grouped by host receipt time because each board executes
    its deliberate reset independently.  The consensus endpoint is the next
    observational same-home event, not the later 10/10 qualification.
    """
    reset_rows = []
    for name, data in datasets:
        for _, _, wall, body in evidence.records(data):
            if wall is not None and 'TEST RESET EXECUTED' in body:
                reset_rows.append((wall, name))
    reset_rows.sort()
    waves = []
    for wall, name in reset_rows:
        if not waves or wall - waves[-1]['last'] > max_skew:
            waves.append({'first': wall, 'last': wall, 'boards': {name}})
        else:
            waves[-1]['last'] = wall
            waves[-1]['boards'].add(name)
    consensus = global_home_convergences(datasets, required, max_skew)
    observations = []
    board_names = {name for name, _ in datasets}
    for name, data in datasets:
        for cycle in evidence.parse_evidence(data)[0]:
            if cycle.wall is not None and cycle.home:
                observations.append((cycle.wall, name, cycle.home))
    observations.sort()
    results = []
    wave_index = 0
    while wave_index < len(waves):
        wave = waves[wave_index]
        # Reset waves can overlap one recovery episode.  Evaluate from the
        # latest reset currently in the episode, then absorb any subsequent
        # wave that occurs before its resulting consensus.  This prevents one
        # rendezvous from being printed once per staggered board reset.
        episode_waves = [wave]
        next_index = wave_index + 1
        while True:
            latest_reset = episode_waves[-1]['last']
            latest = {}
            fragmented_at = None
            for wall, name, home in observations:
                if wall < latest_reset:
                    continue
                latest[name] = (wall, home)
                if (len(latest) == len(board_names) and
                        len({value[1] for value in latest.values()}) > 1):
                    fragmented_at = wall
                    break
            following = [event for event in consensus
                         if fragmented_at is not None and
                         event['host_time'] >= fragmented_at]
            event = following[0] if following else None
            if (event is not None and next_index < len(waves) and
                    waves[next_index]['first'] <= event['host_time']):
                episode_waves.append(waves[next_index])
                next_index += 1
                continue
            break

        boards = set()
        for member_wave in episode_waves:
            boards.update(member_wave['boards'])
        # An old common home may still be visible after only one board resets.
        # Require a complete post-wave snapshot that actually fragments before
        # accepting a later same-home event as recovery.
        latest = {}
        fragmented_at = None
        for wall, name, home in observations:
            if wall < episode_waves[-1]['last']:
                continue
            latest[name] = (wall, home)
            if (len(latest) == len(board_names) and
                    len({value[1] for value in latest.values()}) > 1):
                fragmented_at = wall
                break
        following = [event for event in consensus
                     if fragmented_at is not None and
                     event['host_time'] >= fragmented_at]
        event = following[0] if following else None
        results.append({
            'reset_time': episode_waves[0]['first'],
            'reset_end': episode_waves[-1]['last'],
            'boards': sorted(boards),
            'fragmented_time': fragmented_at,
            'consensus_time': event['host_time'] if event else None,
            'bssid': event['bssid'] if event else None,
            'latency': (event['host_time'] - episode_waves[0]['first']) if event else None,
        })
        wave_index = next_index
    return results


def print_reset_recovery(datasets, required: int) -> None:
    print('Rendezvous recovery events | reset episode → emerging consensus')
    results = reset_recovery_events(datasets, required)
    if not results:
        print('  no TEST RESET EXECUTED records in selected logs')
        return
    completed = [row for row in results if row['latency'] is not None]
    for row in results:
        reset = datetime.fromtimestamp(row['reset_time']).astimezone().isoformat(timespec='seconds')
        if row['fragmented_time'] is None:
            print(f"  reset={reset} status=fragmentation-not-observed")
        elif row['latency'] is None:
            fragmented = datetime.fromtimestamp(row['fragmented_time']).astimezone().isoformat(timespec='seconds')
            print(f"  reset={reset} fragmented={fragmented} status=consensus-not-observed")
        else:
            fragmented = datetime.fromtimestamp(row['fragmented_time']).astimezone().isoformat(timespec='seconds')
            consensus = datetime.fromtimestamp(row['consensus_time']).astimezone().isoformat(timespec='seconds')
            print(f"  reset={reset} fragmented={fragmented} consensus={consensus} latency={row['latency']:.1f}s home={row['bssid']}")
    if completed:
        values = [row['latency'] for row in completed]
        print(f"  summary completed={len(completed)}/{len(results)} median={statistics.median(values):.1f}s min={min(values):.1f}s max={max(values):.1f}s")


def current_home_distribution(datasets, summaries=None):
    """Group each logged board's latest observed home by BSSID.

    This is an observational snapshot of boards with serial logs, not global
    swarm membership. Boards without a usable latest home are returned
    separately instead of being counted as a beacon group.
    """
    groups = defaultdict(list)
    unknown = []
    for name, data in datasets:
        latest = ((summaries[name] if summaries is not None else
                   evidence.summarize(data))['latest'] or {})
        home = latest.get('home')
        if home and home != '-':
            groups[home].append(name)
        else:
            unknown.append(name)
    rows = [
        {'bssid': bssid, 'devices': len(names), 'boards': sorted(names)}
        for bssid, names in groups.items()
    ]
    rows.sort(key=lambda row: (-row['devices'], row['bssid']))
    return rows, sorted(unknown)


def current_home_unchanged_cycles(datasets, parsed=None) -> int:
    """Count completed wake observations in the current full assignment.

    A distribution includes which logged board occupies which BSSID, not merely
    the group-size histogram. Counting begins once every logged board has a
    usable home. A later observation that moves one board starts a new count.
    The result is necessarily bounded by the retained log suffixes.
    """
    board_names = {name for name, _ in datasets}
    observations = []
    for name, data in datasets:
        cycles = parsed[name][0] if parsed is not None else evidence.parse_evidence(data)[0]
        for cycle in cycles:
            if cycle.wall is not None and cycle.home:
                observations.append((cycle.wall, name, cycle.home))
    observations.sort()
    latest = {}
    stable_cycles = 0
    for _, name, home in observations:
        previous = latest.get(name)
        latest[name] = home
        if set(latest) != board_names:
            continue
        if previous is not None and previous != home:
            stable_cycles = 1
        elif stable_cycles:
            stable_cycles += 1
        else:
            # This observation completed the first full assignment visible in
            # the retained tails.
            stable_cycles = 1
    return stable_cycles


def print_current_home_distribution(datasets, summaries=None, parsed=None) -> None:
    rows, unknown = current_home_distribution(datasets, summaries)
    stable_cycles = current_home_unchanged_cycles(datasets, parsed)
    print('Current home distribution | latest observation per logged board | '
          f'unchanged in {stable_cycles} completed wake cycles (retained tails)')
    if not rows:
        print('  no current home observations')
    for row in rows:
        print(f"  {row['bssid']}  devices={row['devices']}  boards={','.join(row['boards'])}")
    if unknown:
        print(f"  unknown-home devices={len(unknown)}  boards={','.join(unknown)}")


def remap_cycles_by_current_identity(parsed):
    """Regroup historical cycles under the USB alias owning that board now."""
    # USB numbers identify today's logger connections, not physical boards.
    # Recover physical identity from report-clock-rx, which ties an incarnation
    # to the sender's stable ESP MAC.  Anchor each MAC to the USB alias owning
    # its most-recent incarnation, then regroup older cycles under that alias.
    epoch_macs = defaultdict(set)
    for cycles in parsed.values():
        for cycle in cycles:
            for record in cycle.received_clocks:
                if record.get('incarnation') and record.get('sender'):
                    epoch_macs[record['incarnation']].add(record['sender'])
    epoch_mac = {epoch: next(iter(macs)) for epoch, macs in epoch_macs.items()
                 if len(macs) == 1}
    mac_aliases = defaultdict(set)
    for name, cycles in parsed.items():
        latest_epoch = next((cycle.epoch for cycle in reversed(cycles) if cycle.epoch), None)
        if latest_epoch in epoch_mac:
            mac_aliases[epoch_mac[latest_epoch]].add(name)
    mac_alias = {mac: next(iter(names)) for mac, names in mac_aliases.items()
                 if len(names) == 1}

    remapped = defaultdict(list)
    for stream_name, cycles in parsed.items():
        for cycle in cycles:
            mac = epoch_mac.get(cycle.epoch)
            alias = mac_alias.get(mac)
            # Keep unresolved current data under its connection label. It can
            # still define opportunities, but cannot be used as attributed
            # sender evidence until another board identifies its incarnation.
            remapped[alias or stream_name].append(cycle)
    parsed = dict(remapped)
    board_origins = {alias: {mac} for mac, alias in mac_alias.items()}
    return parsed, board_origins


def pairwise_link_stats_all(datasets, parsed_evidence=None):
    """Measure all link contexts with one parse and a time-window sweep."""
    # Legacy formats lack the exchange identity and appointment semantics needed
    # to establish a scout opportunity. Never mix them into this statistic.
    parsed = {
        name: [cycle for cycle in (parsed_evidence[name][0] if parsed_evidence is not None
                                   else evidence.parse_evidence(data)[0])
               if cycle.wire is not None and cycle.wire >= SCOUT_LINK_MIN_WIRE_VERSION]
        for name, data in datasets
    }
    parsed, board_origins = remap_cycles_by_current_identity(parsed)

    rows = {context: [] for context in ('scout', 'home', 'all')}
    for left, right in combinations(sorted(parsed), 2):
        left_cycles = sorted(
            (cycle for cycle in parsed[left]
             if cycle.wall is not None and cycle.end_wall is not None and
             cycle.appointment_targets), key=lambda cycle: cycle.wall)
        right_cycles = sorted(
            (cycle for cycle in parsed[right]
             if cycle.wall is not None and cycle.end_wall is not None and
             cycle.appointment_targets), key=lambda cycle: cycle.wall)
        opportunities = {context: [] for context in rows}
        first_possible = 0
        for a in left_cycles:
            while (first_possible < len(right_cycles) and
                   right_cycles[first_possible].end_wall <= a.wall):
                first_possible += 1
            candidate = first_possible
            while candidate < len(right_cycles) and right_cycles[candidate].wall < a.end_wall:
                b = right_cycles[candidate]
                candidate += 1
                scout_targets = ((a.scout_targets & b.appointment_targets) |
                                 (b.scout_targets & a.appointment_targets))
                home_targets = a.home_targets & b.home_targets
                overlap = min(a.end_wall, b.end_wall) - max(a.wall, b.wall)
                if overlap <= 0:
                    continue
                if scout_targets:
                    opportunities['scout'].append((a, b, overlap, sorted(scout_targets)))
                if home_targets:
                    opportunities['home'].append((a, b, overlap, sorted(home_targets)))
                all_targets = scout_targets | home_targets
                if all_targets:
                    opportunities['all'].append((a, b, overlap, sorted(all_targets)))

        for context, context_opportunities in opportunities.items():
            if not context_opportunities:
                continue

            def direction(receiver_cycles, sender):
                values = []
                raw_values = []
                sender_origins = board_origins.get(sender, set())
                known = bool(sender_origins)
                for receiver in receiver_cycles:
                    peer_rows = [receiver.peers[origin] for origin in sender_origins
                                 if origin in receiver.peers]
                    values.append(sum(int(peer.get('valid', 0)) for peer in peer_rows))
                    raw_values.append(sum(int(peer.get('frames', 0)) for peer in peer_rows))
                return {
                    'identity_mapped': known,
                    'received_cycles': sum(value > 0 for value in values),
                    'valid_packets': sum(values),
                    'raw_packets': sum(raw_values),
                    'valid_per_opportunity': sum(values) / len(values),
                    'valid_per_overlap_second':
                        sum(values) / sum(item[2] for item in context_opportunities),
                }

            rows[context].append({
                'left': left, 'right': right,
                'opportunities': len(context_opportunities),
                'overlap_seconds': sum(item[2] for item in context_opportunities),
                'targets': sorted({target for item in context_opportunities for target in item[3]}),
                'left_received_from_right': direction(
                    [item[0] for item in context_opportunities], right),
                'right_received_from_left': direction(
                    [item[1] for item in context_opportunities], left),
            })
    return rows


def pairwise_link_stats(datasets, context='scout'):
    """Measure directed delivery for one requested overlap context."""
    if context not in ('scout', 'home', 'all'):
        raise ValueError(f'unknown link context: {context}')
    return pairwise_link_stats_all(datasets)[context]


def scout_link_stats(datasets):
    """Backward-compatible scout-only pairwise view."""
    return pairwise_link_stats(datasets, 'scout')


def print_pairwise_link_stats(rows, context) -> None:
    labels = {
        'scout': 'scout-involved',
        'home': 'home/home',
        'all': 'combined scout + home/home',
    }
    print(f"ESP-NOW pairwise delivery | {labels[context]} same-target overlaps")
    if not rows:
        print('  no qualifying scout overlaps in retained tails')
        return
    for row in rows:
        print(f"  {row['left']} <-> {row['right']}  opportunities={row['opportunities']} "
              f"overlap={row['overlap_seconds']:.1f}s targets={','.join(row['targets'])}")
        for receiver, sender, key in (
                (row['left'], row['right'], 'left_received_from_right'),
                (row['right'], row['left'], 'right_received_from_left')):
            value = row[key]
            if not value['identity_mapped']:
                print(f'    {receiver} <- {sender}: sender identity unavailable')
                continue
            reliability = 100 * value['received_cycles'] / row['opportunities']
            print(f"    {receiver} <- {sender}: hit={value['received_cycles']}/{row['opportunities']} "
                  f"({reliability:.0f}%) valid/opportunity={value['valid_per_opportunity']:.1f} "
                  f"valid/second={value['valid_per_overlap_second']:.2f} "
                  f"valid={value['valid_packets']} raw={value['raw_packets']}")


def print_scout_link_stats(rows) -> None:
    """Backward-compatible printer used by callers and older tests."""
    print_pairwise_link_stats(rows, 'scout')


def print_pairwise_ascii_table(rows, context, board_names) -> None:
    """Print separate directed throughput and healthy-overlap matrices."""
    labels = {
        'scout': 'scout-involved',
        'home': 'home/home',
        'all': 'combined scout + home/home',
    }
    names = sorted(set(board_names))
    cells_by_direction = {}
    for row in rows:
        for receiver, sender, key in (
                (row['left'], row['right'], 'left_received_from_right'),
                (row['right'], row['left'], 'right_received_from_left')):
            value = row[key]
            if value['identity_mapped']:
                hit_rate = 100 * value['received_cycles'] / row['opportunities']
                cells_by_direction[(receiver, sender)] = (
                    value['valid_per_overlap_second'], hit_rate)
    name_width = max([len('receiver'), *(len(name) for name in names)], default=8)
    cell_width = max(8, *(len(name) for name in names))

    def matrix(title, value_index, formatter):
        print(f"{title} | {labels[context]} same-target overlaps")
        print(f"{'receiver':<{name_width}} <- | " +
              ' | '.join(f'{name:>{cell_width}}' for name in names))
        for receiver in names:
            cells = []
            for sender in names:
                if receiver == sender:
                    cell = '--'
                elif (receiver, sender) in cells_by_direction:
                    cell = formatter(cells_by_direction[(receiver, sender)][value_index])
                else:
                    cell = '-'
                cells.append(f'{cell:>{cell_width}}')
            print(f'{receiver:<{name_width}} <- | ' + ' | '.join(cells))
        print('  rows receive from columns; - = no sampled overlap')

    matrix('ESP-NOW valid packets/second matrix', 0, lambda value: f'{value:.2f}')
    matrix('ESP-NOW healthy overlap sessions matrix', 1,
           lambda value: f'{value:.0f}%')


def csim_beacon_environments(parsed, board_names, limit=10):
    """Summarize per-board scan windows after physical-identity remapping."""
    cycles_only = {name: value[0] if isinstance(value, tuple) else value
                   for name, value in parsed.items()}
    remapped, _ = remap_cycles_by_current_identity(cycles_only)
    environments = []
    for board in board_names:
        cycles = [cycle for cycle in remapped.get(board, []) if cycle.scan_observed]
        by_bssid = defaultdict(list)
        for cycle in cycles:
            for row in cycle.scan_observed:
                by_bssid[row['bssid']].append(row)
        eligible_bssids = [bssid for bssid, observations in by_bssid.items()
                           if any(row['eligible'] for row in observations)]
        ranked = sorted(eligible_bssids, key=lambda bssid: (
            -len(by_bssid[bssid]), bssid))[:limit]
        entries = []
        for bssid in ranked:
            observations = by_bssid[bssid]
            rssis = [row['rssi'] for row in observations]
            measured_rates = [row['packets'] / row['span']
                              for row in observations if row['span'] > 0]
            rates = measured_rates + [0.0] * max(
                0, len(cycles) - len(observations))
            entries.append({
                'bssid': bssid,
                'rssi': statistics.fmean(rssis),
                'rssi_variation': statistics.pstdev(rssis),
                'packet_rate': statistics.fmean(rates) if rates else 0.0,
                'rate_variation': statistics.pstdev(rates) if rates else 0.0,
            })
        if not entries:
            raise ValueError(f'no remapped beacon scan observations for {board}')
        environments.append(entries)
    return environments


def write_csim_pairwise_header(path: Path, rows, board_names,
                               parsed_evidence=None) -> None:
    """Write a complete combined-context RF matrix for the CSIM fleet."""
    names = sorted(set(board_names))
    directions = {}
    for row in rows:
        for receiver, sender, key in (
                (row['left'], row['right'], 'left_received_from_right'),
                (row['right'], row['left'], 'right_received_from_left')):
            value = row[key]
            if value['identity_mapped']:
                directions[(receiver, sender)] = (
                    value['valid_per_overlap_second'],
                    100 * value['received_cycles'] / row['opportunities'])
    missing = [(receiver, sender) for receiver in names for sender in names
               if receiver != sender and (receiver, sender) not in directions]
    if missing:
        pairs = ', '.join(f'{receiver}<-{sender}' for receiver, sender in missing)
        raise ValueError(f'cannot generate CSIM matrix; unsampled links: {pairs}')

    def matrix(index, formatter):
        lines = []
        for receiver in names:
            values = []
            for sender in names:
                value = 0 if receiver == sender else directions[(receiver, sender)][index]
                values.append(formatter(value))
            lines.append('    {' + ', '.join(values) + '},')
        return '\n'.join(lines)

    beacon_text = ''
    if parsed_evidence is not None:
        environments = csim_beacon_environments(parsed_evidence, names)
        environment_rows, counts = [], []
        for entries in environments:
            counts.append(str(len(entries)))
            values = [
                f'{{0x{entry["bssid"]}ULL, {entry["rssi"]:.3f}f, '
                f'{entry["rssi_variation"]:.3f}f, {entry["packet_rate"]:.6f}f, '
                f'{entry["rate_variation"]:.6f}f}}' for entry in entries]
            values += ['{0, 0, 0, 0, 0}'] * (10 - len(values))
            environment_rows.append('    {' + ', '.join(values) + '},')
        beacon_text = f'''\nstruct BeaconEnvironment {{
    uint64_t bssid;
    float rssi;
    float rssiVariation;
    float packetsPerSecond;
    float packetRateVariation;
}};
static constexpr size_t maxBeaconsPerBoard = 10;
static constexpr size_t beaconCount[boardCount] = {{
    {', '.join(counts)},
}};
static constexpr BeaconEnvironment beacons[boardCount][maxBeaconsPerBoard] = {{
{chr(10).join(environment_rows)}
}};
'''

    content = f'''// Generated by scripts/analyze_rendezvous.py --write-csim-header.
// Source context: combined scout + home/home same-target overlaps.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace CsimPairwiseData {{
static constexpr size_t boardCount = {len(names)};
static constexpr const char *boardNames[boardCount] = {{
    {', '.join(json.dumps(name) for name in names)},
}};
static constexpr float packetsPerSecond[boardCount][boardCount] = {{
{matrix(0, lambda value: f'{value:.6f}f')}
}};
static constexpr float healthyWindowPercent[boardCount][boardCount] = {{
{matrix(1, lambda value: f'{value:.6f}f')}
}};
{beacon_text}
}} // namespace CsimPairwiseData
'''
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(content)
    temporary.replace(path)


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
    reset = latest.reset or '-'
    print(f"{name:<12} rendezvous={state:<12} artificial-test-board-count={required} qualified={len(qualified):3d}  latest-consensus={latest.consensus:<5}  reset={reset:<8}  latest-home={latest.home}  latest-listeners={latest.listeners}  latest-exchange={'healthy' if latest.healthy else 'incomplete'}")
    return state == "QUALIFIED"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--recent", type=int, default=10, help="complete cycles per board; 0 shows only latest state (default: 10)")
    ap.add_argument("--log-dir", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--remote-host", default="miner6.local")
    ap.add_argument("--remote-dir", default="~/src/espRaw80211")
    ap.add_argument("--tail-bytes", type=int, default=DEFAULT_TAIL_BYTES,
                    help="maximum suffix read per log; 0 or -1 reads the full file (default: 1000000)")
    ap.add_argument("--convergence", action="store_true", help="show cycles from reset execution to next 10/10 consensus")
    ap.add_argument("--reset-recovery", action="store_true", help="measure reset-wave execution to next emerging same-home consensus")
    ap.add_argument("--session", default="latest", help="latest (default), all, or an exact logger session ID")
    ap.add_argument("--since", help="inclusive ISO host timestamp with timezone")
    ap.add_argument("--until", help="inclusive ISO host timestamp with timezone")
    ap.add_argument("--evidence", action="store_true", help="summarize reception, merges and incarnations")
    ap.add_argument("--overlaps", action="store_true", help="compare same-BSSID planned exchange windows")
    ap.add_argument("--scout-links", "--pairwise-links", dest="scout_links",
                    action="store_true", help="show scout, home/home, and combined pairwise ESP-NOW delivery")
    ap.add_argument("--ascii-table", action="store_true",
                    help="with pairwise links, additionally print directed packets/second matrices")
    ap.add_argument("--write-csim-header", type=Path,
                    help="write complete combined pairwise measurements as a CSIM C++ header")
    ap.add_argument("--json", action="store_true", help="machine-readable evidence and optional overlaps")
    ap.add_argument("--local-only", action="store_true", help="do not read Miner6")
    args = ap.parse_args()
    if args.ascii_table:
        args.scout_links = True
    if args.write_csim_header:
        args.scout_links = True
    if args.ascii_table and args.json:
        ap.error('--ascii-table cannot be combined with --json')
    if args.recent < 0 or args.tail_bytes < -1:
        ap.error('--recent must be nonnegative and --tail-bytes must be -1 or greater')
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
    if args.evidence or args.overlaps or args.scout_links or args.json:
        parsed_evidence = {name: evidence.parse_evidence(data) for name, data in datasets}
        summaries = {name: evidence.summarize(data, parsed_evidence[name])
                     for name, data in datasets}
        home_distribution, unknown_homes = current_home_distribution(datasets, summaries)
        home_distribution_unchanged_cycles = current_home_unchanged_cycles(
            datasets, parsed_evidence)
        pairs = evidence.overlaps(datasets) if args.overlaps else []
        link_stats = (pairwise_link_stats_all(datasets, parsed_evidence)
                      if args.scout_links else {})
        if args.write_csim_header:
            try:
                write_csim_pairwise_header(
                    args.write_csim_header, link_stats['all'],
                    [name for name, _ in datasets], parsed_evidence)
            except ValueError as exc:
                ap.error(str(exc))
        scout_links = link_stats.get('scout', [])
        caveats = ['Overlap is same-BSSID planned-window evidence, not proof of radio delivery.',
                   'Host clocks must be approximately aligned; 15-second host-time gate applied.',
                   'Epoch rejection totals include normal self-origin relay rejection.',
                   'Absence of a sampled packet is not proof that no packet arrived.']
        if args.scout_links:
            caveats.append('Pairwise packet counts cover the complete merged radio interval containing each qualifying overlap.')
        if args.json:
            print(json.dumps({'boards': summaries,
                              'current_home_distribution': home_distribution,
                              'current_home_distribution_unchanged_cycles':
                                  home_distribution_unchanged_cycles,
                              'unknown_home_boards': unknown_homes,
                              'overlaps': pairs,
                              'scout_links': scout_links,
                              'pairwise_links': link_stats,
                              'warnings': warnings, 'caveats': caveats}, indent=2))
        else:
            print_current_home_distribution(datasets, summaries, parsed_evidence)
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
            if args.scout_links:
                for context in ('scout', 'home', 'all'):
                    print_pairwise_link_stats(link_stats[context], context)
                    if args.ascii_table:
                        print_pairwise_ascii_table(
                            link_stats[context], context,
                            [name for name, _ in datasets])
            for note in caveats:
                print('Note: ' + note)
        return 0
    if args.convergence or args.reset_recovery:
        print_current_home_distribution(datasets)
        if args.reset_recovery:
            print_reset_recovery(datasets, swarm_board_count())
        if not args.convergence:
            return 0
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
        print_current_home_distribution(datasets)
        print(f"Rendezvous dashboard | last {args.recent} complete cycles | KPI = exchange-start → deep-sleep")
        for name, data in datasets:
            dashboard(name, data, args.recent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
