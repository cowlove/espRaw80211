# Deterministic rendezvous selection: two-stage experiment

## Goal and constraints

Reduce convergence delay by giving independently informed devices a shared
preference over rendezvous beacons. This is a zero-knowledge scheme: no fixed
AP list, designated leader, global membership count, or site-specific BSSID
constellation may participate in protocol decisions. The real deployment
targets small swarms, normally fewer than ten devices.

Deliberately fragmented startup remains an essential worst-case test, not a
defect to remove from the benchmark. Hardware is unchanged by this proposal.

## Motivation

The existing policy favors the largest locally estimated group, with lower
BSSID breaking equal-count ties. Devices use the same comparator but often
have different, aging membership estimates. They can therefore disagree on
the preferred destination. At a 120-second rendezvous cadence, successive
encounters, confirmation delays, and individual migrations cost minutes.

Stable BSSID ordering removes uncertainty from *ranking*; it cannot remove
differences in *eligibility*. The key distinction is shared ordering, not
shared knowledge. Numeric ascending BSSID is the initial ranking: lower is
better. A fixed hash could later remove address-prefix bias, but is not needed
for this experiment and must not be tuned to the measured farm.

## Stage 1: canonical initial candidate

Each device scans normally, filters by the existing local beacon eligibility
checks, then chooses the smallest BSSID among **all** eligible candidates.
RSSI and packet count establish usability; they do not order eligible choices.
In particular, do not restrict canonical selection to a locally RSSI-ranked
top-six subset, which would reintroduce unnecessary differences between peers.

Implementation: CSIM-only `--canonical-startup`. Without the flag, the existing
random-top-six startup and benchmark defaults remain unchanged. The canonical
branch uses the same existing startup eligibility (nonzero BSSID, minimum RSSI
and packet count); the existing no-candidate fallback is unchanged. It consumes
the usual startup RNG draw in both arms, avoiding an unnecessary random-stream
shift at that decision. Later trajectories may still consume different draws.

Unchanged: 60–120-second cold-start staggering, scan/sleep scheduling, scout
policy, count-first migration rules, proposal delay, wire format, and strongest
beacon timing-recovery fallback. This experiment changes only initial choice,
not continuous home selection or reconciliation.

Hypothesis: devices with overlapping observations will often independently
form larger groups, reducing later merge work. Overlap alone does not guarantee
the same minimum: a private lower-ranked candidate can still split the swarm.

Compare identical seed ranges, horizon and RF scale under:

```sh
make BOARD=csim espRaw80211_csim
./scripts/csim-benchmark.sh
./scripts/csim-benchmark.sh --canonical-startup
./scripts/csim-stress.sh --devices 8 --iterations 300 --seconds 3600
./scripts/csim-stress.sh --devices 8 --iterations 300 --seconds 3600 --canonical-startup
```

Seven devices use the original measured RF model; eight use its synthetic
expansion, not measurements from the newly enlarged hardware farm. Report
timeouts alongside success-only latency statistics. A startup win alone does
not demonstrate better recovery from an existing partition.

The CSIM convergence oracle checks persisted home-BSSID agreement across all
contexts, not completed peer discovery, healthy exchanges, or sustained
connectivity. Canonical startup can satisfy agreement before every device has
heard its peers; report it as a selection-convergence result, not proof of
operational rendezvous completion.

At the experiment's parent revision `5724202`, CSIM defaults to established
scouting every **two** wakes, while hardware uses every wake. Preserve those
defaults here and label the cohorts explicitly. To match deployed cadence, add
`--established-scout-interval-wakes 1` to **both** arms. The original default
seven-device benchmark remains a separate reproducibility regression.

## Stage 2: deterministic reconciliation (not implemented)

Keep deliberately fragmented initial homes, but investigate making stable
BSSID rank the primary destination preference instead of estimated group size.
Scout promising eligible candidates; retain direct usability confirmation.
Initially hold confirmation delay and radio budgets fixed to isolate ranking.
Advertising candidates and maintaining temporary old-home contact are design
options, not implemented requirements or proven solutions.

With fixed eligibility and exclusively downward-ranked moves, a device cannot
oscillate between homes. This does **not** prove global convergence: different
local minima can persist, and failure recovery can require upward moves.

Before promotion, specify candidate expiration, failed-target backoff, continued
exploration, and how peers reconcile a candidate unavailable to some members.
Do not interpret silence as a reliable veto or require acknowledgments from an
unknown global membership set. A connected peer graph does not imply that a
single beacon is usable by everyone. If no common usable beacon exists, the
one-BSSID objective requires a different model, such as bridging/multiple homes.

## Acceptance and experiment discipline

- Preserve the default seven-device/300-seed fragmented-start regression.
- Run an eight-device comparison separately; never pool its synthetic topology
  with the measured seven-device model.
- Test unequal visibility (including private preferred beacons), disappearing
  beacons, late arrivals, and persistent foreign peers before hardware promotion.
- Assess failures, latency, home-switch churn, and awake time. The initial
  benchmark measures first convergence only; stability/energy need follow-up.
- Keep stage 1 and stage 2 independently selectable. Do not change hardware,
  proposal delays, or membership freshness as part of stage 1.

## Results

### Hardware smoke trial authorized 2026-09-26

Stage 1 is now enabled for hardware through
`ARTIFICIAL_TEST_CANONICAL_STARTUP=1` in `testSwarmConfig.h`. Setting it to zero
and rebuilding restores randomized top-six startup. CSIM still defaults to
randomized startup and requires `--canonical-startup` for the treatment.
Both environments execute the same selector. Earlier CSIM-only statements
above describe the initial experiment, before this hardware promotion.

Deploy to L0–L4 and M0–M2 only; leave the foreign ESP32-S3 unchanged. Keep the
60–120-second stagger, eight-board oracle, every-wake scouting, proposal delay,
and merge policy unchanged. Exclude the flashing transition epoch. Inspect
`startup-canonical` and `@b git=` records from every board, then compare clean
8/8 supervisor epochs, stability through sustain, and timing-recovery activity.
The prior hardware rollback reference is `01ca386`. Stage 2 remains unimplemented.

### CSIM comparisons

2026-09-26: seeds 1–300 in each arm, RF scale 0.60, 3,600-second horizon.
Latency statistics include successes only. Cold-start stagger is included.

| Fleet / scout interval | Startup | Converged | Median seconds | p95 seconds |
|---|---|---:|---:|---:|
| 7 / 2 wakes (unchanged default) | Random top six | 300/300 | 713.936 | 1293.467 |
| 7 / 2 wakes | Canonical | 300/300 | 225.582 | 225.582 |
| 8 / 2 wakes (synthetic RF) | Random top six | 294/300 | 717.089 | 1076.377 |
| 8 / 2 wakes | Canonical | 300/300 | 225.583 | 225.583 |
| 7 / 1 wake (deployed cadence) | Random top six | 300/300 | 588.865 | 933.525 |
| 7 / 1 wake | Canonical | 300/300 | 225.582 | 225.582 |

At the deployed every-wake cadence, canonical startup reduces the median by
61.7% and p95 by 75.8%, with 300/300 in both arms. This also explains why the
default CSIM baseline and previously reported every-wake results differed:
they used different scout intervals, not conflicting measurements of one arm.

Default seven-device baseline exactly reproduces the prior 713.936-second
median. Canonical startup lowers that median by about 68%. In the inspected
seed 30, all seven contexts independently selected `60a4b792da8a` from nine
eligible candidates, despite staggered starts; first home agreement was at
225.582 seconds. That address was discovered, not configured. A 3,600-second
non-exiting run also exercised distributed test resets, so later convergence
markers must not be mistaken for independent clean-start samples.

The tight canonical latency distribution reflects common initial selection and
appointment timing in this fixed beacon environment. It is not evidence of a
universal bound. No disappearing-beacon or private-preferred-beacon fixtures
were added in stage 1; robustness to those remains open. Compact/legacy log
analysis equivalence and decision-snapshot replay passed on the default path.
