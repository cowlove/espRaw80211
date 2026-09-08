# Rendezvous Timing and Logging Implementation Plan

Status: design agreed; firmware packet changes are not yet implemented.

This document records the agreed direction for the next implementation pass.
The six deployed boards are controlled as one ecosystem: a firmware packet
layout revision is deployed to all six boards together. Backward compatibility
with older deployed firmware is therefore not a design requirement.

## Goals

The logs should let offline analysis answer, with evidence:

1. Which beacon/BSSID each board was targeting or using?
2. Were two boards visiting the same beacon during overlapping exchange windows?
3. Did the ESP-NOW callback receive the other board's physical MAC?
4. If a packet arrived, was it valid, rejected, or ignored by association logic?
5. How old was the association information when it was transmitted and received?
6. Did a scouting mission skip or displace the board's normal home rendezvous?

The implementation should improve observability without making the ESP-NOW
payload large enough to alter the timing behavior being measured.

## Host-side serial logging

The serial pipeline prefixes every captured firmware line with:

```text
<ISO wall time> host_mono_ns=<value> board=usbN port=/dev/ttyUSBN session=<id> | <original firmware line>
```

The logger also emits a session metadata line containing host, process, port,
board label, and session identity.

Two clocks are deliberately retained:

- wall time makes logs readable and correlatable with external events;
- host monotonic time supports reliable ordering and interval measurement.

These timestamps represent host receipt of serial output. They do not replace
beacon TSF timestamps for proving RF/rendezvous overlap.

Log files may continue to append indefinitely. Analysis must support selecting:

- the newest logger session;
- a specified session or host-time interval; or
- all historical sessions.

Session metadata, rather than device uptime alone, must delimit runs because a
board reset can repeat firmware-local timestamps and sequence values.

## Deployment and logger lifecycle

The deployment script should own the logger lifecycle:

- discover `/dev/ttyUSBN` boards by default;
- accept explicit indices such as `--boards 0,1,2,3`;
- build before stopping existing monitors;
- terminate all matching `*.usbN` screen sessions, including stale duplicates;
- create fresh detached screen sessions;
- run optional erase, then upload;
- start the timestamping serial logger only after upload succeeds;
- append to `cat.usbN.out`.

The script must not depend on injecting Ctrl-C into a pre-existing foreground
shell. `--erase-flash` remains explicitly destructive; `--keep-screens` is an
intentional opt-out for reusing existing sessions.

## Compact ESP-NOW timing tuple

The packet-format change is intentionally separate from the host logging work.
The current proposed per-timing-record layout is:

```text
full BSSID
beacon TSF milliseconds (uint32 low bits)
exchange-start delta milliseconds (signed int16)
exchange-end delta milliseconds (signed int16)
origin/generation/age data
boot/wake/exchange sequence context
```

### Timestamp semantics

`beacon_ts_ms_low` is the observed beacon TSF converted to milliseconds. A
32-bit millisecond value wraps after approximately 49.7 days. The sequence and
age context is used to resolve wrap, reboot, and stale-report ambiguity.

The signed exchange-window deltas are relative to the beacon timestamp. A
16-bit signed millisecond delta covers approximately +/-32.7 seconds, which is
ample for the current multi-second exchange window. Millisecond precision is
adequate for this experiment; sub-millisecond accuracy is not needed to decide
whether multi-second rendezvous windows overlap.

The full BSSID is retained. No BSSID dictionary or abbreviation table is used
in the first implementation because independently understandable records are
more valuable than the small space saving while the protocol is under
investigation.

### Packet-size policy

Minimize the record size, because smaller records allow more beacon/association
entries in one ESP-NOW packet. Optimize in this order:

1. millisecond rather than microsecond timing;
2. one full timestamp plus signed window deltas;
3. fixed-width age and sequence fields;
4. omit redundant compatibility/revision metadata;
5. measure the actual serialized size and remaining ESP-NOW payload budget.

The existing report prefix/magic, length checks, and any parser-required
framing/version field remain. Fields whose only purpose is rolling
compatibility between old and new deployments may be removed because all six
boards are revised together.

Before editing the packet structure, inspect and record the current serialized
report size and the ESP-NOW payload limit. Do not assume that a nominal C/C++
struct layout equals its wire size; account for packing, padding, and explicit
serialization.

## Firmware logging around the packet

The local firmware log should retain more detail than the packet needs. Each
exchange should identify:

- boot ID, wake sequence, and exchange sequence;
- home, proposal, scout target, and actual timing anchor;
- BSSID and beacon TSF observation;
- exchange start/end and lateness;
- scan candidate BSSID, RSSI, channel, target-hit count, and scan sequence;
- whether the home exchange was skipped, attempted, incomplete, or healthy;
- raw/rx/valid counts and parse rejection counts;
- physical ESP-NOW callback MAC and protocol report-origin MAC;
- deduplicated per-source frame/valid/rejection/refresh counts;
- complete association entries with origin, selected BSSID, generation, age,
  freshness, current-home, and qualification flags;
- reset reason and consensus counters.

The physical callback source and protocol report origin must remain separate;
their byte-order normalization must be explicit so a representation difference
is not mistaken for relaying.

## Cross-BSSID analysis

Beacon TSF values are only directly comparable when they belong to the same
BSSID. The analyzer may estimate relative offsets between different beacon
clocks using repeated observations, but it must label those estimates and
include uncertainty/drift rather than treating them as exact global time.

The analyzer should eventually produce an overlap matrix resembling:

```text
wake/exchange: ...
usb0 target=BSSID-X window=[start,end]
usb1 target=BSSID-X window=[start,end]
translated overlap: YES
usb0 received usb1 physical MAC: NO
usb1 received usb0 physical MAC: NO
classification: overlapping-window / below-callback loss
```

Host timestamps provide run ordering, reboot/session boundaries, and a useful
fallback when TSF data is absent. They are not by themselves proof of RF
overlap because serial output can be buffered or delayed.

## Implementation order

1. Validate the current report header and serialized payload size.
2. Finish and validate host timestamp/session-aware logging.
3. Add compact timing fields without changing rendezvous decisions.
4. Add local receive-time and packet-summary logging.
5. Update the analyzer to select sessions and correlate timing windows.
6. Build, deploy, and erase/reflash all six boards as one controlled revision.
7. Run a clean experiment and review packet size, timing, and log volume before
   adding further fields.

