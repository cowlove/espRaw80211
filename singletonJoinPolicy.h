#pragma once

#include <cstddef>
#include <cstdint>

namespace SingletonJoinPolicy {

// Called only after a healthy, fully covered direct scout appointment.
// One board at home may join any observed destination group of two or more.
inline bool mayAdopt(size_t homeMembers, size_t destinationMembers) {
    return homeMembers == 1 && destinationMembers >= 2;
}

// Established groups only consider a destination observed during a direct,
// healthy scout visit, and only when that group is strictly larger.
inline bool mayPropose(size_t homeMembers, size_t destinationMembers) {
    return homeMembers >= 2 && destinationMembers > homeMembers;
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
