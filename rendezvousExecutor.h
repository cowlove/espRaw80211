#pragma once
#include <stdint.h>

namespace RendezvousExecutor {
// Monotonic elapsed time plus the planned sleep carried across deep sleep.
// This counts logical periods, independent of how many times setup() runs.
struct RoundClock {
    uint64_t remainder = 0;
    uint64_t last = 0;
    uint64_t tick(uint64_t now, uint64_t period) {
        if (!period || now < last) return 0;
        const uint64_t delta = now - last;
        last = now;
        const uint64_t rounds = remainder / period + delta / period;
        const uint64_t a = remainder % period, b = delta % period;
        const bool carry = b >= period - a;
        remainder = carry ? b - (period - a) : a + b;
        return rounds + (carry ? 1 : 0);
    }
};

inline uint32_t age(uint32_t current, uint64_t rounds) {
    return rounds > UINT32_MAX - current ? UINT32_MAX : current + (uint32_t)rounds;
}

struct Coverage {
    bool started = false, finished = false, full = false;
    uint32_t sent = 0, received = 0;
    void begin(uint64_t now, uint64_t plannedStart, bool alreadyLate,
               uint32_t tx, uint32_t rx, uint64_t tolerance) {
        started = true;
        full = !alreadyLate && now >= plannedStart && now-plannedStart <= tolerance;
        sent = tx; received = rx;
    }
    bool healthy(uint32_t tx, uint32_t rx) const {
        return full && tx-sent >= 20 && rx-received >= 3;
    }
};
}
