#pragma once

#include <cstddef>

namespace SingletonJoinPolicy {

// Called only after a healthy, fully covered direct scout appointment.
// One board at home may join any observed destination group of two or more.
inline bool mayAdopt(size_t homeMembers, size_t destinationMembers) {
    return homeMembers == 1 && destinationMembers >= 2;
}

} // namespace SingletonJoinPolicy
