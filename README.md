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

- nominal rendezvous goal: **30 seconds**;
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

For migration-policy experiments, stop at the first global convergence so the
10/10 qualification and reset delay do not dominate the metric:

```sh
./scripts/run_csim.sh --seconds 3600 --reception-scale 0.60 \
  --random-seed 17 --exit-on-convergence
```

The convergence marker's `time=` field is the clean-start convergence latency.
No marker means that seed timed out without converging.

Run the fixed 200-seed benchmark in parallel using all available processors:

```sh
./scripts/csim-benchmark.sh
```

It reports the convergence success/timeout counts and the minimum, median,
mean, p95, and maximum first-convergence times.

Firmware output defaults to compact, line-oriented ASCII diagnostics. Human
status and decision records remain descriptive; repeated exchange details use
short `@` records (`@i` identity, `@e` exchange boundary, `@d` appointment,
`@b` beacon clock, `@r`/`@t` radio clock, `@l` plan, `@x` counters, `@p` peer,
`@a` association summary, and `@q` end-of-wake beacon scan). Each `@q` record
contains BSSID `b`, average RSSI `r`, current-wake packet count `n`, age of the
last observation in microseconds `a`, and the beacon TSF clock `t`. Historical
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
