# Convergence Delay: Hardware Evidence and CSIM Plan

## Purpose

The seven-board farm has shown that most clean-reset epochs quickly form a
large group, but the final merge—often a 4/3 split—dominates convergence time
and variability.  This note defines how to investigate and reduce that tail
without changing live rendezvous behavior prematurely.

Reliability remains the acceptance criterion.  Reduced awake time is a later,
separate optimization.

## Proposal-delay cap campaign

The current hardware experiment compares the production proposal-delay cap
against the previous settings:

- **Cap 5:** original production behavior;
- **Cap 3:** deployed as `ebcd8be`;
- **Cap 2:** deployed as `7562a9c`.

The cap controls the maximum credibility-based proposal activation delay.  It
does not change singleton coalescence, beacon scanning, exchange duration, or
the distributed 10/10 reset mechanism.

### Why we tested it

CSIM's ordinary clean-start workload showed little difference between caps,
because most runs do not reach a mature proposal on the critical path.  A
deliberately mature 4/3 fixture did exercise the cap and showed a strong
effect:

| cap | median release-to-7/7 | p95 | success |
|---:|---:|---:|---:|
| 5 | 729.7s | 962.4s | 100/100 |
| 3 | 489.6s | 714.7s | 100/100 |
| 2 | 369.6s | 609.6s | 100/100 |

The fixture is useful evidence that credibility delay can dominate a mature
large-group merge, but it is intentionally controlled and cannot predict the
full hardware trajectory by itself.

### Hardware evidence so far

The cap-3 hardware phase produced approximately **124 clean epochs** (three
invalid reset attempts excluded), with median convergence around **893s**.
The cap-2 phase subsequently reached approximately **103 clean epochs** (one
invalid reset attempt excluded), with median around **935s**.  These samples
have similar broad, skewed distributions and do not show a statistically
credible cap-2 improvement; the roughly 42-second median difference is well
within uncertainty and environmental variation.

The current interpretation is deliberately conservative:

- cap 3 operated reliably but did not produce a clear hardware speedup;
- cap 2 is also operating reliably, but has not yet demonstrated a speedup;
- the hardware comparison is observational rather than randomized, with
  time-of-day, RF activity, foreign-board presence, and reset epochs as
  possible confounders;
- invalid coordinated resets remain visible in the logs but are excluded from
  primary convergence statistics;
- the large 4/3-or-larger merge tail remains the dominant source of variance.

The cap campaign should therefore be treated as a reliability/performance
experiment, not as evidence that a CSIM improvement transfers automatically
to hardware.  Keep the prior cap commits available for rollback, and compare
the phases using the same `since-reset` metric with F+/F− and time-of-day
stratification when possible.  Do not change another protocol parameter until
the current cap-2 sample is complete enough for a stable comparison.

## What the current farm data says

With the 120-second rendezvous cadence, the observed clean epochs show:

- median reset to first 4+ member group: about 270 seconds;
- median first 4+ group to 7/7 consensus: about 480 seconds;
- the 4+ to 7/7 portion is typically most of the total convergence interval.

The existing compact hardware logs record much of the deployed policy's
decision evidence and outcome:

- scan observations and remote beacon statistics;
- direct scout and visitor evidence, including estimated member counts;
- home credibility;
- proposal creation, refresh, cancellation, commit, and rejection reason;
- appointment completeness and exchange health.

They do **not** currently record complete claim and association matrices.
Those row-level dumps exist in legacy CSIM diagnostics, while normal hardware
logging emits an association summary.  Historical hardware logs are therefore
useful for attributing the deployed policy's delays, but are not sufficient for
general snapshot replay without additional records.

They do **not** retain every received ESP-NOW payload or its exact arrival
order.  Therefore they cannot prove the later network trajectory of an
alternative policy after it changes a board's home beacon.

## Two complementary evaluation modes

### 1. Hardware snapshot replay

At each appointment completion, replay alternate *local decision rules*
against the exact pre-decision state observed by the deployed firmware.  This
is the correct boundary: one awake interval can contain several appointments,
and migration evaluation occurs as each home or scout appointment completes,
not only at the end of the whole interval.

Examples:

- different group-size or BSSID ordering;
- a different credibility-to-confirmation-delay mapping;
- different requirements for direct scout versus visitor evidence;
- proposal replacement/cancellation rules.

This answers: *would this board have made a different decision at this
observed appointment boundary?*  It exposes which 4/3 tails are caused by lack
of qualifying evidence versus intentional policy delay.

It does not answer: *what reception opportunities would exist after all of
those different decisions feed back into later schedules?*

### 2. CSIM full-feedback trials

Use CSIM to test the subset of policy changes that look promising in snapshot
replay.  CSIM changes homes, schedules future appointments, models directed
links, and therefore captures the feedback unavailable from a fixed hardware
trace.

For each candidate, run many deterministic seeds against repeatable initial
topologies, at minimum:

- 4/3 equal-or-near-equal split;
- 3/2/2 split;
- 3/2/1/1 split;
- reset/rejoin while a large group exists.

Report convergence probability, reset-to-first-7/7 latency distribution,
4+→7/7 tail distribution, migration count, and post-convergence stability.
Compare against the current policy using identical seeds and measured beacon/
ESP-NOW environment data.

Only candidates that improve the tail without materially reducing success rate
or stability should advance to the hardware farm.

## Logging additions

### Always-on compact decision snapshots

At each appointment decision boundary, capture a compact, parseable snapshot
containing:

- incarnation, logical round, exchange sequence, appointment index and kind;
- current home and estimated home membership;
- directly observed candidate beacon(s) and estimated membership;
- evidence source (`scout` or `visitor`) and full/healthy status;
- credibility and pending proposal state;
- selected action and rejection/cancellation reason.

The snapshot must describe the state *before* the action and the deployed
action separately.  Association rows used to calculate group estimates should
be emitted once per exchange under a snapshot ID; appointment records can
reference that ID instead of duplicating the table.  Include a stable state
fingerprint so missing/truncated rows are detectable.

Capture records into a small fixed-size RAM buffer and emit them only after the
radio interval ends.  Serial formatting during an appointment would perturb
the timing being measured.  The number of planned appointments is bounded, so
the buffer can be bounded too; an overflow marker is mandatory.

These records should be sufficient to replay the current group-size,
tie-break, evidence, credibility, and proposal policies while remaining
compact enough for continuous logging.  They should describe semantic state,
not raw radio frames.  More experimental policies that depend on information
not represented in the snapshot must be declared unsupported rather than
silently approximated.

### Sampled semantic input traces

For a small, configurable fraction of wakes (initially about 1 in 50), retain
a bounded, ordered trace of decoded decision inputs:

- beacon BSSID, RSSI, TSF, and local arrival offset;
- accepted ESP-NOW claim/association updates, origin, radio sender, and local
  arrival offset;
- trace overflow indicator and record count.

Emit it after the window as compact records.  Do not continuously print every
promiscuous packet: serial volume and logging work must not perturb the timing
experiment.  A bounded sampled trace gives replay-quality ground truth for
selected wakes while the always-on snapshots cover all wakes.

Sampled semantic tracing is a second phase, not a prerequisite for the first
group-migration experiments.  Begin with appointment snapshots, measure their
UART/storage cost, and add sampled inputs only if the snapshots leave a
specific ambiguity.

## Replay implementation

Avoid reimplementing migration policy independently in Python.  Extract or
retain the decision rules as pure production C++ functions, and build a small
host replay executable that consumes normalized snapshots and invokes those
same functions.  The Python analyzer should reconstruct snapshots, select
cohorts, and summarize results; the production policy remains the decision
oracle.

Before evaluating alternatives, replay the deployed policy and require it to
reproduce the logged action and reason.  Any mismatch indicates an incomplete
snapshot, parser error, or policy/version mismatch and excludes that record
from counterfactual analysis.

Counterfactual hardware replay is valid only until the first alternative action
diverges from the deployed action.  Later physical observations came from the
deployed trajectory and must not be presented as the alternate policy's future.
CSIM is responsible for following that changed trajectory.

## Analyzer work

Add a 4+ convergence-tail report that, per clean epoch, identifies:

1. first complete 4+ topology;
2. first qualifying cross-group direct evidence;
3. proposal start/refresh/commit/cancel timestamps;
4. time spent with no qualifying evidence, awaiting confirmation, or rejected;
5. final 7/7 consensus.

The analyzer should preserve raw rows and provide distributions by board,
initial topology, BSSID pair, appointment source, and F+/F− foreign-board
classification.  A snapshot-replay mode can then report how often an
alternative policy would disagree with the deployed action.

The first historical-log report requires no firmware change.  It should
attribute each observed 4+→7/7 tail among:

- no qualifying cross-group opportunity yet;
- qualifying evidence rejected by group ordering or freshness;
- proposal/credibility confirmation in progress;
- proposal invalidation, cancellation, or replacement;
- migration completed but consensus not yet visible.

## Decision sequence

1. Implement analyzer reconstruction of existing 4+ tails using the evidence
   already logged.
2. Define and test a versioned appointment-snapshot schema; make CSIM emit it
   first and prove replay of the deployed action.
3. Add buffered always-on snapshots to hardware and validate completeness,
   timing impact, UART volume, and truncation handling.
4. Use new hardware snapshots to rank candidate policy changes.
5. Implement the best candidates in CSIM and run paired, multi-seed
   full-feedback comparisons.
6. Deploy one conservative candidate to the farm; evaluate convergence,
   recovery, and stability before changing another parameter.

No exchange or beacon window shortening is part of this work.

## Implementation slices

Keep each slice independently testable and deployable:

### Slice A: host decision model (complete)

`appointmentDecisionSnapshot.h` defines the versioned pre-decision state,
bounded appointment buffer, ordering rule, and replay outcome.  Its host test
exercises the current singleton, ordering, proposal, cancellation, activation,
schema-version, incomplete-row, and overflow branches.  This header is not yet
wired into firmware and therefore cannot change live behavior.

### Slice B: CSIM snapshot proof (complete)

Instrument CSIM appointment boundaries, emit association rows plus decision
snapshots, parse them, and require replay to reproduce every deployed action.
Include multiple appointments in one interval and deliberately truncated trace
fixtures.  This proves the schema before adding hardware UART traffic.

CSIM now emits `@v` decision records and keyed `@vrow` association rows.  A
runtime assertion compares the replayed outcome with the branch actually
taken, while `scripts/decision_snapshots.py` independently verifies schema
version, row count, association fingerprint, and action equality.  The compact
versus legacy compatibility test validates both forms on every run.

### Slice C: historical tail attribution (first report complete)

Use existing logs to classify 4+→7/7 wall time under the deployed policy.  This
can proceed independently of new firmware, but must label unavailable evidence
as unknown rather than infer a complete association table.

The first analyzer view is available as:

```sh
./scripts/analyze_rendezvous.py --merge-attribution --session all --tail-bytes 5m
```

For each complete cold-reset epoch it reports the ordered observer-visible
chain from the first complete 4+ (but not 7/7) topology through first received
cross-group evidence, first new proposal, first later commit, and 7/7
consensus.  It also counts migration rejections by reason.  Missing stages
remain explicit.  Because these are first events across independent boards,
the report does not claim that they form one causal proposal chain, and a
proposal that began before the observer's first 4+ snapshot is not invented.

The second analyzer view uses the deployed appointment snapshots:

```sh
./scripts/analyze_rendezvous.py --merge-decisions --session all --tail-bytes 5m
```

It classifies every local decision inside an observed 4+→7/7 tail as target
smaller, equal-size/higher-BSSID, weaker than an existing pending target,
credibility wait, proposal, refresh, commit, or cancellation.  It also compares
the device's local membership counts with the nearest complete observer
topology.  Such an observer disagreement is evidence of an asynchronous or
incomplete local view, not proof that the firmware made an incorrect decision.
Replay mismatches are counted explicitly and exclude a record from
counterfactual use.

In an initial recent 23-episode sample, 451 snapshot decisions contained 223
not-preferred rejections (206 target-smaller and 18 equal-size/higher-BSSID),
60 credibility waits, 59 proposal starts, and 39 commits.  Visitor evidence
accounted for 283 decisions versus 61 direct-scout decisions.  Forty-one local
preference results disagreed with the nearest complete observer topology.  The
first locally preferred target appeared a median 46 seconds after the first
observer-visible 4+ topology (p90 166 seconds).  This points toward local
membership-view quality and visitor-driven opportunity as higher-value next
targets than further reducing proposal activation delay.

A one-step membership-freshness counterfactual is available as:

```sh
./scripts/analyze_rendezvous.py --membership-counterfactual \
  --membership-ttls 8,10,12 --session all --tail-bytes 5m
```

It reconstructs the deployed six-wake home and target counts from every full
snapshot row set before considering that record.  It then asks whether longer
association freshness would change the local group-ordering result.  This is
only first-decision screening; changed future observations still require CSIM.

In the initial 344-snapshot hardware sample, all deployed six-wake counts
reconstructed exactly.  Freshness limits of 8, 10, and 12 wakes rescued zero
rejected targets.  Instead they produced 2, 3, and 4 preferred→rejected
reversals respectively, each moving away from the nearest complete observer
topology.  Therefore simply retaining association rows longer is not a useful
solution to the large-group merge tail.  The next experiment should improve
established-group scout opportunity/target selection rather than increase
membership TTL.

The first CSIM scout experiment adds two simulation-only controls:

```sh
--established-scout-interval-wakes 1
--established-scout-largest-known
```

The first changes established groups from one scout every two logical wakes to
one every wake.  The second retains the production cadence but chooses the
eligible rival with the largest locally reconstructed membership, using lower
BSSID as the equal-size tie-break, instead of fair rotation.  Production
hardware remains fair/every-two-wakes.

In 300 ordinary paired seeds at reception scale 0.60:

- baseline: 300/300, median 1005s, p95 1799s;
- every-wake fair scouting: 300/300, median 825s, p95 1537s;
- largest-known targeting: 300/300, median 825s, p95 1424s;
- both changes: 299/300, median 705s, p95 1197s.

The combined policy is not a candidate: seed 30 remained in a persistent 4/3
split past the 3600-second limit, despite each individual treatment converging
that seed.  The interaction repeatedly refreshed proposals and demonstrates
that independently safe scout changes cannot be assumed safe when combined.

Planned-awake accounting over 30 ordinary seeds also favors target selection.
Largest-known targeting left awake time per plan effectively unchanged
(12.68s versus 12.77s baseline) and reduced total planned awake time per seed
from 940s to 779s because convergence required fewer plans.  Every-wake
scouting raised awake time per plan to 14.24s, although faster convergence
reduced total planned awake time slightly to 896s.  Therefore the isolated
largest-known selector is the strongest next hardware candidate; cadence
acceleration should remain a separate later experiment.

Largest-known selection was subsequently promoted to the production default,
while the established-group cadence remains one scout every two logical wakes.
CSIM now matches that production default.  Use `--established-scout-fair` to
reproduce the previous fair-rotation baseline; do not combine largest-known
selection with every-wake scouting unless the seed-30 stall is understood.

### Slice D: passive hardware snapshots (deployed)

Wire the proven capture path into firmware using a fixed-size RAM buffer and
emit only after the radio interval.  First deploy with no policy changes.
Compare timing, log volume, replay completeness, and convergence statistics
against the pre-deployment baseline.

For hardware, avoid CSIM's deliberately verbose row duplication: capture each
association table version once per interval, assign it a snapshot ID, and let
each buffered appointment decision reference that ID.  Emit all records after
ESP-NOW stops, followed by an explicit end/count record.  The host parser must
reject an incomplete set rather than treating it as replayable evidence.

The implementation uses eight table-version slots and 40 decision slots per
interval.  Three five-hour CSIM runs observed at most two of each.  Overflow is
non-fatal to rendezvous behavior but makes that trace explicitly invalid.  The
fixed buffers add about 11.6 KiB RAM; no trace formatting occurs until the
exchange interval has ended.  The parser also handles intervals that cross a
logical-round boundary without treating their differing wake generations as
separate traces.

The first seven-board deployment produced complete trace boundaries on every
board.  Initial decision-bearing intervals replayed exactly with valid table
counts and fingerprints; zero-decision intervals emitted explicit complete
end records as designed.

### Slice E: counterfactual and full-feedback experiments

Rank one-step policy disagreements from hardware snapshots, then implement the
promising candidates in CSIM and run paired seeds.  Only a candidate that
preserves convergence probability and stability advances to hardware.

### Optional slice F: sampled semantic inputs

Add bounded sampled input traces only if appointment snapshots expose a
specific unresolved ambiguity.  They are not required for the first 4/3 policy
experiments.
