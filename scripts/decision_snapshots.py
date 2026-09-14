#!/usr/bin/env python3
"""Parse and validate appointment-decision snapshots from CSIM logs."""
from __future__ import annotations

import argparse
import dataclasses
import re
from pathlib import Path

SNAPSHOT = re.compile(
    r"@v v=(?P<v>\d+) e=(?P<e>[0-9a-f]+) w=(?P<w>\d+) "
    r"x=(?P<x>\d+) i=(?P<i>\d+) o=(?P<o>\d+) k=(?P<k>\d+) "
    r"src=(?P<src>\S+) h=(?P<h>[0-9a-f]+) hm=(?P<hm>\d+) "
    r"t=(?P<t>[0-9a-f]+) tm=(?P<tm>\d+) pb=(?P<pb>[0-9a-f]+) "
    r"ph=(?P<ph>[0-9a-f]+) pm=(?P<pm>\d+) pa=(?P<pa>\d+) "
    r"n=(?P<n>\d+) q=(?P<q>[0-9a-f]+) actual=(?P<actual>\S+) "
    r"replay=(?P<replay>\S+)"
)
ROW = re.compile(
    r"@vrow e=(?P<e>[0-9a-f]+) w=(?P<w>\d+) x=(?P<x>\d+) "
    r"i=(?P<i>\d+) o=(?P<o>\d+) m=(?P<m>[0-9a-f]+) "
    r"b=(?P<b>[0-9a-f]+) g=(?P<g>\d+) a=(?P<a>\d+) z=(?P<z>[0-9a-f]+)"
)
MASK64 = (1 << 64) - 1


def _key(values: dict[str, str]) -> tuple[int, int, int, int, int]:
    return (
        int(values["e"], 16), int(values["w"]), int(values["x"]),
        int(values["i"]), int(values["o"]),
    )


def _mix(value: int, item: int) -> int:
    return ((value ^ item) * 1099511628211) & MASK64


@dataclasses.dataclass
class ParsedSnapshot:
    fields: dict[str, str]
    rows: list[dict[str, str]] = dataclasses.field(default_factory=list)

    def validate(self) -> None:
        if int(self.fields["v"]) != 1:
            raise ValueError(f"unsupported snapshot version {self.fields['v']}")
        if self.fields["actual"] != self.fields["replay"]:
            raise ValueError(
                f"decision mismatch {self.fields['actual']} != {self.fields['replay']}"
            )
        expected = int(self.fields["n"])
        if len(self.rows) != expected:
            raise ValueError(f"association rows {len(self.rows)} != expected {expected}")
        fingerprint = 1469598103934665603
        for row in self.rows:
            for field, base in (("m", 16), ("b", 16), ("g", 10),
                                ("a", 10), ("z", 16)):
                fingerprint = _mix(fingerprint, int(row[field], base))
        expected_fingerprint = int(self.fields["q"], 16)
        if expected and fingerprint != expected_fingerprint:
            raise ValueError(
                f"association fingerprint {fingerprint:x} != {expected_fingerprint:x}"
            )
        if not expected and expected_fingerprint:
            raise ValueError("empty association snapshot has a fingerprint")


def parse_lines(lines: list[str]) -> list[ParsedSnapshot]:
    snapshots: dict[tuple[int, int, int, int, int], ParsedSnapshot] = {}
    pending_rows: dict[tuple[int, int, int, int, int], list[dict[str, str]]] = {}
    order: list[tuple[int, int, int, int, int]] = []
    for line in lines:
        row_match = ROW.search(line)
        if row_match:
            values = row_match.groupdict()
            pending_rows.setdefault(_key(values), []).append(values)
            continue
        snapshot_match = SNAPSHOT.search(line)
        if not snapshot_match:
            continue
        values = snapshot_match.groupdict()
        key = _key(values)
        if key in snapshots:
            raise ValueError(f"duplicate snapshot key {key}")
        snapshots[key] = ParsedSnapshot(values)
        order.append(key)
    for key, rows in pending_rows.items():
        if key not in snapshots:
            raise ValueError(f"association rows without snapshot {key}")
        snapshots[key].rows.extend(rows)
    result = [snapshots[key] for key in order]
    for snapshot in result:
        snapshot.validate()
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--require-snapshots", action="store_true")
    args = parser.parse_args()
    snapshots = parse_lines(args.log.read_text(errors="replace").splitlines())
    if args.require_snapshots and not snapshots:
        raise SystemExit("no appointment decision snapshots found")
    print(f"validated {len(snapshots)} appointment decision snapshots")


if __name__ == "__main__":
    main()
