# Compact structured logging plan

## Goal

Replace the high-volume human-oriented serial transcript with a compact,
versioned event stream while preserving every fact used by
`scripts/analyze_rendezvous.py` and ad-hoc analysis. Human-readable output
becomes a decoded view rather than the storage format.

## Proposed format

- Firmware emits typed binary events with a schema version and numeric event
  ID.
- Each event contains only its typed positional fields and a boot-relative
  timestamp. Stable boot/session metadata is emitted once in a session header.
- COBS framing permits recovery after truncation, reconnects, and arbitrary
  serial noise. A per-record CRC rejects damaged records.
- The host logger stores append-only framed records. Host wall time,
  monotonic time, physical connection, and logger session identity are stored
  once per connection plus compact receipt-time deltas where needed.
- The analyzer consumes typed records directly. A decoder can reconstruct an
  equivalent readable transcript for debugging and historical comparison.

Fixed event IDs and explicitly typed fields are preferred over verbose JSON or
self-describing CBOR for the production stream. The schema and decoder provide
self-description outside the constrained wire/storage path.

## Equivalence requirement

Inventory every firmware message currently parsed or used during diagnosis,
including identity, scan/beacon evidence, plans and appointments, packet timing,
pairwise reception, association/claim state, migrations, recovery, sleep,
reset, and error counters. For each old line define one structured event and a
deterministic text rendering. CSIM must demonstrate event-for-event semantic
equivalence before text logging is disabled.

Unknown event IDs and newer schema versions must be retained and skipped
safely, not make the whole file unreadable.

## Transition

1. Inventory current messages and define the first event schema.
2. Add a shared encoder/decoder and emit structured events in CSIM alongside
   text.
3. Compare decoded events against the existing analyzer inputs and add
   truncation, corruption, rollover, and reconnect tests.
4. Teach `timestamp_serial.py` and `analyze_rendezvous.py` to use the structured
   stream while retaining legacy-text input.
5. Briefly deploy dual output to the board farm and compare both analyses.
6. Make structured output the default; retain a compile/runtime verbose-text
   diagnostic mode.
7. Rotate and `zstd` historical text logs only after the structured pipeline is
   proven.

## Operational constraints

- Logging must not change rendezvous timing or initialize ESP-NOW earlier.
- Records must remain recoverable after a partial write or USB disconnect.
- Board identity is the stable on-wire MAC/incarnation, never a transient
  `/dev/ttyUSB*` label.
- Schema changes are versioned and backward-readable.
- Preserve raw counters and evidence; derived summaries belong in analysis,
  not firmware.

Expected reduction is roughly 10--30x because repeated labels, field names,
timestamps, ports, and session UUIDs disappear. It should also materially
reduce UART formatting and transmission overhead on the ESP32.
