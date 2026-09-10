# ESPRaw80211 beacon rendezvous and unattended clustering experiment

This project is an ESP32 prototype for using nearby 802.11 beacon frames as a
shared timing reference, exchanging attributable ESP-NOW observations, and
testing whether a physically connected group can repeatedly rediscover one
another after a deliberate reset.

It has two purposes: protocol development, and an unattended six-board farm
that collects from-scratch resynchronization episodes. It is an experimental
test instrument, not a production low-power scheduler. See
[`TESTING_FARM.md`](TESTING_FARM.md) for the physical farm and reset harness.

## Current deployed behavior

Each wake broadly captures management frames on Wi-Fi channel 4, identifies
802.11 beacons by BSSID, chooses or retains a home beacon, aligns to its TSF,
and opens an ESP-NOW exchange window. Reports use the `BRPT` prefix and carry
the sender's selected beacon plus rotating attributable claims and association
records.

Current experimental defaults:

- nominal rendezvous goal: **120 seconds**;
- ESP-NOW report cadence while awake: **5 Hz**;
- claim freshness: **20 wakes**;
- selected-beacon association freshness: **6 wake cycles**;
- test consensus: **6 fresh listeners for 10 consecutive healthy cycles**;
- post-commit reset delay: **3 completed wake cycles**.

The old one-off experiment that multiplied the rendezvous goal by five after
50 repetitions has been removed. Persisted state can still contain a legacy
goal until initialization or a reset clears it, but this firmware will not
increase the goal again.

## Runtime phases

1. **Broad beacon survey:** collect visible BSSIDs, packet counts, RSSI, and
   timing continuity.
2. **Clock alignment:** use the selected beacon TSF and local receive timestamp
   to calculate the next rendezvous boundary.
3. **ESP-NOW exchange:** initialize the mux on the configured channel, send
   `BRPT` reports, receive peer reports, and merge claims/associations.
4. **Decision and persistence:** retain the home beacon on incomplete exchange;
   update proposals only from healthy evidence; advance the unattended test
   counter only when six fresh associations support the home beacon.
5. **Deep sleep/reboot:** sleep until the next beacon boundary, preserving
   protocol state in SPIFFS.

For executor sleeps longer than 60 seconds, this is a two-step sleep: wake
roughly 60 seconds before the exchange boundary, listen only until the first
direct target beacon, then immediately reproject and take the final sleep to
the normal acquisition lead. The stutter wake never initializes ESP-NOW. A
five-second no-target timeout clears the stutter intent and falls through to
ordinary acquisition/recovery.

## Exchange-window timing contract

The exchange window is referenced to a shared beacon clock.  The current
experimental phase is a fixed **5 seconds after each selected beacon's TSF
period rollover**.  Phase zero is not inherently preferable: what matters is
that peers using the same beacon calculate the same repeatable phase.

The executor must provide **coverage**, not millisecond-perfect radio gating:

1. Sleep scheduling wakes the board early enough for sleep-timer error and a
   beacon-only acquisition period (currently about 5.5 seconds of lead).
2. Promiscuous beacon capture continues until a bounded ESP-NOW warm-up lead
   before the first planned appointment (initially 500 ms).
3. ESP-NOW initialization starts at that warm-up point so the first appointment
   is covered even if initialization takes measurable time.
4. Once initialized, ESP-NOW may transmit and receive through the warm-up lead
   and throughout the active interval.  Planned boundaries are used for
   attribution, health, and report scheduling—not as fine-grained TX/RX gates.
5. Nearby or overlapping appointments are coalesced into one continuous active
   interval.  Every constituent appointment must be covered; later members of
   a merged interval do not require a separate radio restart at their boundary.

Compact timing records should retain the planned start, initialization begin/
completion, and first exchange activity so the warm-up margin can be measured
on hardware before shrinking acquisition or exchange windows.

### Window-settling measurements

Each completed normal exchange emits compact `@m` measurements for its beacon
capture (`k=b`) and ESP-NOW interval (`k=e`). `s` is the elapsed microseconds
to the last conservative state mutation; `d` is the elapsed time to the last
narrower current-decision mutation. `0` means that no qualifying mutation was
observed in that window. These are passive diagnostics: they do not shorten a
window or alter rendezvous/migration behavior. Analyze the per-cycle data and
its p50/p90 summaries with:

```sh
./scripts/analyze_rendezvous.py --evidence --session all
```

`s` is the safer tuning bound: it includes accepted evidence that could affect
a later decision. `d` is an optimistic bound for the current decision only;
exact counterfactual equivalence would require packet-trace replay.

## Reading logs

Important `gossip` fields are:

- `healthy`/`incomplete`: local transport health for protocol decisions;
- `home`: BSSID used for that wake;
- `listeners`: fresh association origins selecting that home beacon. This is a
  total count, includes the board itself, and is not a direct RF packet count;
- `espnow tx/ok/fail/busy`: local send attempts and driver results;
- `rawrx`: frames delivered to `ESPNowMux::onRecv()`;
- `rx`: frames matching `BRPT` and reaching `onReport()`;
- `valid`: version-4 reports accepted by the clustering protocol;
- `peers`: distinct senders with valid reports;
- `exchange-usec`: beacon-relative exchange window.

Per-peer `raw` and `valid` lines distinguish direct packet reception from
relayed association evidence. A receiver can hear other boards while USB0 is
absent, which points toward a USB0-specific source/radio path rather than a
generally inactive receiver.

The analyzer lists retained complete cycles oldest-first within the selected
window. For a current no-history snapshot:

```sh
python3 scripts/analyze_rendezvous.py --log-dir . --recent 0
```

Use positive `--recent` values for timing/history and `--convergence` for
reset-to-10/10 episodes and current reset attempts.

## Build and deployment

```sh
make BOARD=esp32
```

Retain the persistent `build/` directory; do not run `make clean` during
ordinary iteration. For local boards exposed as `/dev/ttyUSB0` through
`/dev/ttyUSB3`:

```sh
python3 scripts/deploy_usb_screens.py
```

This builds once, terminates matching `*.usbN` screen sessions, uploads each
selected `/dev/ttyUSB<N>`, and creates fresh detached screen sessions whose
append-mode logger starts only after a successful upload. Use `--boards 0,1,2,3`
to select explicit ports and `--erase-flash` for a destructive clean start.
Uploads run in parallel after one shared build, with one worker per selected
board by default. Each board is independently hash-verified and gets a fresh
logger only after its successful upload. Use `--jobs 1` for sequential
troubleshooting or `--jobs N` to bound USB/CPU concurrency. Any failures are
collected and reported after the other in-flight uploads finish.
The unsupported `--keep-screens` option has been removed. Logs append
to `cat.usbN.out`; each line is prefixed by `scripts/timestamp_serial.py` with
host wall time, monotonic nanoseconds, board, port, and logger session ID while
preserving the original firmware text. Miner6 is a separate host with two boards.
When a committed tree is available, sync it with `git push`/`git pull` before
building there. Direct binary transfer is a fallback when the remote build
environment cannot build safely.

## Limitations

For empirical CSIM experiments, `--reception-scale VALUE` multiplies both the
directed healthy-window probability and per-packet delivery probability. The
default is `0.60`; values above `1.0` improve reception and resulting
probabilities are capped at 100%. CSIM prints the effective value at startup.

```sh
make BOARD=csim espRaw80211_csim
make BOARD=csim clear-state
./espRaw80211_csim --seconds 7200 --reception-scale 0.8
```

CSIM emits one process-wide marker whenever all simulated boards transition
onto the same nonzero home beacon, and a matching marker when that topology
diverges again:

```text
CSIM GLOBAL CONVERGENCE count=1 time=345.136 beacon=60a4b792e686
CSIM GLOBAL DIVERGENCE count=1 time=824.594
```

This makes long-run convergence throughput directly countable without parsing
the per-board trace:

```sh
./espRaw80211_csim --seconds 18000 --reception-scale 0.60 > csim-5h.log
grep -c 'CSIM GLOBAL CONVERGENCE' csim-5h.log
```

Use the runner for concurrent or independent experiments. It gives every run
private simulated SPIFFS, RTC, and sleep-checkpoint state while forwarding all
arguments to the same CSIM executable:

```sh
./scripts/run_csim.sh --seconds 18000 --reception-scale 0.60 > csim-5h.log
```

Use `--random-seed N` for repeatable but distinct experiment trajectories.
CSIM advances the deterministic seed across its internal deep-sleep process
re-execs, so `rand()` does not restart from the same point after every wake:

```sh
./scripts/run_csim.sh --seconds 18000 --reception-scale 0.60 \
  --random-seed 17 > seed-17.log
```

Use `--singleton-scout-aggressiveness P`, where `P` is from 0 through 1, to
control the independent probability that a singleton includes each eligible
non-home beacon in its plan. The default 1 visits all eligible candidates;
zero disables singleton scouts. Established groups retain their conservative
one-target fair rotation. Aggressive singleton wakes are forced through deep
sleep after at most 60 seconds so the next wake starts with clean beacon-only
acquisition.

For migration-policy experiments, stop at the first global convergence so the
10/10 qualification and reset delay do not dominate the metric:

```sh
./scripts/run_csim.sh --seconds 3600 --reception-scale 0.60 \
  --random-seed 17 --exit-on-convergence
```

The convergence marker's `time=` field is the clean-start convergence latency.
No marker means that seed timed out without converging.

Run the benchmark in parallel using all available processors. It defaults to
200 seeds, 3,600 simulated seconds, and reception scale 0.60. The benchmark
parses and reports overrides for those values. All other arguments are appended
verbatim to the generated CSIM command line:

```sh
./scripts/csim-benchmark.sh
./scripts/csim-benchmark.sh --iterations 1000
./scripts/csim-benchmark.sh --iterations 200 --reception-scale 0.40
```

It reports the convergence success/timeout counts and the minimum, median,
mean, p95, and maximum first-convergence times.

Firmware output defaults to compact, line-oriented ASCII diagnostics. Human
status and decision records remain descriptive; repeated exchange details use
short `@` records (`@i` identity, `@e` exchange boundary, `@d` appointment,
`@b` beacon clock, `@r`/`@t` radio clock, `@l` plan, `@x` counters, `@p` peer,
`@a` association summary, and `@q` end-of-wake beacon scan). Each `@q` record
contains BSSID `b`, average RSSI `r`, current-wake packet count `n`, age of the
last observation in microseconds `a`, and the beacon TSF clock `t`. Age is
diagnostic only: beacon eligibility uses RSSI and packet count observed during
the current wake, with no elapsed-time freshness cutoff. Historical
verbose logs remain supported by the analyzer. Verify both encodings produce
identical analysis with:

```sh
./scripts/test_csim_log_compatibility.sh
```

- Only one Wi-Fi channel is monitored.
- A common beacon may not be physically visible to every board; a stable
  logical partition is then valid behavior.
- ESP-NOW send success is not end-to-end delivery acknowledgment.
- `rawrx == 0` places loss below the application protocol, but cannot alone
  distinguish RF loss, driver filtering, channel state, or callback setup.
- Persisted SPIFFS state can make a flashed board resume old timing or
  association state unless the experiment deliberately erases or resets it.
- The unattended reset loop is a measurement harness, not production proof.
