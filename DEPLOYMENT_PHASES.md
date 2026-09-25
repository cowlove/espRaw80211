# Hardware test-period ledger

This ledger separates Git commit time from actual hardware deployment time.
Before the one-shot `@b git=...` record was added, boundaries are reconstructed
from deployment reports, cold-reset/epoch markers, logger rotations, and the
first post-deployment records; those boundaries are approximate.

## Current limitation

Older device logs before the Git-identity rollout cannot be unambiguously
labeled by Git commit. Newer firmware emits one `@b git=<commit>` record per
boot, but the supervisor log itself remains clock-only and spans multiple days.

## Phase ledger

| Phase | Commit | Git commit time (PDT) | Deployment evidence | Confidence |
|---|---|---:|---|---|
| Aggressive singleton scouting | `206758e` | Sep 9 08:46 | Explicit seven-board flash/hash verification | high |
| Cold-epoch staggering | `7d89ea9` | Sep 9 11:19 | Explicit seven-board deployment and logger restarts | high |
| 120-second cadence / stutter refinement | `890bea1` | Sep 10 11:12 | Explicit seven-board deployment; post-flash stutter records | high |
| Passive settling metrics | `1222f11` | Sep 10 11:28 | Explicit seven-board deployment; `@m` records confirmed | high |
| Proposal-delay cap 3 | `ebcd8be` | Sep 15 20:59 | Explicit seven-board deployment/hash verification | high |
| Proposal-delay cap 2 | `7562a9c` | Sep 17 12:34 | Explicit seven-board deployment/hash verification | high |
| Largest-known established scouting | `e6c1069` | Sep 19 11:38 | Explicit seven-board deployment/hash verification; later phase analyses used this boundary | high |
| Git identity marker, source only | `bd189b1` | Sep 19 17:09 | Tested and pushed; explicitly **not flashed** | certain: not deployed |
| Fresh-confirmation proposal commit | `9d7b4b6` | Sep 23 13:54 | Explicit seven-board deployment; all boards emitted `@b git=9d7b` | high |
| Eight-board cohort (adds Miner6 M2) | `01ca386` | Sep 25 13:05 | Five local plus three Miner6 uploads hash-verified; all emitted `@b git=01ca` | high |

Analyzer/docs/CSIM-only commits between these checkpoints are not firmware
treatment changes unless a deployment is explicitly recorded. Equal-size tie,
positive-partial-scout, and other behavior changes should be treated as
sub-phases only when their deployment records are available; do not infer them
from Git time alone.

## Filtering rules

1. Prefer `@b git=` markers for deployments at or after `9d7b4b6`.
2. For older phases, use deployment time plus the first subsequent
   `TEST EPOCH RESET`/cold-boot marker on each board. Exclude an ambiguous
   transition epoch caused by logger restart or flash reset.
3. Use supervisor reset waves only when every configured cohort board
   acknowledges: seven before `01ca386`, eight from `01ca386` onward. Exclude
   `epoch-invalid` epochs.
4. Keep F+/F−, time-of-day, and foreign-board presence as covariates; do not
   attribute their effects to firmware without overlap.
5. For the cap campaign, cap 3 runs from its deployment boundary to the cap-2
   boundary; cap 2 runs after that boundary. Remove invalid epochs.

## Existing phase results

- Cap 3: approximately 124 clean epochs, median about 893 s.
- Cap 2: approximately 103 clean epochs, median about 934.5 s; no defensible
  improvement over cap 3 was established.
- Largest-known targeting: repeated convergence/reset was verified, but an
  earlier count mixed clock-only supervisor history with older phases. Recompute
  it using this ledger and reset/boot evidence.

## Current data-quality boundary

The `01ca386` deployment is a deliberate new phase: the controlled cohort grew
from seven to eight boards and all eight logs contain the commit marker. Do not
compare its convergence distribution directly with seven-board phases without
treating cohort size as a changed condition.
