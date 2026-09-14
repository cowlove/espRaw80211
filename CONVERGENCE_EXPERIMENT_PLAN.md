# Convergence Delay: Hardware Evidence and CSIM Plan

## Purpose

The seven-board farm has shown that most clean-reset epochs quickly form a
large group, but the final merge—often a 4/3 split—dominates convergence time
and variability.  This note defines how to investigate and reduce that tail
without changing live rendezvous behavior prematurely.

Reliability remains the acceptance criterion.  Reduced awake time is a later,
separate optimization.

## What the current farm data says

With the 120-second rendezvous cadence, the observed clean epochs show:

- median reset to first 4+ member group: about 270 seconds;
- median first 4+ group to 7/7 consensus: about 480 seconds;
- the 4+ to 7/7 portion is typically most of the total convergence interval.

The existing logs record the deployed policy's relevant wake-boundary state:

- complete claim and association matrices;
- scan observations and remote beacon statistics;
- direct scout and visitor evidence, including estimated member counts;
- home credibility;
- proposal creation, refresh, cancellation, commit, and rejection reason;
- appointment completeness and exchange health.

They do **not** retain every received ESP-NOW payload or its exact arrival
order.  Therefore they cannot prove the later network trajectory of an
alternative policy after it changes a board's home beacon.

## Two complementary evaluation modes

### 1. Hardware snapshot replay

At the end of each wake/exchange window, replay alternate *local decision
rules* against the exact state observed by the deployed firmware.

Examples:

- different group-size or BSSID ordering;
- a different credibility-to-confirmation-delay mapping;
- different requirements for direct scout versus visitor evidence;
- proposal replacement/cancellation rules.

This answers: *would this board have made a different decision at this observed
wake boundary?*  It exposes which 4/3 tails are caused by lack of qualifying
evidence versus intentional policy delay.

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

At each decision boundary, emit a compact, parseable snapshot containing:

- current home and estimated home membership;
- directly observed candidate beacon(s) and estimated membership;
- evidence source (`scout` or `visitor`) and full/healthy status;
- credibility and pending proposal state;
- selected action and rejection/cancellation reason.

These records should be sufficient to implement hardware snapshot replay and
remain compact enough for continuous logging.  They should describe semantic
state, not raw radio frames.

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

## Decision sequence

1. Implement analyzer reconstruction of existing 4+ tails.
2. Add always-on compact decision snapshots and validate their log cost.
3. Use historical and new snapshots to rank candidate policy changes.
4. Implement the best candidates in CSIM and run multi-seed full-feedback
   comparisons.
5. Deploy one conservative candidate to the farm; evaluate convergence,
   recovery, and stability before changing another parameter.

No exchange or beacon window shortening is part of this work.
