# ESP32 Wi-Fi beacon rendezvous prototype

This is an exploratory ESP32 sketch for using nearby 802.11 beacon frames as
a timing reference. It is not a finished low-power scheduler or a general
Wi-Fi scanner.

## What it does

The sketch puts the ESP32 into promiscuous management-frame receive mode on a
single configured channel (`wifi_channel`, currently channel 4). It recognizes
beacon frames, identifies them by transmitter address (the BSSID), and records
three clocks:

- the beacon's 802.11 TSF timestamp (`pk->timestamp`),
- the ESP32 receive timestamp supplied by the Wi-Fi driver
  (`pt->rx_ctrl.timestamp`), and
- the local `micros()` value captured when the callback runs.

Up to 64 beacon records are kept in `pktLog`. Repeated observations update the
record's timestamp, averaged RSSI, receive time, and count. The prototype can
select the beacon with the highest observation count and persist its address in
`/beaconX`.

## Runtime flow

1. `setup()` prints the persisted beacon, configures the task watchdog, and
   starts promiscuous capture with `intr_oneShot()` for the persisted beacon.
2. `loop()` waits briefly for that beacon. If it is not seen, it switches to
   `intr_collect()`, collects beacon records for about 250 ms, chooses the most
   frequently observed beacon, and persists its address.
3. The selected beacon timestamp and local receive time are reduced modulo a
   persisted rendezvous period (`/currentGoal`). The difference is reported as
   the timing error between the beacon clock and the ESP32 clock.
4. The sketch then attempts ordinary Wi-Fi association and eventually restarts.
   The current source does not yet implement a robust final wake schedule; the
   historical experiments and log output include deep-sleep/reboot behavior.

## Persistent state and safety guards

The sketch stores beacon identity, rendezvous goal, sleep time, scale, and a
repetition counter through `SPIFFSVariable`. Older or erased state can contain
zero values. In particular, a zero `/currentGoal` would cause an integer
divide-by-zero at the modulo operation, so the code restores the prototype's
60,000,000-microsecond default before using it. A zero sleep interval is also
handled when calculating the reported percentage error.

The rendezvous goal is the repeating period used to calculate the next beacon
clock boundary; it is not merely the amount of time spent asleep. The actual
sleep interval is the remaining time until that boundary after capture and
processing overhead. The initial 60-second period leaves room for boot,
promiscuous capture, logging, and synchronization while still providing a
useful rendezvous cadence. A valid persisted goal is retained across deep
sleep.

## Build notes

The Makefile targets the original ESP32 by default:

```sh
make BOARD=esp32
make BOARD=esp32 upload
```

It also contains an ESP32-S3 path, but the sketch has target/core-version
assumptions and is not currently portable to every Arduino-ESP32 release.
The current host build was verified with Arduino-ESP32 3.2.0 and the ESP32
board; the task-watchdog initialization is conditional for that API version.

## Known limitations

- Only one Wi-Fi channel is monitored.
- Beacon visibility, RSSI ranking, and antenna/multipath effects can make two
  nearby devices choose different beacon sets.
- The raw-frame structure and timestamp handling are hardware/core-specific.
- Promiscuous capture, Wi-Fi association, SPIFFS state, and deep sleep are
  combined experimentally rather than coordinated as a production design.
- There is no bounded, validated recovery protocol when the selected beacon is
  absent or the clocks disagree.

The later design discussion in `../espSensorModule/power-and-scheduling.md`
describes the intended evolution: stable beacon identities, quorum or
configured-beacon selection, a deterministic future boundary, and a bounded
recovery window.
