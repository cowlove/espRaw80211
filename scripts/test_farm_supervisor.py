#!/usr/bin/env python3
"""Reset the hardware test farm after sustained observed 7/7 convergence.

Device logs are authoritative. This process keeps only transient candidate and
acknowledgment state; reset events and epoch boundaries are emitted by loggers
and firmware into the existing append-only board logs.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import shlex
import subprocess
import sys
import time
import uuid
from pathlib import Path

import analyze_rendezvous as analyzer
import rendezvous_evidence as evidence
from test_swarm_config import swarm_board_count


def timestamped(message, now=None):
    """Print one compact, locally timestamped supervisor status line."""
    when = time.localtime(time.time() if now is None else now)
    print(f'{time.strftime("%H:%M:%S", when)} {message}', flush=True)


def board_alias(name):
    prefix = 'M' if name.startswith('miner6 ') else 'L'
    try:
        return f'{prefix}{int(name.rsplit("usb", 1)[1])}'
    except (IndexError, ValueError):
        return name


def format_home_groups(groups, unknown=()):
    def alias_key(value):
        return (value[:1], int(value[1:])) if value[1:].isdigit() else (value, -1)

    parts = []
    for home, names in sorted(groups.items(),
                              key=lambda item: (-len(item[1]), item[0])):
        aliases = ','.join(sorted((board_alias(name) for name in names),
                                  key=alias_key))
        parts.append(f'{home}={aliases}')
    if unknown:
        parts.append('?=' + ','.join(sorted(board_alias(name) for name in unknown)))
    return ' '.join(parts)


def convergence_state(datasets, now, required, max_age,
                      observation_floors=None):
    """Return unanimous home, failure reason, and current usable home groups."""
    observation_floors = observation_floors or {}
    if len(datasets) != required:
        return None, f'expected {required} logs, found {len(datasets)}', {}, []
    groups = {}
    unknown = []
    first_reason = None
    for name, data in datasets:
        latest = (evidence.summarize(data)['latest'] or {})
        home, observed = latest.get('home'), latest.get('host_time')
        if not home or home == '-' or observed is None:
            unknown.append(name)
            first_reason = first_reason or f'{name} has no current home observation'
            continue
        if observed <= observation_floors.get(name, float('-inf')):
            unknown.append(name)
            first_reason = first_reason or f'{name} awaiting post-epoch home observation'
            continue
        age = now - observed
        if age < -5 or age > max_age:
            unknown.append(name)
            first_reason = first_reason or f'{name} latest observation age {age:.1f}s'
            continue
        groups.setdefault(home, []).append(name)
    if first_reason:
        return None, first_reason, groups, unknown
    if len(groups) != 1:
        return None, f'{len(groups)} current homes', groups, unknown
    return next(iter(groups)), None, groups, unknown


def load_datasets(log_dir, remote_host, remote_dir, local_boards, remote_boards,
                  tail_bytes=1_000_000):
    datasets = []
    for index in local_boards:
        path = Path(log_dir) / f'cat.usb{index}.out'
        try:
            data = analyzer.read_local_tail(path, tail_bytes)
        except OSError as error:
            print(f'local read failed ({path}): {error}', file=sys.stderr)
            data = b''
        datasets.append((f'local usb{index}', data))
    for index in remote_boards:
        data = analyzer.read_remote(
            remote_host, f'{remote_dir}/cat.usb{index}.out', tail_bytes)
        datasets.append((f'miner6 usb{index}', data))
    return datasets


def convergence_snapshot(datasets, now, required, max_age,
                         observation_floors=None):
    home, reason, _, _ = convergence_state(
        datasets, now, required, max_age, observation_floors)
    return home, reason


def epoch_acknowledgments(datasets, request_time):
    acknowledged = {}
    for name, data in datasets:
        for _, _, wall, body in evidence.records(data):
            if (wall is not None and wall >= request_time - 2 and
                    'TEST EPOCH RESET ' in body):
                acknowledged[name] = max(wall, acknowledged.get(name, wall))
    return acknowledged


def atomic_request(path, token):
    path = Path(path)
    temporary = path.with_name(path.name + f'.{os.getpid()}.tmp')
    temporary.write_text(token + '\n', encoding='utf-8')
    os.replace(temporary, path)


def issue_reset_requests(remote_host, local_boards, remote_boards, token, dry_run=False):
    local_paths = [Path('/tmp') / f'espRaw80211-usb{i}.reset-request'
                   for i in local_boards]
    remote_paths = [f'/tmp/espRaw80211-usb{i}.reset-request'
                    for i in remote_boards]
    if dry_run:
        for path in local_paths:
            timestamped(f'DRY-RUN reset request {path} id={token}')
        for path in remote_paths:
            timestamped(f'DRY-RUN reset request {remote_host}:{path} id={token}')
        return

    def local():
        for path in local_paths:
            atomic_request(path, token)

    def remote():
        commands = []
        for path in remote_paths:
            temporary = path + f'.{token}.tmp'
            commands.append(
                f"printf '%s\\n' {shlex.quote(token)} > {shlex.quote(temporary)} && "
                f"mv -f -- {shlex.quote(temporary)} {shlex.quote(path)}")
        subprocess.run(['ssh', '-x', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                        remote_host, ' && '.join(commands)], check=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(local), pool.submit(remote)]
        for future in futures:
            future.result()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir', type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument('--remote-host', default='miner6.local')
    parser.add_argument('--remote-dir', default='~/src/espRaw80211')
    parser.add_argument('--poll-seconds', type=float, default=15)
    parser.add_argument('--sustain-seconds', type=float, default=120)
    parser.add_argument('--freshness-seconds', type=float, default=180)
    parser.add_argument('--ack-timeout-seconds', type=float, default=180)
    parser.add_argument('--local-boards', default='0,1,2,3,4')
    parser.add_argument('--remote-boards', default='0,1')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if min(args.poll_seconds, args.freshness_seconds,
           args.ack_timeout_seconds) <= 0 or args.sustain_seconds < 0:
        parser.error('durations must be positive (sustain may be zero)')
    try:
        local_boards = [int(value) for value in args.local_boards.split(',')]
        remote_boards = [int(value) for value in args.remote_boards.split(',')]
    except ValueError:
        parser.error('board lists must be comma-separated integers')
    if (len(set(local_boards)) != len(local_boards) or
            len(set(remote_boards)) != len(remote_boards)):
        parser.error('board lists must not contain duplicates')

    required = swarm_board_count()
    if len(local_boards) + len(remote_boards) != required:
        parser.error(f'configured reset targets must total {required} boards')
    candidate_home = None
    candidate_since = None
    reset_time = None
    reset_token = None
    observation_floors = {}
    last_status = None
    timestamped(f'test-farm supervisor started boards={required} '
                f'poll={args.poll_seconds:g}s sustain={args.sustain_seconds:g}s '
                f'freshness={args.freshness_seconds:g}s')
    while True:
        now = time.time()
        datasets = load_datasets(args.log_dir, args.remote_host, args.remote_dir,
                                 local_boards, remote_boards)
        if reset_time is not None:
            acknowledged = epoch_acknowledgments(datasets, reset_time)
            missing = sorted(name for name, _ in datasets if name not in acknowledged)
            status = f'reset id={reset_token} acknowledged={len(acknowledged)}/{required}'
            if status != last_status:
                timestamped(status + (f' missing={",".join(missing)}' if missing else ''))
                last_status = status
            if len(acknowledged) == required:
                timestamped(f'reset id={reset_token} complete; monitoring new epoch')
                observation_floors = acknowledged
                reset_time = reset_token = None
                candidate_home = candidate_since = None
                last_status = None
            elif now - reset_time > args.ack_timeout_seconds:
                timestamped(f'reset id={reset_token} blocked after {now-reset_time:.0f}s; '
                            f'missing={",".join(missing)}')
            time.sleep(args.poll_seconds)
            continue

        home, reason, groups, unknown = convergence_state(
            datasets, now, required, args.freshness_seconds,
            observation_floors)
        if home is None:
            topology = format_home_groups(groups, unknown)
            status = f'not converged: {reason}' + (f' | {topology}' if topology else '')
            if status != last_status:
                timestamped(status, now)
                last_status = status
            candidate_home = candidate_since = None
        elif home != candidate_home:
            candidate_home, candidate_since = home, now
            last_status = None
            timestamped(f'convergence candidate home={home}; sustain timer started', now)
        else:
            sustained = now - candidate_since
            status = f'converged home={home} sustained={sustained:.0f}s'
            if status != last_status:
                timestamped(status, now)
                last_status = status
            if sustained >= args.sustain_seconds:
                reset_token = uuid.uuid4().hex
                reset_time = time.time()
                timestamped(f'requesting coordinated cold reset id={reset_token} home={home}')
                issue_reset_requests(args.remote_host, local_boards, remote_boards,
                                     reset_token, args.dry_run)
                if args.dry_run:
                    return 0
                last_status = None
        time.sleep(args.poll_seconds)


if __name__ == '__main__':
    raise SystemExit(main())
