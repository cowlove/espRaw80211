# Zero-Knowledge Beacon Rendezvous Design

## Purpose

ESP32 clients use clocks carried by Wi-Fi beacon frames to wake within a
sub-second rendezvous window and exchange ESP-NOW traffic.  A loud local
beacon can split otherwise co-located clients into groups that wake on
different clocks.  The protocol should eventually move those logical groups
to a beacon visible to all of them, without knowing how many clients exist.

The capture HAL remains limited to beacon delivery.  Observation, gossip,
selection, scouting, persistence, and sleep policy belong to the application.

## Assumptions and limits

- Membership is unknown and may change at any time.
- Preconfigured information is limited to stable protocol parameters such as
  the nominal rendezvous period and scouting frequency.
- A client can synchronize, scout, propose, or select only a beacon whose
  frames it receives directly.  Relayed clock data is never sufficient.
- Global convergence is possible only when a common beacon is physically
  visible to every participating client.
- If no common beacon exists, multiple stable rendezvous groups are a valid
  outcome.  Silence cannot distinguish that state from slow discovery.
- Scouting never stops completely, because a new client may initially select
  a louder, non-common beacon.

## Terminology

- **Home beacon:** the beacon currently used for normal periodic rendezvous.
- **Distractor:** a beacon that is attractive locally but not shared broadly.
- **Direct claim:** an observation made by the identified originating client.
- **Relayed claim:** an unchanged direct claim forwarded by another client.
- **Supporter set:** the distinct origins with direct claims for a BSSID.
- **Scout wake:** an occasional rendezvous on another directly visible beacon.
- **Logical partition:** clients in the same physical area that do not exchange
  ESP-NOW traffic because they follow different beacon clocks.

## Information model

The atomic gossip fact is:

```
(origin client, beacon BSSID, observation generation, signal quality)
```

Relays preserve the origin and generation.  They must not convert claims into
recursive anonymous counts.  Receivers merge by `(origin, BSSID)`, accept only
newer generations, and can therefore deduplicate facts delivered through
multiple paths.

For initial development, use the full 48-bit client MAC.  It is easy to inspect
and collision-free.  A stable 32-bit hash can be evaluated later if packet
pressure justifies it.

Each client maintains:

- directly visible beacon observations for the current wake;
- a longer-lived table of direct and relayed claims;
- the set of client identities discovered through those claims;
- the supporter set and signal-quality summary for each known BSSID;
- its home beacon, current scout target, and proposed beacon;
- freshness, stability, and transmission bookkeeping.

Claim freshness and client identity retention are separate.  Normal sleep and
scout gaps must not make supporter sets oscillate.  Evidence used to move to a
new beacon should be reasonably fresh; evidence should age out much more slowly
when aging would force a move away from a stable home beacon.

Persisted claims are relay knowledge, not votes. A rendezvous decision uses
only claims directly observed or received during the current wake. Because
membership is unknown, exchange completeness is judged from local transport
health: the large majority of scheduled sends must succeed and valid peer
reports must arrive. An incomplete exchange retains the current home and
clears proposal progress. Packet loss therefore delays convergence rather
than becoming evidence for a switch.

Each stored claim also records the local wake when its origin generation last
advanced. Hearing another relay repeat the same generation does not refresh
that age. Old claims remain inspectable, but cease voting and cease being
relayed after the bounded freshness window until the origin refreshes them.

Beacon visibility and current association are separate facts. Visibility is a
many-to-many `(origin, BSSID)` relation and may remain true after a client
moves. Current association is a single last-writer-wins record per origin:
`(origin, selected BSSID, origin generation, age)`. A newer origin generation
immediately replaces the older selected BSSID. Relays preserve the origin and
generation and increase, rather than reset, the elapsed age. Only a report
received directly from the origin has age zero. Expired associations do not
count as current listeners, while their independently aged visibility claims
may remain useful.

Association age is measured in elapsed seconds so correctness does not depend
on every client using the same wake period. The singleton fast path compares
fresh association records: one listener on the current home and at least two
listeners on a directly visible, locally usable candidate permits an immediate
move. Visibility supporter sets continue to establish whether that move is
physically possible; they no longer masquerade as current listener counts.

## ESP-NOW gossip

Reports are sent at 5 Hz while awake.  A report header should identify:

- protocol version;
- immediate sender;
- sender session/wake generation and packet sequence;
- home beacon and proposed beacon;
- report/fragment type and included claim count.

Claim records identify the original client, BSSID, origin generation, signal
quality, and whether the immediate sender observed the claim directly.  A
relay cannot promote an indirect claim to direct.

The complete table need not fit in one ESP-NOW packet.  Successive reports
rotate through local and relayed claims.  Frequently repeating the sender's
home/proposal state while fairly rotating claims makes packet loss delay, but
not corrupt, convergence.

## Merge and selection rules

1. Local beacon frames create or refresh direct claims owned by this client.
2. Received claims retain their original owner and generation.
3. Duplicate and out-of-order claims are harmless.
4. Coverage dominates RSSI.  RSSI ranks candidates only after their supporter
   relationships have been considered.
5. Prefer a candidate whose supporter set is a strict superset of the current
   home beacon's supporter set.  This represents a genuine group merge.
6. Incomparable supporter sets do not trigger a switch merely because one is
   temporarily larger.  Continue scouting and gathering evidence.
7. Equivalent sets may be ranked by worst/median RSSI and then BSSID as a
   deterministic tie-breaker.
8. Proposals must remain stable for multiple rendezvous rounds before a home
   switch.  Moving away from a stable beacon uses stronger hysteresis than
   moving toward a demonstrated superset.
9. Proposal rounds must be consecutive healthy exchange rounds. An incomplete
   exchange cannot advance or preserve a partially completed proposal.
10. Equal current-wake support from only one device is not consensus evidence
    and cannot trigger a switch. Equal-support deterministic tie-breaking is
    enabled only with multi-device current evidence corroborated by equivalent
    retained supporter sets. A retained strict superset may corroborate a move
    only when the current wake also contains multi-device support.

No client declares discovery complete.  A newly learned client or claim can
always reopen the decision.

## Scouting

Most wakes use the home beacon.  A sparse, randomized schedule selects other
beacons that the client has directly observed.  Selection must be fair so that
every locally visible candidate is eventually visited.

After receiving the scout beacon's frames, the client derives the same
canonical rendezvous phase from its TSF that a home client would use.  This
aligns independent scouts on the candidate beacon instead of relying on
arbitrary overlapping dwell intervals.  The scout exchanges gossip, returns
to its home schedule, and carries newly merged claims back to that group.

Scouting continues at a reduced but nonzero rate after apparent convergence.

## Application states

- **HOME:** normal capture, gossip, and sleep on the selected beacon.
- **SCOUT:** temporary visit to another directly observed beacon.
- **CANDIDATE:** increased verification of a possible supporter-set superset.
- **PROPOSE:** advertise a stable preferred candidate without switching yet.
- **COMMITTED:** use the chosen home beacon while continuing sparse scouting.

These are application states and do not extend `HardwareContext` or the beacon
capture HAL.

## CSIM development topology

The first controlled scenario has an even number of clients:

- every client sees a unique, loud distractor beacon;
- clients in the first half share one weaker group beacon and clock;
- clients in the second half share a different weaker group beacon and clock;
- no beacon is common to both halves.

The expected stable result, once selection is enabled, is two groups, each on
its own common beacon.  This validates merging within a possible population
and stability when no globally common beacon exists.  Later scenarios will add
one still weaker beacon visible to both halves and verify eventual merging.

## Implementation stages

1. Add attributable, deduplicated claim storage and a versioned wire format;
   retain existing beacon selection.
2. Rotate/fragment gossip and verify exact propagation across normal home
   rendezvous rounds.
3. Implement perpetual scout wakes and demonstrate claims crossing logical
   partitions.
4. Compute supporter sets and proposals in report-only mode, with diagnostics
   explaining every proposed change.
5. Enable hysteretic home-beacon switching and verify the two-group scenario.
6. Add a universally visible weak beacon and verify eventual global merging.
7. Exercise packet loss, client arrival/removal, beacon disappearance, stale
   claims, incomparable sets, and near-tie signal quality.

## Required observability

CSIM output should make protocol behavior reviewable: wake mode and target,
directly visible BSSIDs, received senders, claim insert/update/drop decisions,
supporter sets, candidate ranking, proposal age, switch reason, and sleep
deadline.  Tests should assert stable outcomes rather than depend only on log
inspection.
