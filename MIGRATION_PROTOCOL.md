# Rendezvous Beacon Migration Protocol

This document describes how a board decides whether to keep or change its
selected rendezvous beacon. It is intended to be a precise guide to the current
implementation, including the evidence it trusts, its persistent state, every
decision path, and the known tradeoffs.

## Implementation status

The policy accepts positive evidence from a partial scout. At least one valid,
directly received ESP-NOW report must say that its sender selected the scout
target. Missing packets during a partial scout are not negative evidence.

## Purpose and desired invariants

The protocol tries to coalesce independently starting boards onto one beacon
without allowing incomplete or stale gossip to fragment an established group.
Its main invariants are:

- A board may learn about another group either by scouting that group or by
  hearing one of its members visit the board's home.
- An established member may move only toward a group believed to be strictly
  larger than its current group.
- Claim gossip alone must not move an established member.
- Positive evidence from a scout is useful even if the scout began late.
- Missing packets in a partial scout must not prove that a peer is absent.
- A credible established home is deliberately harder to leave.
- A pending move is canceled if the current home catches up before commitment.
- Beacon quality determines whether a beacon can be visited, but does not by
  itself authorize migration.

## Terminology

### Home beacon

The beacon currently persisted in `spiffsBeacon`. Normal home rendezvous
appointments are scheduled around its beacon timing.

### Scout target

An eligible non-home beacon selected for a temporary ESP-NOW appointment. A
scout observes a possible destination; scouting does not itself change home.

### Logical round

Approximately one rendezvous period (currently 30 seconds). Association aging,
scout cadence, credibility confirmation, and proposal activation use logical
rounds rather than raw boot count.

### Association

An assertion of the form:

```
origin board -> selected beacon
```

An association can be received directly from its origin or relayed by another
board. It carries origin generation/incarnation data so evidence from an older
boot cannot overwrite newer evidence.

### Listener count / group estimate

`listenerCount(bssid)` counts unique association-table origins whose selected
beacon is `bssid` and whose age is no more than six logical rounds. The local
board publishes and retains its own association, so the home count normally
includes itself.

This is an **estimate**, not an omniscient physical board count. It can contain
both directly learned and relayed fresh associations.

### Direct positive scout evidence

In the current implementation, a scout has positive evidence when at least one
valid packet was directly received from a sender whose report header says its
`selectedBeacon` equals the scout target.

This proves that at least that sender selected the target. It does not prove
that all target members were heard.

### `full`

The appointment began close enough to its planned start and covered the whole
window. A late-started appointment is `full 0` even if it receives useful
packets.

### `healthy`

Transport-health evidence requiring a full appointment, at least 20 successful
transmissions, at least three valid received reports from at least one sender,
and no more than five transmission failures.

Therefore a partial appointment is never `healthy`, even when it contains valid
positive evidence.

### Home credibility

A persistent score from 0 through 12 associated with the current home. It
increases by one after a healthy, full home appointment that hears a direct
peer, and otherwise decreases by one. It controls how long an established
member waits before committing a proposed move.

## Inputs to migration

The active migration path uses three distinct kinds of information.

### 1. Local beacon observations

Beacon packets establish that a BSSID is visible and provide timing needed to
schedule a scout. A candidate currently needs:

- nonzero BSSID;
- RSSI of at least -85 dBm;
- at least three captured beacon packets; and
- fresh timing that can be projected into an appointment.

These observations decide **where and when it is possible to scout**. Beacon
RSSI and packet count do not authorize a home change.

### 2. Direct ESP-NOW reports during an appointment

A valid report identifies its physical sender and the sender's selected beacon.
During a scout this triggers evaluation of the scout target. During a home
appointment, a visiting scout advertising another home can trigger
reverse-discovery evaluation through the same migration policy.

### 3. Fresh association table

Direct reports and their relayed association entries update the association
table. The table supplies the numerical home and destination group estimates.
Only entries no more than six logical rounds old count.

This creates an important distinction:

- the **decision trigger** is direct positive evidence from the scout target;
- the **group-size estimate** may include fresh relayed association evidence.

## How scout targets are scheduled

Home appointments are planned first. A scout can be added only when it fits the
awake-time budget and does not displace the home appointment.

Scouting is considered every two logical rounds. Each eligible non-home beacon
gets one selection ticket. When an established group has fresh association
evidence that a locally visible beacon contains exactly one member, that
beacon gets one additional ticket. The resulting weighted random choice keeps
every destination discoverable while gently favoring visits that can carry a
larger-group invitation to an isolated board. It adds no new progression or
persistent targeting state. Overlapping or nearby radio windows may be merged
into one awake interval.

The scheduler may discover that an appointment has already partly elapsed when
it builds or enters the interval. Such an appointment is partial (`full 0`),
but received packets remain real observations.

## Complete decision flow

The current behavior can be summarized as:

```text
complete scout appointment
    |
    +-- no directly received packet whose sender selected this target
    |       -> no migration evaluation
    |
    +-- positive direct target packet(s)
            -> estimate homeMembers and targetMembers from fresh associations
               |
               +-- both estimates == 1 and target BSSID is lower
               |       -> commit immediately (deterministic 1+1 merge)
               |
               +-- homeMembers == 1 and targetMembers >= 2
               |       -> commit immediately (singleton join)
               |
               +-- homeMembers >= 2 and targetMembers > homeMembers
               |       -> create, refresh, or compare a delayed proposal
               |
               +-- otherwise
                       -> reject the target, or cancel its pending proposal

complete home appointment
    |
    +-- no direct visitor advertising an eligible alternative home
    |       -> no reverse migration evaluation
    |
    +-- choose largest advertised eligible group
            -> apply the same singleton/adopt or established/propose flow above
```

There is no `healthy && full` prerequisite for scout evaluation. Positive
packet evidence is the gate; coverage remains diagnostic context.

## Singleton behavior

A board is treated as a singleton when its fresh home listener count is exactly
one. It immediately adopts a scout target when the target listener count is at
least two. Two directly encountered singleton groups use lowest BSSID as a
deterministic tie-break: the board on the higher home moves and the board on
the lower home stays.

```text
homeMembers == 1 && targetMembers >= 2 -> commitHome(target)
homeMembers == 1 && targetMembers == 1 && target < home -> commitHome(target)
```

There is no credibility delay or proposal period for this case. Once the board
moves, it normally ceases to be a singleton, so the relaxed rule no longer
applies.

Because both boards compute the same winning BSSID, the 1+1 exception creates
a two-board group without oscillation. It closes the deadlock in which every
board initializes on a different beacon and all encounters are otherwise
rejected as equal-sized.

### Visitor evidence at home

A board need not personally scout the larger group. A member of that group may
scout the board's home while continuing to advertise its real selected beacon.
The board evaluates only packets received during that exact home appointment.
A visitor target must:

- be advertised by a directly heard visitor;
- differ from the board's current home;
- have at least one fresh associated member (two for ordinary singleton
  adoption, or one when the deterministic 1+1 tie-break selects it); and
- remain locally visible, eligible, and supported by fresh beacon timing.

If several visitors advertise eligible destinations, the board chooses the
largest fresh group estimate, breaking equal-size ties by lowest BSSID. A true
singleton commits immediately when the target has at least two members. An
established group member uses the normal strictly-larger test and
credibility-delayed proposal path. A partial home appointment may provide
positive visitor evidence; missing traffic is never negative evidence.

## Established-member behavior

A board with at least two estimated home members is established. It may propose
a destination only when:

```text
targetMembers > homeMembers
```

Equal-size and smaller destinations are rejected. This strict inequality is
the primary anti-fragmentation rule.

### Creating a proposal

A new proposal persists:

- target BSSID;
- the home BSSID from which it was proposed;
- activation logical round; and
- target member-count snapshot.

The activation delay depends on current home credibility:

| Home credibility | Delay |
| ---: | ---: |
| 0-3 | 2 rounds |
| 4-7 | 3 rounds |
| 8-11 | 4 rounds |
| 12 | 5 rounds |

At a 30-second logical round, this is nominally about 60 to 150 seconds, though
the actual commit also waits for a qualifying home appointment.

### Seeing the same target again

Another qualifying scout of the same target refreshes the stored target-member
snapshot. It does **not** postpone the existing activation round.

### Seeing a different target

If a proposal is already pending, a different target replaces it only if its
current member estimate is greater than the pending proposal's stored estimate.
An equally large or smaller alternative is rejected as `weaker-than-pending`.

## Proposal commitment and cancellation

A proposal is checked only after a healthy, full appointment at the current
home. This gives the incumbent group a fresh opportunity to demonstrate its
current size before the board leaves.

The checks occur in this order:

1. **No proposal:** keep home.
2. **Home changed since proposal:** cancel with `home-changed`.
3. **Home now equals or exceeds the target snapshot:** cancel with
   `home-caught-up`.
4. **Activation round not reached:** retain the proposal and keep home.
5. **Activation round reached:** commit the stored target.

The commit uses the stored target-member snapshot. It does not currently
require a fresh second scout at the instant of commitment.

## What committing a new home does

`commitHome(target)`:

- persists the new home BSSID;
- clears the pending proposal;
- resets home credibility to zero for the new home;
- resets scout phase and pending scout target;
- resets the rendezvous timing goal/repetition adjustment; and
- restores the initial timing scale.

The scheduler must then acquire fresh timing for the new home. If that fails,
the separate recovery state machine performs two blind sleeps and then reboots
into strongest-immediate-beacon initialization rather than remaining awake
forever.

## What cannot move a board

The following are deliberately insufficient by themselves:

- a stronger RSSI;
- a higher local beacon packet count;
- seeing a beacon without receiving ESP-NOW evidence there;
- claim gossip alone;
- relayed association gossip without a direct positive scout trigger;
- a destination estimated equal to or smaller than an established home;
- missing packets during a partial scout;
- a pending proposal whose home has caught up;
- an unconfirmed proposal before its credibility-dependent activation round.

The source still contains the older `reportOnlyCandidate()` claim/supporter
ranking function, but it has no active call site in the current migration path.
It should be considered legacy/auxiliary code, not part of the policy above.

## Why partial scout packets now matter

The old gate treated coverage quality as a prerequisite for using any evidence:

```text
healthy && full -> consider migration
```

That conflates positive and negative inference. A partial visit cannot establish
that no one was present, because the board missed part of the window. But a
valid report actually received during that visit is still direct proof that its
sender was present and selected that beacon.

The current gate therefore uses:

```text
direct valid packet advertising scout target -> consider migration
```

`full` remains valuable for diagnostics, coverage statistics, and negative
conclusions. It is no longer used to discard positive evidence.

In the deterministic 7,200-second CSIM run at reception scale 0.60, this changed
the result from a permanent 4+3 split to first 7/7 convergence at about 834
seconds and first 10/10 qualification at about 1,226 seconds. Useful partial
scouts contained 3, 8, and 5 valid target packets in key decisions.

## Worked examples

### Singleton joins a group

```text
home estimate:        1
scout target estimate: 3
direct target packet: yes
result: immediate commit to target
```

### Established group proposes a larger group

```text
home estimate:         3
target estimate:       4
home credibility:      9
direct target packet:  yes
result: proposal activates four logical rounds later
```

If the home estimate remains below four at a healthy, full home appointment on
or after activation, the board moves.

### Home catches up

```text
proposal target snapshot: 4
current home estimate:     4
result: cancel proposal (home-caught-up)
```

### Partial scout with useful traffic

```text
full:                  0
valid target packets:  5
home estimate:         3
target estimate:       4
result: migration proposal evaluated from positive direct evidence
```

### Partial scout with no traffic

```text
full:                  0
valid target packets:  0
result: no migration evaluation and no negative membership conclusion
```

## Logs associated with decisions

- `scout-positive-evidence`: direct packets advertising the scout target and
  whether the appointment was full.
- `singleton-join direct`: immediate singleton adoption.
- `singleton-join visitor`: immediate adoption of a group advertised by a
  visitor during the singleton's home appointment.
- `singleton-coalesce`: a 1+1 encounter resolved toward the lower BSSID.
- `scout-weighted`: a scout selection made while one or more fresh rumored
  singleton targets were eligible; reports whether the selected target had
  the additional ticket.
- `visitor-positive-evidence`: a directly heard visitor advertised an eligible
  alternative home during this home appointment.
- `migration-proposal visitor`: an established member proposed a larger group
  learned from a visitor; confirmation is identical to a scout proposal.
- `migration-proposal direct`: new proposal, group estimates, credibility, and
  activation round.
- `migration-proposal refreshed`: same target seen again.
- `migration-rejected ... not-larger`: destination is not strictly larger.
- `migration-rejected ... weaker-than-pending`: an alternative does not beat
  the existing proposal snapshot.
- `migration-proposal canceled ... home-caught-up`: incumbent group recovered.
- `migration-proposal canceled ... home-changed`: proposal no longer belongs
  to the current home.
- `migration-proposal committed`: delayed move accepted.
- `home-credibility`: updated score and the health/coverage inputs.
- `appointment complete`: home/scout, target, `full`, and `healthy` status.

## Known complexities and open policy questions

### Direct trigger, partly relayed size estimate

The positive scout trigger is direct, but `listenerCount()` may count relayed
fresh associations. A single direct packet can therefore trigger evaluation of
a group size partly supported by gossip. If this proves too permissive, the
next refinement should calculate a separate directly observed destination
count rather than reinstating the full-window gate.

### Target snapshot can age before commitment

The delayed commit compares a fresh home estimate against a stored target
snapshot. The target may have changed during the delay. Requiring another
positive scout before commitment would be safer but slower.

### Larger equal partitions cannot deliberately merge

The deterministic lower-BSSID rule resolves 1+1 encounters safely. Strictly-
larger migration still means two stable equal-size groups of two or more cannot
deliberately resolve their tie. A broader deterministic tie-breaker would need
additional safeguards against simultaneous group swaps.

### Association capacity and freshness affect perceived size

The in-memory table holds 32 associations, but each wire report carries only
three rotating association records. Evidence expires after six logical rounds.
Thus the estimated size can lag, fluctuate, or differ between boards even when
the physical group is unchanged.

### Credibility deliberately trades speed for stability

High credibility adds up to three extra confirmation rounds. This protects an
established group from a transient observation but can slow valid convergence.

### Home and scout evidence are intentionally asymmetric

Partial positive scout evidence may create a proposal, while proposal commitment
still requires a healthy, full home appointment. This is deliberate: the scout
supplies proof of an alternative, and the full home visit supplies a final fair
test of the incumbent.

## Compact pseudocode

```text
on scout completion(target):
    positive = directPacketsWhoseSenderSelected(target)
    log coverage and positive packet count

    if positive == 0:
        return

    homeN = freshAssociationCount(home)
    targetN = freshAssociationCount(target)

    if homeN == 1 and targetN >= 2:
        commitHome(target)
        return

    if homeN < 2 or targetN <= homeN:
        cancel proposal for target, if any
        return

    if proposal is for target from current home:
        refresh target snapshot without changing activation round
        return

    if another proposal exists and targetN <= its snapshot:
        reject target
        return

    proposal = {
        target,
        current home,
        target snapshot = targetN,
        activation = current round + delay(home credibility)
    }

on home completion(home):
    visitors = directPacketsReceivedDuringThisAppointment()
    candidates = eligibleFreshHomesAdvertisedBy(visitors)
    if candidates:
        evaluate largestThenLowestBssid(candidates) using the same flow above

on healthy full home completion(home):
    update home credibility

    if proposal belongs to another home:
        cancel it
    else if freshAssociationCount(home) >= proposal.targetSnapshot:
        cancel it
    else if current round >= proposal.activation:
        commitHome(proposal.target)
```
