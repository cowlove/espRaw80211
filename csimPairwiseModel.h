#pragma once

#ifdef CSIM

#include <stdint.h>
#include "csimPairwiseData.h"

// Empirical seven-board RF model derived from analyze_rendezvous.py's
// full-history COMBINED matrices on 2026-09-08. Rows are receivers; columns
// are senders. The same model intentionally applies to home and scout windows.
namespace CsimPairwiseModel {

static constexpr size_t boardCount = CsimPairwiseData::boardCount;
static constexpr uint64_t firstMac = 0xddeeff000001ULL;
static constexpr float nominalPacketsPerSecond = 5.0f;
// Apply one deliberately conservative scale to both empirical success gates.
// This stands in for dependencies missing from the first measured model.
static constexpr float receptionScale = 0.60f;
static constexpr uint64_t seed = 0x6d5a56e9d31b4a27ULL;

struct ReceiverState {
    bool active = false;
    uint32_t window = 0;
    uint32_t packet[boardCount] = {};
};

inline ReceiverState states[boardCount] = {};

inline int index(uint64_t mac) {
    return mac >= firstMac && mac < firstMac + boardCount
        ? static_cast<int>(mac - firstMac) : -1;
}

inline uint64_t mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline uint32_t sample(uint8_t receiver, uint8_t sender, uint32_t window,
                       uint32_t packet, uint32_t scale) {
    const uint64_t key = seed ^ (uint64_t(receiver) << 56) ^
        (uint64_t(sender) << 48) ^ (uint64_t(window) << 16) ^ packet;
    return static_cast<uint32_t>(mix(key) % scale);
}

inline float scaledSuccess(float success) {
    if (success <= 0) return 0;
    if (success >= 1) return receptionScale;
    return success * receptionScale;
}

inline void beginWindow(uint64_t receiverMac, uint32_t window) {
    const int receiver = index(receiverMac);
    if (receiver < 0) return;
    states[receiver] = {};
    states[receiver].active = true;
    states[receiver].window = window;
}

inline void endWindow(uint64_t receiverMac) {
    const int receiver = index(receiverMac);
    if (receiver >= 0) states[receiver].active = false;
}

inline bool drop(uint64_t senderMac, uint64_t receiverMac) {
    const int sender = index(senderMac), receiver = index(receiverMac);
    if (sender < 0 || receiver < 0 || sender == receiver) return false;
    ReceiverState &state = states[receiver];
    if (!state.active) return true;

    // One receiver-specific draw gates the complete overlap window. A second
    // draw applies the measured unconditional packet rate, intentionally
    // making this first model slightly harsher than the hardware observations.
    const float windowSuccess = scaledSuccess(
        CsimPairwiseData::healthyWindowPercent[receiver][sender] / 100.0f);
    if (sample(receiver, sender, state.window, 0, 1000000) >=
            static_cast<uint32_t>(windowSuccess * 1000000.0f))
        return true;
    const float probability = scaledSuccess(
        CsimPairwiseData::packetsPerSecond[receiver][sender] /
        nominalPacketsPerSecond);
    if (probability <= 0) return true;
    if (probability >= 1) return false;
    const uint32_t packet = ++state.packet[sender];
    return sample(receiver, sender, state.window, packet, 1000000) >=
        static_cast<uint32_t>(probability * 1000000.0f);
}

} // namespace CsimPairwiseModel

#endif
