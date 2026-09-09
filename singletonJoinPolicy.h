#pragma once

#include <stddef.h>
#include <stdint.h>

namespace SingletonJoinPolicy {

// Positive evidence is useful even when a scout appointment began late. A
// missing packet during a partial appointment is not negative evidence.
inline bool mayEvaluateScout(uint32_t directPacketsSelectingTarget) {
    return directPacketsSelectingTarget > 0;
}

// Called after direct scout traffic confirms that at least one sender selected
// the target. One board at home may join an observed group of two or more.
inline bool mayAdopt(size_t homeMembers, size_t destinationMembers) {
    return homeMembers == 1 && destinationMembers >= 2;
}

// Two directly encountered singleton groups would otherwise reject one
// another forever as equal-sized. Both sides independently choose the lower
// BSSID, so exactly the singleton on the higher BSSID moves.
inline bool mayCoalesce(size_t homeMembers, size_t destinationMembers,
                        uint64_t homeBssid, uint64_t destinationBssid) {
    return homeMembers == 1 && destinationMembers == 1 && homeBssid &&
        destinationBssid && destinationBssid < homeBssid;
}

// Groups have one deterministic global ordering: larger membership wins, and
// equal-sized groups choose the lower BSSID. This prevents equal established
// groups from remaining deadlocked or moving in both directions.
inline bool groupPreferred(size_t candidateMembers, uint64_t candidateBssid,
                           size_t incumbentMembers, uint64_t incumbentBssid) {
    if (!candidateBssid || candidateBssid == incumbentBssid) return false;
    if (candidateMembers != incumbentMembers)
        return candidateMembers > incumbentMembers;
    return candidateBssid < incumbentBssid;
}

// Established groups consider destinations supported by positive direct
// appointment evidence according to the shared group ordering.
inline bool mayPropose(size_t homeMembers, uint64_t homeBssid,
                       size_t destinationMembers, uint64_t destinationBssid) {
    return homeMembers >= 2 &&
        groupPreferred(destinationMembers, destinationBssid,
                       homeMembers, homeBssid);
}

inline uint32_t proposalDelay(uint32_t homeCredibility) {
    const uint32_t extra = homeCredibility / 4;
    return 2 + (extra > 3 ? 3 : extra);
}

inline uint32_t reinforce(uint32_t credibility, bool heardDirectPeer) {
    return heardDirectPeer && credibility < 12 ? credibility + 1 : credibility;
}

inline uint32_t decay(uint32_t credibility) {
    return credibility ? credibility - 1 : 0;
}

} // namespace SingletonJoinPolicy
