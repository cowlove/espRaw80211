#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rendezvousTiming.h"

// Version 5 is a coordinated wire revision, little-endian except the six
// BSSID octets, which are in printed/network order. No legacy decoding.
struct __attribute__((packed)) BeaconReportHeader {
    uint8_t version;
    uint64_t senderMac;
    uint64_t selectedBeacon;
    uint32_t wakeGeneration;
    uint16_t packetSequence;
    uint8_t claimCount;
    uint8_t associationCount;
    uint32_t incarnation;
    uint8_t clockBssid[6];
    uint32_t clockMsLow;
    int16_t exchangeStartDeltaMs;
    int16_t plannedEndDeltaMs;
    uint8_t timingValid;
};
struct __attribute__((packed)) BeaconClaimEntry {
    uint64_t originMac;
    uint64_t bssid;
    uint32_t originGeneration;
    int8_t rssi;
};
struct __attribute__((packed)) BeaconAssociationEntry {
    uint64_t originMac;
    uint64_t selectedBeacon;
    uint32_t originGeneration;
    uint16_t ageCycles;
};
static_assert(sizeof(BeaconReportHeader) == 44, "unexpected wire header size");
static_assert(sizeof(BeaconClaimEntry) == 21, "unexpected claim size");
static_assert(sizeof(BeaconAssociationEntry) == 22, "unexpected association size");
static_assert(44 + 3 * 21 + 4 * 22 + 4 == 199, "unexpected packet budget");

inline uint64_t reportClockBssid(const BeaconReportHeader &header) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 6; ++i) value = (value << 8) | header.clockBssid[i];
    return value;
}

inline bool reportDeltaMs(uint64_t eventTsf, uint64_t referenceTsf, int16_t &delta) {
    const uint64_t eventMs = eventTsf / 1000;
    const uint64_t referenceMs = referenceTsf / 1000;
    if (eventMs >= referenceMs) {
        if (eventMs - referenceMs > INT16_MAX) return false;
        delta = (int16_t)(eventMs - referenceMs);
    } else {
        if (referenceMs - eventMs > 32768) return false;
        delta = (int16_t)(-(int32_t)(referenceMs - eventMs));
    }
    return true;
}

inline bool setReportTiming(BeaconReportHeader &header, uint64_t bssid,
                             uint64_t tsf, uint64_t localRx,
                             uint64_t start, uint64_t plannedEnd) {
    header.timingValid = 0;
    uint64_t startTsf, endTsf;
    int16_t startDelta, endDelta;
    if (!bssid || plannedEnd < start ||
        !projectBeaconTsf(tsf, localRx, start, startTsf) ||
        !projectBeaconTsf(tsf, localRx, plannedEnd, endTsf) ||
        !reportDeltaMs(startTsf, tsf, startDelta) ||
        !reportDeltaMs(endTsf, tsf, endDelta)) return false;
    for (unsigned i = 0; i < 6; ++i) header.clockBssid[i] = bssid >> (40 - 8 * i);
    header.clockMsLow = (uint32_t)(tsf / 1000);
    header.exchangeStartDeltaMs = startDelta;
    header.plannedEndDeltaMs = endDelta;
    header.timingValid = 1;
    return true;
}

inline bool validReportLength(const BeaconReportHeader &header, size_t length) {
    return header.version == 5 && header.claimCount <= 3 &&
        header.associationCount <= 4 && length == sizeof(header) +
        header.claimCount * sizeof(BeaconClaimEntry) +
        header.associationCount * sizeof(BeaconAssociationEntry);
}
