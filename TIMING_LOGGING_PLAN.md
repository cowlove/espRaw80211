# Rendezvous Timing and Logging Implementation Plan

## Exchange-window placement contract (2026-09-10)

This is the agreed executor policy for the next timing implementation slice.
It supersedes any earlier suggestion to initialize ESP-NOW exactly at phase
zero or to gate TX/RX tightly at appointment boundaries.

- Use one fixed, shared phase relative to the selected beacon's TSF period.
  The current phase remains 5 seconds after rollover.  A fixed offset is valid
  as long as all peers calculate it consistently.
- Schedule deep-sleep wake sufficiently early to absorb observed sleep-timer
  error and collect beacons before ESP-NOW affects capture (currently about
  5.5 seconds).
- Keep beacon capture active until a bounded ESP-NOW warm-up lead before the
  first planned interval (initially 500 ms).  Start initialization then, not
  immediately after planning and not exactly at the appointment boundary.
- Promise complete *coverage* of each appointment.  ESP-NOW may be ready and
  active during the warm-up lead; do not delay or explicitly stop TX/RX merely
  to align individual milliseconds with the planned start/end.
- Coalesced appointments use one continuous radio-active interval.  They must
  cover every included appointment; later members do not trigger another init
  or radio gate.
- Record planned start, init begin/completion, and first exchange activity.
  Tune only the meaningful large boundaries—acquisition lead/window and
  exchange duration—after hardware measurements, rather than micromanaging
  sub-window TX/RX timing now.

Implementation readiness: the intended changes are bounded warm-up scheduling
and compact timing observability.  No new clock phase, per-packet gate, or
special merged-window state machine is required.

### Long-sleep stutter refinement (2026-09-10)

The nominal test rendezvous period is now 120 seconds. Policy freshness and
qualification use logical wake cycles, not wall-clock seconds, so this change
does not alter their meaning. Fixed physical durations remain deliberate:
five seconds for normal beacon acquisition and exchange, 500ms ESP-NOW warm-up,
and a 60-second maximum continuous singleton wake.

When an executor sleep to the normal final-acquisition wake exceeds 60 seconds,
split it. The board first sleeps to roughly 60 seconds before the planned
exchange boundary, persisting only the target BSSID (never a boot-local timer
deadline). This stutter wake remains beacon-capture-only. Its first direct
target beacon immediately supplies a fresh TSF/local projection, after which
the board re-sleeps to the usual 5.5-second acquisition lead plus a one-second
stutter safety margin for boot variance. It does not wait
for scan ranking or initialize ESP-NOW. If no target arrives within the normal
five-second capture period—or the recalculated target is already within the
final acquisition lead—the stutter intent is cleared and ordinary planning /
missing-home recovery proceeds.

Executor integration (2026-09-08, not deployed): the live loop now executes
home-first plans with rotating additional scouts and continuous radio activity
across merged windows. Two home appointments form the planning horizon; the
merge gap is 1 second and cumulative additional scout budget is 7 seconds.
Sleep uses a 5.5-second boot/acquisition lead. The initial beacon-only acquisition
and delayed ESP-NOW initialization are preserved. Missing fresh home timing
keeps the board awake to reacquire rather than substituting a scout.

Membership age and reset qualification now advance by elapsed logical periods,
not physical boots. Only full, healthy home coverage qualifies a round; partial
or scout coverage cannot do so. Wire version 7 adds a persisted exchange sequence
for multiple intervals in one round: maximum packet size is 180 bytes. The
analyzer closes intervals explicitly and matches sampled reception by incarnation
and exchange sequence; its legacy cycle count denotes exchanges, not rounds.

Validation: 20 host tests, including same-round exchange disambiguation. A
300-second four-node CSIM run exercised merged home/scout intervals, full and
partial coverage, packet reception, logical aging, and sleep/re-execution.
CSIM is not proof of physical radio-state recovery or six-board convergence.
All six physical boards remain on version 6; coordinated deployment and live
reset/rejoin validation are still required. Earlier status sections below are
historical checkpoints, including the original planner integration checklist.

Status: host infrastructure, TSF/merge diagnostics, sender timing, and
incarnation-aware membership are implemented through wire version 6, deployed
to all six boards on 2026-09-08. Session-aware offline same-BSSID correlation is
implemented. Baseline/reset-rejoin validation, cross-BSSID clock reconstruction,
and interval scheduling remain pending. Earlier slice descriptions below are
historical checkpoints.

Interval planner slice (2026-09-08): `rendezvousPlanner.h` implements a pure,
allocation-free bounded appointment planner, not yet connected to the firmware
executor. Required home appointments are added first; failing to fit one marks
the entire plan invalid. Optional scout additions roll back on capacity/budget
failure. The cumulative scout budget measures additional awake time relative
to the home-only plan, including bridged gaps. Sorting/union preserves each
appointment's bitmask and merges touching, overlapping and configured nearby
intervals. Sleep hints include an explicit caller-supplied boot/acquisition lead;
invalid or exhausted plans request replanning, not indefinite sleep.

The clock helper projects the current or next beacon-relative appointment into
one local monotonic clock. A currently active window is clipped at `now` and
marked late: partial coverage must never be counted as full qualification.
Phase denotes exchange start within a period, separate from acquisition/wake
lead. Overflow and invalid timing parameters fail explicitly; TSF clock startup
does not create a fictitious negative-time previous appointment. Observations
must be freshness-qualified by the caller. The helper assumes unit-rate TSF/local
projection; drift/uncertainty guards remain an executor policy, not hidden here.

Fair selection walks eligible BSSIDs after a persisted cursor and wraps; caller
records each attempt/defer outcome and advances the cursor so an unreachable
target cannot starve others. Eligibility (RSSI, channel, freshness), cadence,
and retry budget remain explicit executor responsibilities.

Validation: 18 host tests pass. Planner tests include 1,000 deterministic
random schedules with undefined-behavior sanitizer, cumulative budget and
capacity failure, transitive merging, late windows, TSF/uint64 boundaries,
multi-hour periods, permutation-independent scout selection, and exact recorded
clock pairs. `scripts/replay_interval_planner.cpp` replayed 377 valid observations
from the six running logs: 250 local + 127 Miner6, zero invalid plans. Of these,
97 first windows were already partially elapsed, correctly marked late. Replay
uses the experimental 30s period/5s phase/5s exchange and validates two successive
home windows per observation; it does not claim a multi-beacon hardware test.

Integration still to do, before deployment:
1. Keep the radio awake across merged appointments while tracking each logical
   exchange separately; preserve the delayed first ESP-NOW initialization.
2. Age evidence and advance reset qualification by logical rounds, not boots.
3. Add explicit per-exchange identity to packets/logs for multiple appointments
   in one boot, and adapt analysis accordingly.
4. Persist/update scout cursor, qualify fresh timing observations, replan on home
   changes, and handle missing timing without silently replacing home visits.
5. Test simulator execution and then a controlled six-board deployment.

No live scheduling behavior, firmware binary or running boards changed in this
planner-only slice. MAC normalization remains committed but not deployed.

MAC diagnostics follow-up: hardware protocol origin integers retain the
ESP.getEfuseMac() byte order for wire/persistence compatibility. The new
`protocolRadioMac()` conversion compares physical callbacks in network order;
CSIM already uses network-order context IDs and intentionally uses an identity
conversion. Summary logs retain `origin` and add canonical `origin-radio`,
directly comparable with `radio-from`. `radio-mismatch` now counts differing
addresses after normalization, not the representation reversal. Headerless
fallback peer identities are converted back to the protocol convention too.
No membership migration, wire revision, or hardware deployment is part of this
fix. Both hardware and simulator representations have host regression tests.

Next scheduler implementation slice: a host-tested interval planner separating
home/scout appointments from physical wake/sleep. Preserve home windows, rotate
scout candidates, merge overlapping/nearby windows, and sleep only in worthwhile
gaps. Integrating multiple appointments into the firmware must also move evidence
aging/reset qualification to logical rounds and add exchange sequence identity.
Preserve delayed ESP-NOW initialization and measure capture continuity during
hardware validation. Asymmetric reception is evidence for a radio-state
investigation, not yet proof of a hardware-specific cause.

## Review amendments and implementation boundary (2026-09-08)

These amendments supersede conflicting details in the original proposal below.

- All devices always use one fixed Wi-Fi channel. Membership, logical
  appointments, and power scheduling are separate concepts. Preserve every
  home appointment; add fair rotating scout appointments and merge overlapping
  or nearby radio-active intervals. Stay awake across merged appointments;
  never replace a home visit merely because a scout is nearby.
- Age evidence and evaluate the test harness by logical rounds/elapsed time,
  not physical boot count. Continuous-awake operation must still advance rounds.
- Keep paired full beacon TSF and local reception timestamps in diagnostics.
  Use a consistent local origin and signed differences for TSF projection.
- Preserve delayed ESP-NOW initialization. Jim reports that early initialization
  reduced or sometimes pathologically eliminated promiscuous monitor callbacks.
  This is an observed hardware workaround, not a proven explanation of the
  driver interaction. Preserve beacon-only acquisition before initialization;
  any change requires measuring capture continuity on hardware.
- Add reset-safe origin incarnation semantics; schema revisions are unrelated
  to freshness generations. Do not reject a newly reset origin indefinitely.
- Packet timing belongs once in the sender header where possible, not repeated
  for every claim. Planned ends and actual ends are distinct. Short deltas need
  checked bounds. Wire layout remains provisional pending implementation.
- Count successful merges separately from attempts/rejections, and collect
  raw per-source evidence before mux filtering. Normalize physical MAC format.
- Logger sessions are not board epochs. A logger restart alone does not reset
  firmware, and a firmware reset need not restart the logger.

First implementation slice: checked sequential uploads, fresh screen names,
removal of broken `--keep-screens`, timestamp-prefix parsing, capture-boundary
handling, UUID logger identities, and the initialization-workaround comment.
Host-only regression tests exercise parser equivalence and erase failure.
No board deployment or scheduler rewrite is included in this slice. Screen
liveness is reported separately from actual serial reception. Session/time
selection, port-owner checks, logger readiness handshake, TSF projection repair,
truthful firmware counters, and interval scheduling remain follow-up work.

Second implementation slice (2026-09-08): TSF projection now uses the paired
full TSF/boot-relative receive timestamp without modifying the observation.
Offsets before reception are subtracted with checked underflow/overflow;
invalid/missing observations do not emit a comparable exchange interval.
The observation log distinguishes scheduled exchange end from loop completion.
Intervals are half-open. Association merges return success, so per-peer
`association-refresh` counts only accepted direct-header updates. The new
`association-merge` summary counts all merge calls (including local publication
and relayed entries), with accepted totals and rejection reasons: invalid key,
older generation, not fresher, and table full. Counters reset each wake.
Relayed age increment saturates instead of wrapping at UINT32_MAX.
Generation/reset semantics and acceptance policy otherwise remain unchanged;
reset-safe incarnations are still pending. No packet layout, scheduler,
ESP-NOW initialization order, or board deployment changes in this slice.
Validation: all five host tests pass (including compiled production merge code
and TSF projection edge cases); ESP32 build passes. Live diagnostic validation
remains for the next coordinated deployment.

Third implementation slice (2026-09-08): compact sender timing in wire version
5, preserving the three-claim/four-association capacity. The old maximum was
25 + 3*21 + 4*22 + 4 = 180 bytes. The new header is 44 bytes, giving 199 bytes
including BRPT routing, below ESPNowMux's checked 200-byte limit (no splitting).
Fields are little-endian fixed-width integers; clock BSSID is six network-order
octets. The 19-byte addition is incarnation(4), BSSID(6), low TSF milliseconds(4),
signed start/planned-end deltas(2 each), and validity(1). Full BSSID is retained.
Offsets subtract separately floored full TSF milliseconds before truncating
the reference to 32 bits; this preserves wrap-boundary intervals. Overflow,
underflow, unobserved targets, and deltas beyond int16 invalidate timing rather
than clipping. These timestamps describe the sender's observed target and
scheduled window, not proof of radio delivery or actual final transmission.

Identity semantics: `(sender MAC, incarnation, wake generation, packet sequence)`
identifies a diagnostic sample. A nonzero random incarnation persists across
deep sleep and ordinary reset; test reset/flash erase or generation wrap starts
a new incarnation. Random 32-bit identity is probabilistic, not globally unique
or ordered. Current firmware has one exchange per wake. A future interval
scheduler needs a distinct exchange sequence once multiple exchanges per wake
are possible. Logger sessions do not change this identity.

This slice does NOT change claim/association acceptance to use incarnations:
relayed entries still lack them. Follow-up must propagate origin incarnation,
allow new direct-origin lifetimes despite lower generations, and prevent old
relays from rolling an origin back. Do not order random incarnation IDs or
treat a wire schema version as a freshness generation. The old reset-generation
rejection issue therefore remains pending, rather than being silently changed
as part of diagnostic instrumentation.

Receivers validate exact version/count/length framing before merging; malformed
lengths have a separate counter. Timing is diagnostic only. One receive timing
sample per peer per exchange and one transmit observation per exchange bound
new log output; packet fields are refreshed on every send. Missing first-sample
timing can be revisited with more detailed sampling later. The sender's full
paired TSF/local observation is logged without truncation.

Validation: six host tests pass, including real packed-header roundtrip/layout,
packet-size, truncated/extra-byte/count rejection, signed delta limits, invalid
observations, and 32-bit millisecond rollover. All six boards must move to this
wire version together at the later coordinated deployment; none changed here.
The final ESP32 build passes (RAM 89,084 bytes; flash 1,144,234 bytes).

Fourth slice (2026-09-08): wire version 6 adds a four-byte origin incarnation to
each relayed claim/association. Maximum capacity is now two claims and three
associations, rotating through the retained tables: 44 + 2*25 + 3*26 + 4 = 176
physical bytes. This reduces records per report; eventual gossip propagation
and convergence impact must be measured, not assumed equivalent to version 5.

A 32-origin registry retains the accepted incarnation across deep sleep.
Unknown origins may be introduced by gossip; only a direct report from that
origin can replace an established incarnation. Random IDs are compared for
equality only. Replacing an incarnation purges ALL old claims and associations
for that origin before applying its new, possibly lower generation. Differing
incarnations in relayed entries are rejected even if their generation is higher.
Received entries cannot modify this board's own evidence. Registry exhaustion
rejects new identities rather than evicting known origins and enabling rollback.
The epoch rejection counter is separate from association merge attempt counters.

Direct means the report sender header, not an entry forwarded within that
report. This is the existing trusted-farm protocol, not an authenticated/replay-
protected transport: an arbitrarily replayed OLD direct header can still switch
incarnations. The guarantee here is against stale relayed entries, not adversarial
sender spoofing or replay. No ordering is inferred between random incarnation IDs.

Persistence uses new /claims6, /associations6, and /origins6 keys; old keys are
left intact but not imported because they lack incarnation provenance. Startup
retains only records matching the persisted registry, so a mismatched partial
checkpoint is discarded instead of assigned the wrong incarnation. These writes
are not an atomic checkpoint: power loss can lose recent learned evidence.
Registry and tables reset together for the test reset. Ordinary sleep retains
the registry. Existing within-incarnation generation/age rules remain unchanged.

Validation: seven host tests pass, including production gating, merge and
persistence code exercised through reset-to-generation-1, numerically lower
new incarnation IDs, stale/unknown-epoch relays, unaffected peers, self-origin
protection, reload, inconsistent checkpoint records, and registry capacity.
ESP32 build passes (RAM 90,988; flash 1,146,418 bytes). No boards deployed.
Next hardware gate: upgrade all six together, verify report reception and timing,
then reset one board and confirm old relays do not prevent rejoin. Analyzer
correlation and interval scheduling remain later work.

Fifth slice: offline analyzer supports `--session latest` (default), `all`, or
an exact session ID, plus timezone-qualified inclusive `--since` / `--until`.
Both hosts use bounded tail reads (`--tail-bytes`); missing session beginnings,
legacy untimestamped selection, empty inputs and bounded history are reported.
`--evidence` reports complete/partial exchanges, reception, merge/rejection
counters, local incarnation changes, origin replacements, and latest state.
`--json` includes explicit raw-vs-byte-reversed MAC comparisons, avoiding the
mistake of treating the firmware's byte-order mismatch counter as relay proof.
`--overlaps` correlates only version-5-or-newer, complete same-BSSID scheduled
TSF windows, with a 15-second host-time proximity gate against reused clock
eras. Cross-BSSID clock reconstruction is still deferred. A positive overlap
does not prove actual radio activity; unmatched sampled packets remain unknown
unless the receiver has zero raw callbacks. Host clock alignment is assumed,
not measured by this tool. Compact low-ms packet timestamps are retained as
samples, not unwrapped into a fabricated common global clock.

Logger boundaries also stop the legacy convergence counter from stitching
separate captures together. The latest qualification display no longer claims
simultaneous six-board convergence from six independently latest records.

Example baseline review (read-only; no logger or board changes):

```sh
python3 scripts/analyze_rendezvous.py --evidence --overlaps
python3 scripts/analyze_rendezvous.py --session latest --since 2026-09-08T10:42:40-07:00 --json --overlaps
python3 scripts/analyze_rendezvous.py --recent 0
```

Validation: 15 host tests including session boundaries, truncated captures,
time filters, partial exchanges, TSF half-open intervals, different BSSIDs,
host-time separation, version gating, packet identity, binary log noise, and
cross-session convergence. Read-only six-board smoke test succeeded; no firmware
build/deployment is required for this analyzer-only slice. Full baseline review
remains separate from this early smoke test.

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
