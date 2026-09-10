# Physical Testing Farm and Unattended Resynchronization Experiment

## Purpose

This project is tested as a small physical cluster, not only in CSIM. The
farm exposes failures that are easy to miss in one process: asymmetric RF
reception, ESP-NOW timing windows, sleep/wake clock error, stale gossip,
serial-capture gaps, and boards that independently make different rendezvous
decisions.

The long-run objective is to simulate as many independent from-scratch
resynchronization episodes as possible while the equipment is unattended.
Each episode starts with protocol state cleared by the firmware test hook,
then measures whether devices can rediscover a common beacon, exchange
reports, accumulate fresh association evidence, and qualify again.

This is an experiment harness. Automatic reset is not a production
availability policy and must not be mistaken for proof of global convergence.

## Physical topology

The farm contains six ESP32-class boards:

- four boards attached to the local host as `/dev/ttyUSB0` through
  `/dev/ttyUSB3`;
- two boards attached to the host named `miner6`, normally accessed as
  `miner6.local` over SSH on port 22.

The local deployment and logging helper is:

```bash
./scripts/deploy_usb_screens.py
```

It manages local serial screen sessions and the corresponding `cat.usbN.out`
captures in the project directory. Miner6 has its own two attached USB
devices and screen sessions. `miner6.local:11022` is sometimes a forwarded
connection to Jim's laptop; it is not the canonical Miner6 endpoint. Use
direct `miner6.local` SSH on port 22 for Miner6 work and verify active screen
sessions before assuming a log is current.

The local USB0 board is the principal subject of the historical “unheard
transmits” investigation. Its reported Wi-Fi identity has appeared as
`54a3ffd82240`. Do not infer a complete USB-port-to-MAC mapping from a stale
log: use current `espnow peer` and device-matrix lines when attributing a
board.

## Source, build, and deployment discipline

When firmware-relevant changes are already committed, synchronize hosts with
Git:

1. commit locally, including an `AI generated` annotation when the agent made
   the change;
2. push the commit to the shared remote;
3. pull or fast-forward the other checkout; and
4. build and flash from synchronized source where the remote toolchain exists.

Do not overwrite a remote worktree containing unrelated uncommitted protocol
work. Inspect `git status` and the branch head first. If the remote toolchain
is unavailable, a verified firmware artifact may be used as an explicit
fallback, but the exception should be reported and source should still be
synchronized through Git when safe.

Preserve the project's `build/` directory between ordinary Arduino builds.
Avoid `make clean`; the persistent build directory is intentional. Firmware
deployment and log capture are separate concerns: a flashed board can be
running correctly while its screen session or `cat.usbN.out` file is stale.

## One wake/exchange cycle

Each normal cycle has these conceptual stages:

1. **Beacon survey:** observe Wi-Fi beacons and choose or retain a directly
   visible home beacon.
2. **Clock alignment:** derive the sleep deadline from the selected beacon's
   TSF.
3. **ESP-NOW exchange:** send reports at 5 Hz during the awake window.
4. **Merge and decision:** merge direct and relayed claims/associations and
   evaluate proposals and listener counts.
5. **Persistence and sleep:** save observations and sleep to the next
   rendezvous.

The nominal rendezvous goal is 120 seconds. The obsolete fivefold period
increase after 50 cycles has been removed; a long interval in an old log may
be historical state from that experiment, not current behavior.

## Freshness and listener semantics

The current prototype uses two freshness windows:

- beacon claims: 20 wake cycles;
- selected-beacon associations: 6 wake cycles.

Direct association data has age 0. Relayed association data retains its
origin generation and age and is incremented as it crosses a relay. Hearing
the same generation through a relay does not make it newly direct. An
association at age 6 is still fresh; age 7 is expired.

`listeners=N` is the number of distinct fresh association origins in the
local table that select the logged home beacon. It includes the board itself
when its own association is fresh, and can include relayed evidence. Thus, in
a six-board farm, `listeners=6` means fresh evidence for all six identities
selecting that beacon; it does not mean six other boards were directly heard
in this exchange.

For direct transport evidence, inspect:

- `raw`: frames delivered to the ESP-NOW receive callback;
- `valid`: report frames accepted after protocol validation; and
- `espnow heard` / sender-specific counts.

`rawrx=0` on a peer while other boards produce raw and valid traffic points
below the report parser for that source. `rawrx>0` with `valid=0` points toward
source-specific framing, prefix, length, or version handling. `parse-reject`
is for Wi-Fi beacon-scan parsing, not a generic ESP-NOW report rejection
counter.

## The 10/10 automatic reset framework

The test hook turns unattended runtime into repeated trials. Every device
independently evaluates its end-of-cycle state:

1. The exchange must be healthy: sufficient report transmissions succeed,
   failures remain bounded, and valid peer reports are present.
2. The board must have at least six fresh association origins selecting its
   current home beacon.
3. A qualifying cycle increments the persisted consensus counter.
4. A bad cycle records a miss. The implementation tolerates up to two misses
   while preserving an in-progress streak; the third miss clears the streak.
5. Ten consecutive qualifying cycles commit the reset.
6. The commit is persisted and intentionally cannot be cancelled by later
   packet loss. The board waits three completed wake cycles.
7. `executeTestReset()` clears home-beacon state, claims, associations,
   claim-generation state, scout/proposal state, and consensus state.
8. The next wake starts a new from-scratch episode.

The reset commit and execution are separate. The delay makes the event
observable and prevents a reset in the same cycle as the tenth qualification.
It also means a reset can execute after a brief post-commit loss of listeners;
that is expected test-harness behavior.

The counter is local to each board. A farm-wide episode must therefore be
described carefully: boards may execute resets at different wakes, and
“10/10” is a per-board qualification rule, not a distributed consensus
protocol with one global commit.

## What to measure

Important measurements include:

- time from reset execution to first healthy exchange;
- time to first observed six-listener rendezvous;
- qualifying cycles before reset commit;
- time from commit to reset execution;
- whether all six devices retain a common beacon;
- per-peer raw/valid reception, especially for local USB0 MAC
  `54a3ffd82240`; and
- completed reset-and-resynchronization episodes per unattended hour.

The last metric is why the automatic reset exists. One successful convergence
is useful, but many independent episodes provide better evidence about
intermittent failures than one long run that remains in a favorable state.

## Analyzer workflow

For a no-history current snapshot:

```bash
python3 scripts/analyze_rendezvous.py --log-dir . --recent 0
```

For a short chronological window, use a positive value such as `--recent 10`.
Within each device's retained window, cycle 1 is oldest and the final cycle
is newest. For reset/qualification state across available logs:

```bash
python3 scripts/analyze_rendezvous.py --log-dir . --convergence
```

A quick apparent reunion requires more than matching `home=` values. Check
that each latest cycle is `exchange healthy`, has `listeners=6`, and is from
the same current time neighborhood. Then inspect peer lines to confirm that
identities are actually being received rather than merely retained as stale
hearsay.

Logs are append-only captures, not a synchronized database. A missing or
stale `cat.usbN.out` can make a healthy board look absent. Check capture
timestamps and the screen session before interpreting divergence.

## Review checklist for long runs

Before trusting a result, a code reviewer should verify:

- source commit and flashed binary correspond;
- all six serial captures are live and timestamped;
- no board carries obsolete persisted state that changes the test population;
- freshness uses the intended unit and boundary (`<= 6`);
- a healthy exchange cannot be declared from stale association data alone;
- reset qualification, reset execution, and observed rendezvous are separate;
- packet loss cannot corrupt origin identity or refresh relays artificially;
- local USB0 anomalies are evaluated with raw and valid counters;
- the test hook is isolated from production policy; and
- the run produces enough independent episodes to support its conclusion.

## Known limitations

The farm has no global clock or global reset barrier. RF conditions are
uncontrolled, physical beacon visibility may differ by board, and ESP-NOW
success is not equivalent to end-to-end application receipt by every intended
peer. Association evidence can be indirect until it expires. The experiment
therefore measures this physical topology under realistic timing and packet
loss; it does not prove convergence for arbitrary deployments.
