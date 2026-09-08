#pragma once
#include <stdint.h>

// Both local timestamps must use the same boot-relative clock. A beacon
// captured after exchange start requires a negative offset, not clamping.
inline bool projectBeaconTsf(uint64_t tsf, uint64_t receivedUsec,
                             uint64_t eventUsec, uint64_t &projected) {
    if (eventUsec >= receivedUsec) {
        const uint64_t delta = eventUsec - receivedUsec;
        if (delta > UINT64_MAX - tsf) return false;
        projected = tsf + delta;
    } else {
        const uint64_t delta = receivedUsec - eventUsec;
        if (delta > tsf) return false;
        projected = tsf - delta;
    }
    return true;
}
