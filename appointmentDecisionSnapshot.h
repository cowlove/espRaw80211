#pragma once

#include <stddef.h>
#include <stdint.h>

#include "singletonJoinPolicy.h"

namespace AppointmentDecisionSnapshot {

constexpr uint8_t schemaVersion = 1;

enum class DecisionKind : uint8_t {
    ObserveCandidate = 1,
    CommitCheck = 2,
};

enum class Action : uint8_t {
    InvalidSnapshot = 0,
    NoAction,
    CoalesceSingletons,
    JoinLargerGroup,
    RejectNotPreferred,
    CancelNotPreferred,
    RefreshProposal,
    RejectWeakerThanPending,
    StartProposal,
    CancelHomeChanged,
    CancelHomeCaughtUp,
    AwaitActivation,
    CommitProposal,
};

struct Snapshot {
    uint8_t version = schemaVersion;
    DecisionKind kind = DecisionKind::ObserveCandidate;
    uint32_t incarnation = 0;
    uint32_t wakeGeneration = 0;
    uint32_t exchangeSequence = 0;
    uint8_t appointmentIndex = 0;
    uint8_t decisionOrdinal = 0;
    uint64_t homeBssid = 0;
    uint32_t homeMembers = 0;
    uint64_t targetBssid = 0;
    uint32_t targetMembers = 0;
    uint64_t pendingBssid = 0;
    uint64_t pendingHome = 0;
    uint32_t pendingMembers = 0;
    uint32_t pendingActRound = 0;
    uint8_t associationExpectedRows = 0;
    uint8_t associationObservedRows = 0;
    uint64_t associationFingerprint = 0;
};

inline bool valid(const Snapshot &snapshot) {
    if (snapshot.version != schemaVersion || !snapshot.homeBssid) return false;
    if (snapshot.associationExpectedRows != snapshot.associationObservedRows)
        return false;
    if (snapshot.associationExpectedRows && !snapshot.associationFingerprint)
        return false;
    if (snapshot.kind == DecisionKind::ObserveCandidate)
        return snapshot.targetBssid &&
            snapshot.targetBssid != snapshot.homeBssid;
    if (snapshot.kind == DecisionKind::CommitCheck)
        return snapshot.pendingBssid != 0;
    return false;
}

inline Action replay(const Snapshot &snapshot) {
    using namespace SingletonJoinPolicy;
    if (!valid(snapshot)) return Action::InvalidSnapshot;

    if (snapshot.kind == DecisionKind::CommitCheck) {
        if (snapshot.pendingHome != snapshot.homeBssid)
            return Action::CancelHomeChanged;
        if (!groupPreferred(snapshot.pendingMembers, snapshot.pendingBssid,
                            snapshot.homeMembers, snapshot.homeBssid))
            return Action::CancelHomeCaughtUp;
        if (int32_t(snapshot.wakeGeneration - snapshot.pendingActRound) < 0)
            return Action::AwaitActivation;
        return Action::CommitProposal;
    }

    if (mayCoalesce(snapshot.homeMembers, snapshot.targetMembers,
                    snapshot.homeBssid, snapshot.targetBssid))
        return Action::CoalesceSingletons;
    if (mayAdopt(snapshot.homeMembers, snapshot.targetMembers))
        return Action::JoinLargerGroup;
    if (!mayPropose(snapshot.homeMembers, snapshot.homeBssid,
                    snapshot.targetMembers, snapshot.targetBssid))
        return snapshot.pendingBssid == snapshot.targetBssid ?
            Action::CancelNotPreferred : Action::RejectNotPreferred;
    if (snapshot.pendingBssid == snapshot.targetBssid &&
        snapshot.pendingHome == snapshot.homeBssid)
        return Action::RefreshProposal;
    if (snapshot.pendingBssid &&
        !groupPreferred(snapshot.targetMembers, snapshot.targetBssid,
                        snapshot.pendingMembers, snapshot.pendingBssid))
        return Action::RejectWeakerThanPending;
    return Action::StartProposal;
}

template <size_t Capacity>
class Buffer {
public:
    bool append(const Snapshot &snapshot) {
        if (count_ == Capacity) {
            overflowed_ = true;
            return false;
        }
        records_[count_++] = snapshot;
        return true;
    }

    size_t size() const { return count_; }
    bool overflowed() const { return overflowed_; }
    const Snapshot &operator[](size_t index) const { return records_[index]; }

private:
    Snapshot records_[Capacity] = {};
    size_t count_ = 0;
    bool overflowed_ = false;
};

inline bool orderedAfter(const Snapshot &later, const Snapshot &earlier) {
    // Incarnations are identities, not monotonic sequence numbers. Host wall
    // time must order records across boots.
    if (later.incarnation != earlier.incarnation) return false;
    if (later.wakeGeneration != earlier.wakeGeneration)
        return later.wakeGeneration > earlier.wakeGeneration;
    if (later.exchangeSequence != earlier.exchangeSequence)
        return later.exchangeSequence > earlier.exchangeSequence;
    if (later.appointmentIndex != earlier.appointmentIndex)
        return later.appointmentIndex > earlier.appointmentIndex;
    return later.decisionOrdinal > earlier.decisionOrdinal;
}

inline const char *actionName(Action action) {
    switch (action) {
        case Action::InvalidSnapshot: return "invalid";
        case Action::NoAction: return "none";
        case Action::CoalesceSingletons: return "coalesce";
        case Action::JoinLargerGroup: return "join";
        case Action::RejectNotPreferred: return "reject-not-preferred";
        case Action::CancelNotPreferred: return "cancel-not-preferred";
        case Action::RefreshProposal: return "refresh";
        case Action::RejectWeakerThanPending: return "reject-weaker";
        case Action::StartProposal: return "propose";
        case Action::CancelHomeChanged: return "cancel-home-changed";
        case Action::CancelHomeCaughtUp: return "cancel-home-caught-up";
        case Action::AwaitActivation: return "await-activation";
        case Action::CommitProposal: return "commit";
    }
    return "unknown";
}

} // namespace AppointmentDecisionSnapshot
