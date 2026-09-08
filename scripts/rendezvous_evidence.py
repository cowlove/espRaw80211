"""Session-aware offline evidence. Same-BSSID TSF only; no inferred global clock."""
from dataclasses import dataclass, field
from datetime import datetime
from itertools import combinations
import re

PREFIX = re.compile(r'^(\S+) host_mono_ns=(\d+) board=(\S+) port=(\S+) session=(\S+) \| (.*)$')
IDENTITY = re.compile(r'report-identity incarnation ([0-9a-f]+) wake (\d+) wire-version (\d+)(?: exchange (\d+))?')
WINDOW = re.compile(r'beacon-clock target ([0-9a-f]+) tsf-packet (\d+) exchange (\d+)-(\d+)')
APPOINTMENT = re.compile(r'appointment complete exchange (\d+) kind (home|scout) target ([0-9a-f]+) full (\d+) healthy (\d+)')


def timestamp(value):
    result = datetime.fromisoformat(value.replace('Z', '+00:00'))
    if result.tzinfo is None:
        raise ValueError('timestamps must include a timezone')
    return result.timestamp()


def records(data):
    for line in data.replace(b'\x00', b'').decode('utf-8', 'replace').splitlines():
        match = PREFIX.match(line)
        if match:
            wall, mono, board, port, session, body = match.groups()
            try:
                yield line, session, timestamp(wall), body
            except ValueError:
                yield line, None, None, body
        else:
            yield line, None, None, line


def select(data, session='latest', since=None, until=None):
    # Full-history analysis commonly requests the entire capture. Avoid
    # decoding, splitting, retaining, and re-encoding hundreds of MB only to
    # return identical bytes.
    if session == 'all' and since is None and until is None:
        return data, []
    rows = list(records(data))
    sessions = list(dict.fromkeys(row[1] for row in rows if row[1]))
    chosen = sessions[-1] if session == 'latest' and sessions else session
    warnings = []
    if not sessions and session == 'latest':
        warnings.append('no timestamped session identity; showing legacy capture')
        chosen = 'all'
    kept = [row for row in rows if (chosen == 'all' or row[1] == chosen)
            and (since is None or row[2] is not None and row[2] >= since)
            and (until is None or row[2] is not None and row[2] <= until)]
    if sessions and chosen != 'all' and not any(
            row[1] == chosen and row[3].startswith('logger-session ') for row in rows):
        warnings.append('session start outside retained input; history is partial')
    if (since is not None or until is not None) and any(row[2] is None for row in rows):
        warnings.append('untimestamped lines excluded from time selection')
    if not kept:
        warnings.append('selection has no records')
    return ('\n'.join(row[0] for row in kept) + '\n').encode(), warnings


def fields(body):
    return dict(re.findall(r'([a-z][a-z-]*) ([^ ]+)', body))


@dataclass
class Exchange:
    session: str | None
    epoch: str | None
    wake: int | None
    wall: float | None
    wire: int | None
    sequence: int | None = None
    window: tuple | None = None
    home: str | None = None
    listeners: int | None = None
    health: str = 'unknown'
    rawrx: int | None = None
    valid: int | None = None
    merge: dict = field(default_factory=dict)
    epoch_rejected: int = 0
    bad_length: int = 0
    peers: dict = field(default_factory=dict)
    received_clocks: list = field(default_factory=list)
    appointment_kinds: set = field(default_factory=set)
    appointment_targets: set = field(default_factory=set)
    scout_targets: set = field(default_factory=set)
    home_targets: set = field(default_factory=set)
    end_wall: float | None = None


def parse_evidence(data):
    complete = []
    current = None
    identity = (None, None, None, None)
    previous_session = None
    partial = 0
    for _, session, wall, body in records(data):
        if session != previous_session or body.startswith('logger-session '):
            partial += current is not None
            current = None
            identity = (None, None, None, None)
            previous_session = session
        found = IDENTITY.search(body)
        if found:
            partial += current is not None
            current = None
            epoch, wake, wire, sequence = found.groups()
            identity = (epoch, int(wake), int(wire), int(sequence) if sequence else None)
        if 'ESP-NOW exchange phase started' in body:
            partial += current is not None
            current = Exchange(session, identity[0], identity[1], wall, identity[2], identity[3])
        if current is None:
            continue
        found = WINDOW.search(body)
        if found and current.wire is not None and current.wire >= 5:
            bssid, _, start, end = found.groups()
            if int(end) >= int(start):
                current.window = (bssid, int(start), int(end))
        if 'gossip ' in body:
            found = re.search(r'exchange (healthy|incomplete).*?home ([0-9a-f]+) listeners (\d+)', body)
            if found:
                current.health, current.home, listeners = found.groups()
                current.listeners = int(listeners)
            for key in ('rawrx', 'valid'):
                found = re.search(r'\b' + key + r' (\d+)', body)
                if found:
                    setattr(current, key, int(found[1]))
        if 'association-merge attempts ' in body:
            current.merge = {key: int(value) for key, value in
                             re.findall(r'([a-z-]+) (\d+)', body)}
        found = re.search(r'origin-incarnation rejected (\d+)', body)
        if found:
            current.epoch_rejected = int(found[1])
        found = re.search(r'report-framing bad-length (\d+)', body)
        if found:
            current.bad_length = int(found[1])
        if 'espnow summary origin ' in body:
            values = fields(body.split('espnow summary ', 1)[1])
            current.peers[values['origin']] = values
        if 'report-clock-rx sender ' in body:
            clock_body = body.split('report-clock-rx ', 1)[1]
            values = fields(clock_body)
            # Serial output can occasionally concatenate the next TX record
            # without a newline. Preserve the RX record's leading identity
            # instead of allowing duplicate trailing fields to overwrite it.
            identity = re.match(r'sender ([0-9a-f]+) incarnation ([0-9a-f]+)',
                                clock_body)
            if identity:
                values['sender'], values['incarnation'] = identity.groups()
            current.received_clocks.append(values)
        found = APPOINTMENT.search(body)
        if found and (current.sequence is None or int(found[1]) == current.sequence):
            current.appointment_kinds.add(found[2])
            current.appointment_targets.add(found[3])
            if found[2] == 'scout':
                current.scout_targets.add(found[3])
            else:
                current.home_targets.add(found[3])
        if 'deep sleep ' in body or 'exchange complete interval ' in body:
            current.end_wall = wall
            complete.append(current)
            current = None
    partial += current is not None
    return complete, partial


def summarize(data, parsed=None):
    cycles, partial = parsed if parsed is not None else parse_evidence(data)
    epochs = [c.epoch for c in cycles if c.epoch]
    changes = sum(a != b for a, b in zip(epochs, epochs[1:]))
    merge = {}
    for cycle in cycles:
        for key, value in cycle.merge.items():
            merge[key] = merge.get(key, 0) + value
    latest = cycles[-1] if cycles else None
    mac_order = {'same': 0, 'byte_reversed': 0, 'different': 0}
    for cycle in cycles:
        for origin, peer in cycle.peers.items():
            radio = peer.get('radio-from', '').zfill(12)
            origin = origin.zfill(12)
            reversed_radio = ''.join(reversed([radio[i:i+2] for i in range(0, 12, 2)]))
            key = 'same' if origin == radio else 'byte_reversed' if origin == reversed_radio else 'different'
            mac_order[key] += 1
    transitions = []
    for _, session, wall, body in records(data):
        found = re.search(r'origin-incarnation origin ([0-9a-f]+) epoch ([0-9a-f]+) direct (\d+) replaced 1', body)
        if found:
            transitions.append({'session': session, 'host_time': wall,
                                'origin': found[1], 'incarnation': found[2],
                                'direct': found[3] == '1'})
    return {
        'complete_cycles': len(cycles), 'partial_cycles': partial,
        'cycle_unit': 'completed exchange intervals (not logical rounds)',
        'observed_round_identities': len({(c.session, c.epoch, c.wake) for c in cycles}),
        'observed_incarnation_changes': changes,
        'zero_rawrx_cycles': sum(c.rawrx == 0 for c in cycles),
        'valid_reports': sum(c.valid or 0 for c in cycles),
        'bad_length': sum(c.bad_length for c in cycles), 'merge': merge,
        'peer_mac_order_observations': mac_order,
        'origin_replacements': transitions,
        'epoch_rejections_including_self': sum(c.epoch_rejected for c in cycles),
        'latest': None if latest is None else {
            'session': latest.session, 'incarnation': latest.epoch,
            'wake': latest.wake, 'exchange': latest.sequence,
            'home': latest.home, 'listeners': latest.listeners,
            'health': latest.health, 'rawrx': latest.rawrx,
            'valid': latest.valid, 'host_time': latest.wall},
    }


def overlaps(datasets, max_host_skew=15):
    """Require nearby host times AND same BSSID; TSF determines overlap.

    Host proximity rejects obvious clock resets/repeated TSF eras. It is not
    proof of synchronized host clocks or of actual radio-active intervals.
    """
    parsed = {name: parse_evidence(data)[0] for name, data in datasets}
    result = []
    for left, right in combinations(parsed, 2):
        for a in parsed[left]:
            if not a.window or a.wall is None:
                continue
            for b in parsed[right]:
                if not b.window or b.wall is None or abs(a.wall - b.wall) > max_host_skew:
                    continue
                if a.window[0] != b.window[0]:
                    continue
                duration = min(a.window[2], b.window[2]) - max(a.window[1], b.window[1])
                if duration <= 0:
                    continue
                def reception(receiver, sender):
                    # Match explicit sender epoch+wake, not another nearby cycle.
                    matches = [r for r in receiver.received_clocks
                               if sender.epoch is not None and sender.wake is not None
                               and r.get('incarnation') == sender.epoch
                               and ((sender.sequence is not None and r.get('exchange') == str(sender.sequence))
                                    or (sender.sequence is None and r.get('wake') == str(sender.wake)))]
                    if matches:
                        return 'report-observed'
                    if receiver.rawrx == 0:
                        return 'no-raw-callbacks'
                    return 'unknown (one timing sample/peer/exchange)'
                result.append({'left': left, 'right': right, 'bssid': a.window[0],
                               'left_wake': a.wake, 'right_wake': b.wake,
                               'left_exchange': a.sequence, 'right_exchange': b.sequence,
                               'left_incarnation': a.epoch, 'right_incarnation': b.epoch,
                               'host_time': max(a.wall, b.wall),
                               'overlap_ms': duration / 1000,
                               'left_received_right': reception(a, b),
                               'right_received_left': reception(b, a)})
    return sorted(result, key=lambda row: row['host_time'])
