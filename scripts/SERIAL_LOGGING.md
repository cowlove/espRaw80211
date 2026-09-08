# Reconnecting screen loggers

## Artificial test swarm size versus USB count

`testSwarmConfig.h` is the single source for `ARTIFICIAL_TEST_SWARM_BOARD_COUNT`
(currently 7). Firmware's test reset qualification and the offline analyzer use
it. Count every participating board, including boards without USB/logging.
For example, nine participating boards with only five USB loggers means set 9,
not 5. USB discovery, flashing targets, and simulator context count remain
independent. Logs from a subset provide only that subset's local observations.

This is artificial external knowledge used ONLY for controlled tests. Real-world
swarms have no known global count; scheduling/discovery/membership acceptance
must not depend on it. Rebuild/redeploy after changing the header. The firmware
clears only test qualification/reset progress when this value changes, preserving
membership. Historical analyzer qualification uses the current header value;
firmware logs its compiled test count for experiment provenance.

Run `python3 scripts/deploy_usb_screens.py --log-only` to restart logging for
all detected USB boards without building, flashing, or erasing. Use
`--boards 0,1,2,3,4` to select the five local ports explicitly. Normal deployment
starts this same logger after each successful upload.

Requires Python 3, pySerial and GNU screen. Each logger retries missing ports
every second and appends to `cat.usbN.out`, reopening the log on each emission
to support reconnect and rotation. Logs are not truncated. A fresh session ID
per connection prevents analysis bridging incomplete exchanges across a gap.
Unfinished lines are discarded at disconnect; disconnected data is unrecoverable.

Launch binds to `/dev/serial/by-path` when available. Reconnect to the SAME USB
socket/hub topology: ttyUSB renumbering is tolerated, moving sockets is not.
This identifies a socket, not a board. Swapping boards requires reviewing the
mapping. Duplicate adapter serial IDs make by-id unsuitable for this cluster.
No explicit DTR/RTS reset pulses are issued, though USB drivers/reconnect may
still reset hardware.

The analyzer discovers local `cat.usb*.out` files, including USB4. Historical
files remain discoverable; use session/time filtering as appropriate.

Validation includes a real pseudo-terminal hangup/reconnect regression for
append, session boundaries, resumed data and partial-line isolation. Five local
screens received serial output on 2026-09-08; no firmware was uploaded.
Miner6 loggers were not changed during this local rollout.
