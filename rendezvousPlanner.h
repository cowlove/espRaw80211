#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rendezvousTiming.h"

namespace RendezvousPlanner {

struct Appointment {
    uint64_t start = 0;
    uint64_t end = 0; // half-open local monotonic interval
    uint64_t bssid = 0;
    bool home = false;
    bool late = false; // partial coverage cannot qualify a full appointment
};

struct AwakeInterval {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t appointments = 0;
};

// Return the currently active (clipped and marked late) or next appointment.
// phase is the exchange start within a beacon period, NOT a wake-up deadline.
// Acquisition/boot lead belongs to the executor, separately from radio windows.
inline bool nextAppointment(uint64_t bssid, uint64_t beaconTsf,
                            uint64_t observedLocal, uint64_t now,
                            uint64_t period, uint64_t phase,
                            uint64_t duration, bool home, Appointment &out) {
    if (!bssid || !period || phase >= period || !duration || duration > period ||
        observedLocal > now) return false;
    uint64_t nowTsf;
    if (!projectBeaconTsf(beaconTsf, observedLocal, now, nowTsf)) return false;
    const uint64_t position = nowTsf % period;
    const uint64_t untilStart = phase >= position ? phase - position :
        period - (position - phase);
    const uint64_t age = untilStart == 0 ? 0 : period - untilStart;
    const bool active = age < duration && age <= nowTsf;
    const uint64_t wait = active ? 0 : untilStart;
    const uint64_t remaining = active ? duration - age : duration;
    if (wait > UINT64_MAX - now || remaining > UINT64_MAX - (now + wait)) return false;
    out = {now + wait, now + wait + remaining, bssid, home, active && age != 0};
    return true;
}

// Pure bounded planner; no allocation, radio calls, persistence or sleep.
// Construct all required home appointments BEFORE attempting optional scouts.
// A failed required addition invalidates the plan: never execute a partial
// home schedule. Optional rejection leaves the last valid plan unchanged.
template<size_t Capacity = 32>
class Plan {
    static_assert(Capacity > 0 && Capacity <= 64, "appointment bitmask capacity");
    Appointment appointments_[Capacity] = {};
    AwakeInterval intervals_[Capacity] = {};
    size_t count_ = 0, intervalsCount_ = 0;
    uint64_t mergeGap_;
    uint64_t homeAwake_ = 0;
    bool valid_ = true, scoutsStarted_ = false;

    void rebuild() {
        intervalsCount_ = count_;
        for (size_t i = 0; i < count_; ++i)
            intervals_[i] = {appointments_[i].start, appointments_[i].end, 1ULL << i};
        for (size_t i = 1; i < intervalsCount_; ++i) {
            const AwakeInterval item = intervals_[i];
            size_t j = i;
            while (j && intervals_[j-1].start > item.start) {
                intervals_[j] = intervals_[j-1];
                --j;
            }
            intervals_[j] = item;
        }
        size_t kept = 0;
        for (size_t i = 0; i < intervalsCount_; ++i) {
            const AwakeInterval item = intervals_[i];
            if (kept && (item.start <= intervals_[kept-1].end ||
                         item.start - intervals_[kept-1].end <= mergeGap_)) {
                if (item.end > intervals_[kept-1].end) intervals_[kept-1].end = item.end;
                intervals_[kept-1].appointments |= item.appointments;
            } else intervals_[kept++] = item;
        }
        intervalsCount_ = kept;
    }

    bool wellFormed(const Appointment &a) const { return a.bssid && a.start < a.end; }

public:
    explicit Plan(uint64_t mergeGap) : mergeGap_(mergeGap) {}
    bool valid() const { return valid_; }
    size_t appointmentCount() const { return count_; }
    size_t intervalCount() const { return intervalsCount_; }
    const Appointment &appointment(size_t i) const { return appointments_[i]; }
    const AwakeInterval &interval(size_t i) const { return intervals_[i]; }

    uint64_t awakeUsec() const {
        uint64_t total = 0;
        // Sorted disjoint intervals: their total cannot exceed UINT64_MAX.
        for (size_t i = 0; i < intervalsCount_; ++i)
            total += intervals_[i].end - intervals_[i].start;
        return total;
    }

    bool addHome(Appointment a) {
        if (!valid_ || scoutsStarted_ || count_ == Capacity || !wellFormed(a)) {
            valid_ = false;
            return false;
        }
        a.home = true;
        appointments_[count_++] = a;
        rebuild();
        homeAwake_ = awakeUsec();
        return true;
    }

    // Budget includes time spent bridging nearby intervals, not just the
    // scout's own duration. A scout wholly covered by home costs zero. The
    // limit is cumulative across ALL scouts, relative to the home-only plan.
    bool addScout(Appointment a, uint64_t maxAdditionalAwake) {
        if (!valid_ || count_ == 0 || count_ == Capacity || !wellFormed(a)) return false;
        scoutsStarted_ = true;
        a.home = false;
        appointments_[count_++] = a;
        rebuild();
        if (awakeUsec() - homeAwake_ > maxAdditionalAwake) {
            --count_;
            rebuild();
            return false;
        }
        return true;
    }

    // Execution hint only. Caller supplies boot/acquisition lead, accounts for
    // calibration, and must not sleep if required observation/planning failed.
    uint64_t sleepUntilNext(uint64_t now, uint64_t lead) const {
        if (!valid_) return 0;
        for (size_t i = 0; i < intervalsCount_; ++i) {
            if (intervals_[i].end <= now) continue;
            if (intervals_[i].start <= now || intervals_[i].start - now <= lead) return 0;
            return intervals_[i].start - now - lead;
        }
        return 0; // exhausted horizon requires replanning, not indefinite sleep
    }
};

// Eligible candidate list must already be freshness/channel/RSSI filtered.
// Caller persists lastServed after a deliberate attempt/defer and records its
// outcome, so one unreachable candidate cannot starve all the others.
// Selection is stable under list permutation, duplicate entries and removal.
inline uint64_t chooseScout(const uint64_t *eligible, size_t count,
                            uint64_t home, uint64_t lastServed) {
    uint64_t smallest = 0, next = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t value = eligible[i];
        if (!value || value == home) continue;
        if (!smallest || value < smallest) smallest = value;
        if (value > lastServed && (!next || value < next)) next = value;
    }
    return next ? next : smallest;
}

// Optional stateless weighted exploration. Every eligible non-home beacon
// gets one ticket; a priority entry adds extraTickets more. Callers use the
// fair rotating selector above when extraTickets is zero.
inline uint64_t chooseWeightedScout(const uint64_t *eligible, size_t count,
                                    const uint64_t *priority,
                                    size_t priorityCount, uint64_t home,
                                    uint32_t randomValue,
                                    size_t extraTickets) {
    size_t tickets = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!eligible[i] || eligible[i] == home) continue;
        bool duplicate = false;
        for (size_t prior = 0; prior < i; ++prior)
            if (eligible[prior] == eligible[i]) duplicate = true;
        if (duplicate) continue;
        ++tickets;
        for (size_t j = 0; j < priorityCount; ++j)
            if (priority[j] == eligible[i]) {
                tickets += extraTickets;
                break;
            }
    }
    if (!tickets) return 0;
    size_t ticket = randomValue % tickets;
    for (size_t i = 0; i < count; ++i) {
        if (!eligible[i] || eligible[i] == home) continue;
        bool duplicate = false;
        for (size_t prior = 0; prior < i; ++prior)
            if (eligible[prior] == eligible[i]) duplicate = true;
        if (duplicate) continue;
        size_t weight = 1;
        for (size_t j = 0; j < priorityCount; ++j)
            if (priority[j] == eligible[i]) {
                weight += extraTickets;
                break;
            }
        if (ticket < weight) return eligible[i];
        ticket -= weight;
    }
    return 0;
}

} // namespace RendezvousPlanner
