#pragma once
#include <stdint.h>
#include <stddef.h>

// Random incarnations are identities, never sortable generations. Relays may
// introduce unknown origins but only an origin's direct report may replace
// an established incarnation. Keep this registry across deep sleep.
struct OriginEpoch {
    uint64_t origin = 0;
    uint32_t epoch = 0;
};
enum class EpochDecision { same, introduced, replaced, rejected };
inline EpochDecision acceptEpoch(OriginEpoch *entries, size_t count,
                                 uint64_t origin, uint32_t epoch, bool direct) {
    if (!origin || !epoch) return EpochDecision::rejected;
    size_t empty = count;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].origin == origin) {
            if (entries[i].epoch == epoch) return EpochDecision::same;
            if (!direct) return EpochDecision::rejected;
            entries[i].epoch = epoch;
            return EpochDecision::replaced;
        }
        if (!entries[i].origin && empty == count) empty = i;
    }
    if (empty == count) return EpochDecision::rejected;
    entries[empty].origin = origin;
    entries[empty].epoch = epoch;
    return EpochDecision::introduced;
}
