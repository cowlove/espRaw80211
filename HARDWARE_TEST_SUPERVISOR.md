# Hardware clean-epoch supervisor

This test mode measures clean-start rendezvous convergence without changing the
existing distributed 10/10 qualification and reset mechanism.

## Protocol

Each reconnecting serial logger exclusively owns its serial port. A reset is
requested by atomically creating:

```text
/tmp/espRaw80211-usbN.reset-request
```

The file contains an opaque request ID. The logger notices it, records
`logger-reset-request`, pulses the ESP32 EN line while leaving IO0 high, removes
the file only after that succeeds, and records `logger-reset-pulsed`. No serial
command parser or wake-state rendezvous is involved.

On the resulting external cold boot, test firmware:

1. clears all persisted rendezvous, membership, proposal, and test state;
2. emits `TEST EPOCH RESET reason=cold delay-usec=...`;
3. deep-sleeps for a random 60–120 seconds;
4. wakes normally and begins a fresh beacon scan.

The initial sleep gives the supervisor time to reset every board and staggers
their first scans. Deep-sleep wakes do not clear state or emit another marker.

## Supervisor

`scripts/test_farm_supervisor.py` reads the existing five local and two Miner6
logs. It requires fresh unanimous home observations for a sustained interval,
then creates all seven request files concurrently. It will not re-arm until all
seven logs contain a new epoch marker.

The supervisor does not manage `screen`, open serial ports, or create an event
log. Device logs remain the source of truth. Its stdout is diagnostic only.

Analyze completed clean epochs directly from those logs:

```sh
./scripts/analyze_rendezvous.py --epoch-recovery --session all --tail-bytes 0
```

Dry run:

```sh
./scripts/test_farm_supervisor.py --dry-run
```

Run in a detached screen:

```sh
screen -dmS esp.test-supervisor bash -lc \
  'cd /home/jim/src/espRaw80211 && exec ./scripts/test_farm_supervisor.py'
```

The default two-minute sustain period intentionally lets the supervisor act
before the independent distributed 10/10 reset path. Run without the supervisor
when exercising that endurance mechanism.
