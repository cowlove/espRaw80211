#include "jimlib.h"
#include "espNowMux.h"
#include "raw80211Capture.h"
#include "rendezvousTiming.h"
#include "beaconReport.h"
#ifndef ESP32
#error Only the ESP32 is supported
#endif
#ifndef CSIM
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <rom/uart.h>
#define CSIM_ASSERT(condition) do { } while (0)
#else
#include <assert.h>
#define CSIM_ASSERT(condition) assert(condition)
#endif

// jimlib.cpp is part of the source-discovery CSIM build and owns helpers that
// reference the sketch-level JStuff singleton.
JStuff j;

static uint64_t beaconBssid(const uint8_t *frame) {
    uint64_t value = 0;
    for (int i = 0; i < 6; ++i) value = (value << 8) | frame[10 + i];
    return value;
}

static uint64_t beaconTsf(const uint8_t *frame) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | frame[24 + i];
    return value;
}

struct BeaconInfo {
    uint64_t ssid = 0;
    int rssi = 0;
    uint64_t ts = 0;
    int count = 0;
    uint64_t seen = 0;
    uint64_t seen2 = 0;
    uint64_t firstSeen2 = 0;
    uint64_t maxGapUsec = 0;
    int minRssi = 0;
    int maxRssi = 0;
};

// Application-level rendezvous advertisement.  Keep this deliberately small:
// ESPNowMux reserves four bytes for the routing prefix and uses a conservative
// 200-byte physical packet size.
static_assert(sizeof(BeaconReportHeader) +
              3 * sizeof(BeaconClaimEntry) +
              4 * sizeof(BeaconAssociationEntry) + 4 <= ESPNowMux::physicalPacketBytes,
              "BRPT report exceeds ESPNowMux packet budget");

struct BeaconClaim {
    uint64_t originMac = 0;
    uint64_t bssid = 0;
    uint32_t originGeneration = 0;
    int8_t rssi = -127;
    // Local wake when this origin advanced the claim. Repeated relays of the
    // same origin generation do not keep stale evidence alive forever.
    uint32_t learnedWakeGeneration = 0;
    // Runtime-only: persisted claims remain relay knowledge, but only claims
    // refreshed or received during this wake may vote in a decision.
    uint32_t receivedWakeGeneration = 0;
};

struct BeaconAssociation {
    uint64_t originMac = 0;
    uint64_t selectedBeacon = 0;
    uint32_t originGeneration = 0;
    uint32_t ageCycles = 0;
    uint64_t storedAtUsec = 0;
};

struct RemoteBeaconStats {
    uint64_t bssid = 0;
    uint32_t reports = 0;
    uint32_t observations = 0;
    uint32_t selectedReports = 0;
    int8_t strongestRssi = -127;
    int8_t lastRssi = -127;
    uint64_t lastSender = 0;
    uint64_t selectingClientMacs[16] = {};
    uint8_t selectingClientCount = 0;
};

#ifdef CSIM
#ifndef CONTEXT_COUNT
#define CONTEXT_COUNT 4
#endif
// Application-owned RF world. Each destination selects one environment, so
// simulated devices can observe different BSSIDs and beacon clocks while the
// capture HAL remains unaware of the RF model.
class BeaconSimulationEnvironment : public Csim_Module {
    struct SimBeacon {
        uint64_t bssid;
        int8_t rssi;
        uint8_t channel;
        uint32_t intervalUsec;
        uint64_t tsfOrigin;
        uint32_t tsfRatePpm;
        uint64_t nextUsec;
    };
    struct Environment {
        SimBeacon beacons[2];
    } environments[CONTEXT_COUNT] = {};
    struct Destination {
        CsimWifiBeaconCaptureSource *capture;
        uint8_t channel;
        uint8_t environmentId;
    } destinations[CONTEXT_COUNT] = {};
    size_t destinationCount = 0;

public:
    BeaconSimulationEnvironment() {
        for (uint8_t i = 0; i < CONTEXT_COUNT; ++i) {
            const uint64_t bssidBase = 0x000096ce0000ULL +
                ((uint64_t)i << 8);
            const uint64_t tsfOrigin = 1000000ULL +
                (uint64_t)i * 16000000ULL;
            const uint32_t tsfRatePpm = 1000000 +
                (int32_t)((i % 5) - 2) * 150;
            environments[i].beacons[0] =
                {bssidBase + 2, (int8_t)(-38 - (i % 5)), 4, 51200,
                 tsfOrigin, tsfRatePpm, 0};
            // Split the fleet into two physically valid rendezvous groups.
            // Each client has a louder unique distractor, while all clients
            // in its half see exactly the same weaker beacon and clock.
            const bool secondHalf = i >= (CONTEXT_COUNT + 1) / 2;
            environments[i].beacons[1] = secondHalf
                ? SimBeacon{0x000096ce0fb2ULL, -68, 4, 102400,
                            29000000, 999850, 0}
                : SimBeacon{0x000096ce0fa1ULL, -66, 4, 102400,
                            7000000, 1000125, 0};
        }
    }

    uint64_t absoluteUsec() const { return sim().bootTimeUsec + micros(); }

    void emit(const SimBeacon &beacon, uint64_t emissionUsec,
              const Destination &destination) {
        if (destination.environmentId >= CONTEXT_COUNT ||
            destination.channel != beacon.channel)
            return;
        uint8_t frame[36] = {};
        frame[0] = 0x80;
        for (int i = 0; i < 6; ++i)
            frame[10 + i] = beacon.bssid >> (40 - 8 * i);
        const uint64_t tsf = beacon.tsfOrigin +
            emissionUsec * beacon.tsfRatePpm / 1000000ULL;
        memcpy(frame + 24, &tsf, sizeof(tsf));
        WifiBeaconPacket packet;
        packet.driverTimestampUsec = emissionUsec & 0xffffffffULL;
        // The application timing math intentionally uses the ESP32-style
        // boot-relative callback clock.  Keep the RF scheduler's absolute
        // time internal to the simulator and present the same clock shape as
        // the hardware callback.
        packet.localTimestampUsec = emissionUsec - sim().bootTimeUsec;
        packet.rssi = beacon.rssi;
        packet.channel = beacon.channel;
        packet.data = frame;
        packet.length = sizeof(frame);
        destination.capture->inject(packet);
    }

    void addDestination(CsimWifiBeaconCaptureSource *capture, uint8_t channel,
                        uint8_t environmentId) {
        CSIM_ASSERT(destinationCount < sizeof(destinations) / sizeof(destinations[0]));
        destinations[destinationCount++] = {capture, channel, environmentId};
    }

    void loop() override {
        const uint64_t now = absoluteUsec();
        for (uint8_t environmentId = 0; environmentId < CONTEXT_COUNT;
             ++environmentId) {
            for (SimBeacon &beacon : environments[environmentId].beacons) {
                if (beacon.nextUsec == 0) beacon.nextUsec = now;
                while (now >= beacon.nextUsec) {
                    for (size_t i = 0; i < destinationCount; ++i)
                        if (destinations[i].environmentId == environmentId)
                            emit(beacon, beacon.nextUsec, destinations[i]);
                    beacon.nextUsec += beacon.intervalUsec;
                }
            }
        }
    }
};

static BeaconSimulationEnvironment beaconEnvironment;
using BeaconRendezvousContextBase = Csim_privateContext;
#else
using BeaconRendezvousContextBase = HardwareContext;
#endif

class BeaconRendezvousContext : public BeaconRendezvousContextBase {
    // This is an observation inventory, not the candidate list. Keep it large
    // enough that weak or otherwise ineligible BSSIDs are still available for
    // post-wake diagnosis instead of being displaced by stronger beacons.
    static constexpr size_t packetLogSize = 256;
    static constexpr size_t remoteStatsSize = 64;
    static constexpr size_t claimTableSize = 128;
    static constexpr size_t associationTableSize = 32;
    // Header plus rotating visibility and association records, including the
    // four-byte BRPT prefix, stays within ESPNowMux's conservative 200-byte
    // physical packet limit.
    static constexpr size_t reportMaxClaims = 3;
    static constexpr size_t reportMaxAssociations = 4;
    static constexpr int reportMinRssi = -85;
    static constexpr int minimumCandidatePackets = 3;
    // Testing-only bootstrap policy: after flash erase or an automatic test
    // reset, sample one of the six loudest eligible beacons instead of always
    // taking the single strongest one. Normal home/scout selection is unchanged.
    static constexpr size_t testStartupTopN = 6;
    static constexpr uint64_t reportPeriodUsec = 200000;
    static constexpr uint64_t defaultRendezvousUsec = 30ULL * 1000000ULL;
    static constexpr uint32_t scoutIntervalWakes = 2;
    static constexpr uint32_t claimFreshnessWakes = 20;
    // Long-run test setting: retain association evidence across this many
    // rendezvous periods. Association records are aged by wake, not wall time.
    static constexpr uint32_t associationFreshnessCycles = 6;
    // Temporary long-run bootstrap test hook. Each device independently
    // commits to a reset after ten consecutive healthy cycles in which six
    // fresh associations select its home beacon. It then waits three more
    // completed wake cycles before resetting, even if consensus is lost.
    static constexpr size_t testClusterSize = 6;
    static constexpr int testConsensusCyclesToCommit = 10;
    static constexpr int testResetDelayCycles = 3;
    static constexpr int testConsensusMissesToReset = 3;
    // Beacon acquisition and ESP-NOW exchange are separate phases. Keep a
    // generous acquisition window, and allow five seconds for gossip once a
    // usable beacon has been observed.
    static constexpr uint64_t beaconSamplingWindowUsec = 5ULL * 1000000ULL;
    static constexpr uint64_t exchangeWindowUsec = 5ULL * 1000000ULL;

    BeaconInfo packetLog[packetLogSize] = {};
    SPIFFSVariable<uint64_t> spiffsBeacon{"/beaconX", 0};
    SPIFFSVariable<uint64_t> spiffsSleepTime{"/sleepTimeX", 0};
    SPIFFSVariable<float> spiffsScale{"/scaleX", 1.0};
    SPIFFSVariable<uint64_t> spiffsCurrentGoal{"/currentGoal", 0};
    SPIFFSVariable<int> spiffsCurrentRep{"/currentRep", 0};
    SPIFFSVariable<uint32_t> spiffsClaimGeneration{"/claimGen", 0};
    SPIFFSVariable<uint32_t> spiffsIncarnation{"/incarnation", 0};
    SPIFFSVariable<string> spiffsClaims{"/claims", ""};
    SPIFFSVariable<string> spiffsAssociations{"/associations", ""};
    SPIFFSVariable<int> spiffsScoutPhase{"/scoutPhase", 0};
    SPIFFSVariable<uint64_t> spiffsScoutBeacon{"/scoutBeacon", 0};
    SPIFFSVariable<uint64_t> spiffsProposalBeacon{"/proposal", 0};
    SPIFFSVariable<int> spiffsProposalAge{"/proposalAge", 0};
    SPIFFSVariable<int> spiffsTestConsensusCycles{"/testConsensus", 0};
    SPIFFSVariable<int> spiffsTestConsensusMisses{"/testMisses", 0};
    SPIFFSVariable<int> spiffsTestResetCommitted{"/testResetCommit", 0};
    SPIFFSVariable<int> spiffsTestResetDelayCycles{"/testResetDelay", 0};
    uint64_t targetBeacon = 0;
    int wifiChannel = 4;
    uint64_t startUsec = 0;
    uint64_t nextReportUsec = 0;
    uint64_t espNowStartUsec = 0;
    uint64_t espNowEndUsec = 0;
    uint64_t deviceMac = 0;
    int loopCount = 0;
    RemoteBeaconStats remoteStats[remoteStatsSize] = {};
    BeaconClaim claims[claimTableSize] = {};
    BeaconAssociation associations[associationTableSize] = {};
    uint32_t wakeGeneration = 0;
    uint32_t incarnation = 0;
    uint32_t reportBadLength = 0;
    uint16_t reportSequence = 0;
    uint16_t reportTxCount = 0;
    uint16_t reportRxCount = 0;
    uint16_t reportRxClaimCount = 0;
    uint16_t reportValidRxCount = 0;
    uint64_t reportSenders[16] = {};
    uint64_t reportSenderRadioFrom[16] = {};
    uint16_t reportSenderRaw[16] = {};
    uint16_t reportSenderValid[16] = {};
    uint64_t reportSenderFirstUsec[16] = {};
    uint64_t reportSenderLastUsec[16] = {};
    uint16_t reportSenderShort[16] = {};
    uint16_t reportSenderBadVersion[16] = {};
    uint16_t reportSenderAssociationRefresh[16] = {};
    uint32_t associationMergeAttempts = 0;
    uint32_t associationMergeAccepted = 0;
    uint32_t associationMergeInvalid = 0;
    uint32_t associationMergeOlder = 0;
    uint32_t associationMergeNotFresher = 0;
    uint32_t associationMergeFull = 0;
    uint16_t reportSenderClaimEntries[16] = {};
    uint16_t reportSenderRadioMismatch[16] = {};
    uint8_t reportSenderCount = 0;
    uint32_t scanParseRejects = 0;
    uint32_t scanAccepted = 0;
    uint32_t targetHits = 0;
    size_t claimTransmitCursor = 0;
    size_t associationTransmitCursor = 0;
    bool scoutWake = false;
    bool scoutRendezvousWake = false;
    bool espNowStarted = false;
    uint64_t beaconReceivedAtUsec = 0;

    void executeTestReset(uint64_t homeBssid) {
        out("TEST RESET EXECUTED on %012llx",
            (unsigned long long)homeBssid);
        spiffsBeacon = (uint64_t)0;
        spiffsClaims = string("");
        spiffsAssociations = string("");
        spiffsClaimGeneration = (uint32_t)0;
        spiffsIncarnation = (uint32_t)0;
        spiffsScoutPhase = 0;
        spiffsScoutBeacon = (uint64_t)0;
        spiffsProposalBeacon = (uint64_t)0;
        spiffsProposalAge = 0;
        spiffsTestConsensusCycles = 0;
        spiffsTestConsensusMisses = 0;
        spiffsTestResetCommitted = 0;
        spiffsTestResetDelayCycles = 0;
        memset(claims, 0, sizeof(claims));
        memset(associations, 0, sizeof(associations));
    }

    void maybeResetAfterStableReunion(uint64_t homeBssid,
                                      bool healthyExchange) {
        if (spiffsTestResetCommitted.read()) {
            const int delayCycles = spiffsTestResetDelayCycles.read() + 1;
            spiffsTestResetDelayCycles = delayCycles;
            out("TEST RESET delay %d/%d%s", delayCycles,
                testResetDelayCycles,
                delayCycles >= testResetDelayCycles ? " complete" : "");
            if (delayCycles >= testResetDelayCycles)
                executeTestReset(homeBssid);
            return;
        }

        const size_t listeners = homeBssid == 0 ? 0 :
            listenerCount(homeBssid);
        if (!healthyExchange || homeBssid == 0 ||
            listeners < testClusterSize) {
            const int priorCycles = spiffsTestConsensusCycles.read();
            const int misses = spiffsTestConsensusMisses.read() + 1;
            spiffsTestConsensusMisses = misses;
            if (misses < testConsensusMissesToReset) {
                out("test consensus miss %d/%d; preserving streak %d/%d (healthy %s listeners %d)",
                    misses, testConsensusMissesToReset, priorCycles,
                    testConsensusCyclesToCommit, healthyExchange ? "yes" : "no",
                    (int)listeners);
                return;
            }
            if (priorCycles != 0 || misses >= testConsensusMissesToReset) {
                out("test consensus lost after %d misses; counter reset from %d/%d",
                    misses, priorCycles, testConsensusCyclesToCommit);
                spiffsTestConsensusCycles = 0;
            }
            return;
        }

        if (spiffsTestConsensusMisses.read() != 0)
            out("test consensus miss streak cleared after healthy cycle");
        spiffsTestConsensusMisses = 0;
        const int cycles = spiffsTestConsensusCycles.read() + 1;
        spiffsTestConsensusCycles = cycles;
        out("test consensus %d/%d on %012llx listeners %d", cycles,
            testConsensusCyclesToCommit, (unsigned long long)homeBssid,
            (int)listeners);
        if (cycles < testConsensusCyclesToCommit) return;

        // This persisted commit is intentionally irreversible. Subsequent
        // exchange or association loss cannot cancel the delayed reset.
        spiffsTestResetCommitted = 1;
        spiffsTestResetDelayCycles = 0;
        out("TEST RESET COMMITTED on %012llx; execute after %d wake cycles",
            (unsigned long long)homeBssid, testResetDelayCycles);
    }

    void loadClaims() {
        memset(claims, 0, sizeof(claims));
        const string encoded = spiffsClaims.read();
        size_t offset = 0;
        size_t slot = 0;
        while (offset < encoded.size() && slot < claimTableSize) {
            unsigned long long origin = 0;
            unsigned long long bssid = 0;
            unsigned generation = 0;
            int rssi = -127;
            unsigned learned = 0;
            int consumed = 0;
            int fields = sscanf(encoded.c_str() + offset,
                "%llx,%llx,%x,%d,%x;%n", &origin, &bssid, &generation,
                &rssi, &learned, &consumed);
            if (fields != 5 || consumed <= 0) {
                consumed = 0;
                fields = sscanf(encoded.c_str() + offset,
                    "%llx,%llx,%x,%d;%n", &origin, &bssid, &generation,
                    &rssi, &consumed);
                if (fields != 4 || consumed <= 0) break;
            }
            claims[slot++] = {(uint64_t)origin, (uint64_t)bssid,
                              (uint32_t)generation, (int8_t)rssi,
                              (uint32_t)learned, 0};
            offset += (size_t)consumed;
        }
    }

    void saveClaims() {
        string encoded;
        char record[64];
        for (const BeaconClaim &claim : claims) {
            if (claim.originMac == 0) continue;
            snprintf(record, sizeof(record), "%llx,%llx,%x,%d,%x;",
                     (unsigned long long)claim.originMac,
                     (unsigned long long)claim.bssid,
                     claim.originGeneration, (int)claim.rssi,
                     claim.learnedWakeGeneration);
            encoded += record;
        }
        spiffsClaims = encoded;
    }

    uint32_t associationAgeCycles(const BeaconAssociation &association) const {
        return association.ageCycles;
    }

    void loadAssociations() {
        memset(associations, 0, sizeof(associations));
        const string encoded = spiffsAssociations.read();
        size_t offset = 0;
        size_t slot = 0;
        while (offset < encoded.size() && slot < associationTableSize) {
            unsigned long long origin = 0;
            unsigned long long selected = 0;
            unsigned generation = 0;
            unsigned age = 0;
            int consumed = 0;
            const int fields = sscanf(encoded.c_str() + offset,
                "%llx,%llx,%x,%x;%n", &origin, &selected, &generation,
                &age, &consumed);
            if (fields != 4 || consumed <= 0) break;
            // One persisted record spans one completed sleep/wake boundary.
            const uint64_t aged = (uint64_t)age + 1;
            associations[slot++] = {
                (uint64_t)origin, (uint64_t)selected,
                (uint32_t)generation,
                aged > UINT32_MAX ? UINT32_MAX : (uint32_t)aged,
                startUsec};
            offset += (size_t)consumed;
        }
    }

    void saveAssociations() {
        string encoded;
        char record[64];
        for (const BeaconAssociation &association : associations) {
            if (association.originMac == 0) continue;
            snprintf(record, sizeof(record), "%llx,%llx,%x,%x;",
                     (unsigned long long)association.originMac,
                     (unsigned long long)association.selectedBeacon,
                     association.originGeneration,
                     associationAgeCycles(association));
            encoded += record;
        }
        spiffsAssociations = encoded;
    }

    bool mergeAssociation(uint64_t originMac, uint64_t selectedBeacon,
                          uint32_t originGeneration, uint32_t ageCycles,
                          bool direct = false) {
        associationMergeAttempts++;
        if (originMac == 0 || selectedBeacon == 0) {
            associationMergeInvalid++;
            return false;
        }
        const uint32_t storedAge = direct ? 0U :
            (ageCycles == UINT32_MAX ? UINT32_MAX : ageCycles + 1U);
        size_t empty = associationTableSize;
        for (size_t i = 0; i < associationTableSize; ++i) {
            BeaconAssociation &association = associations[i];
            if (association.originMac == originMac) {
                if (originGeneration < association.originGeneration) {
                    associationMergeOlder++;
                    return false;
                }
                const uint32_t currentAge = associationAgeCycles(association);
                if (originGeneration == association.originGeneration &&
                    !direct && ageCycles >= currentAge) {
                    associationMergeNotFresher++;
                    return false;
                }
                association = {originMac, selectedBeacon, originGeneration,
                               storedAge, micros()};
                associationMergeAccepted++;
                return true;
            }
            if (empty == associationTableSize && association.originMac == 0)
                empty = i;
        }
        if (empty == associationTableSize) {
            associationMergeFull++;
            return false;
        }
        associations[empty] = {originMac, selectedBeacon, originGeneration,
                               storedAge, micros()};
        associationMergeAccepted++;
        return true;
    }

    size_t listenerCount(uint64_t bssid) const {
        size_t count = 0;
        for (const BeaconAssociation &association : associations)
            if (association.originMac != 0 &&
                association.selectedBeacon == bssid &&
                associationAgeCycles(association) <= associationFreshnessCycles)
                count++;
        return count;
    }

    void dumpAssociationTable(uint64_t homeBssid) const {
        out("association-table home %012llx listeners %d freshness %u wake %u",
            (unsigned long long)homeBssid, (int)listenerCount(homeBssid),
            associationFreshnessCycles, wakeGeneration);
        for (const BeaconAssociation &association : associations) {
            if (association.originMac == 0) continue;
            const uint32_t age = associationAgeCycles(association);
            const bool selectedHome = association.selectedBeacon == homeBssid;
            const bool fresh = age <= associationFreshnessCycles;
            out("association origin %012llx selected %012llx generation %u age %u selected-home %u fresh %u qualifies %u",
                (unsigned long long)association.originMac,
                (unsigned long long)association.selectedBeacon,
                association.originGeneration, age,
                selectedHome ? 1U : 0U, fresh ? 1U : 0U,
                selectedHome && fresh ? 1U : 0U);
        }
    }

    static int score(const BeaconInfo &info) { return info.count; }

    static bool betterQuality(const BeaconInfo &a, const BeaconInfo &b) {
        if (a.count != b.count) return a.count > b.count;
        const uint64_t aSpan = a.seen2 >= a.firstSeen2 ?
            a.seen2 - a.firstSeen2 : 0;
        const uint64_t bSpan = b.seen2 >= b.firstSeen2 ?
            b.seen2 - b.firstSeen2 : 0;
        if (aSpan != bSpan) return aSpan > bSpan;
        if (a.maxGapUsec != b.maxGapUsec)
            return a.maxGapUsec < b.maxGapUsec;
        return a.rssi > b.rssi;
    }

    void recordBeacon(BeaconInfo &info, const WifiBeaconPacket &packet) {
        const uint64_t now = packet.localTimestampUsec;
        if (info.count == 0) {
            info.firstSeen2 = now;
            info.minRssi = packet.rssi;
            info.maxRssi = packet.rssi;
        } else {
            const uint64_t gap = now >= info.seen2 ? now - info.seen2 : 0;
            info.maxGapUsec = max(info.maxGapUsec, gap);
            info.minRssi = min(info.minRssi, (int)packet.rssi);
            info.maxRssi = max(info.maxRssi, (int)packet.rssi);
        }
        info.seen2 = now;
        info.seen = packet.driverTimestampUsec;
        info.rssi = (packet.rssi + info.count * info.rssi) /
                    (info.count + 1);
        info.count++;
        info.ts = beaconTsf(packet.data);
    }

    size_t claimCount() const {
        size_t count = 0;
        for (const BeaconClaim &claim : claims)
            if (claim.originMac != 0) count++;
        return count;
    }

    void dumpDeviceBeaconMatrix() {
        const uint64_t homeBssid = spiffsBeacon.read();
        out("matrix begin claims %d", (int)claimCount());
        for (const BeaconClaim &claim : claims) {
            if (claim.originMac == 0) continue;
            out("matrix claim device %012llx beacon %012llx gen %u rssi %d%s",
                (unsigned long long)claim.originMac,
                (unsigned long long)claim.bssid,
                claim.originGeneration, (int)claim.rssi,
                claim.bssid == homeBssid ? " selected-home" : "");
        }
        for (const BeaconAssociation &association : associations) {
            if (association.originMac == 0) continue;
            const uint32_t age = associationAgeCycles(association);
            out("matrix association device %012llx selected %012llx gen %u age %uwakes %s",
                (unsigned long long)association.originMac,
                (unsigned long long)association.selectedBeacon,
                association.originGeneration, age,
                age <= associationFreshnessCycles ? "fresh" : "expired");
        }
        for (const BeaconInfo &info : packetLog) {
            if (info.ssid == 0) continue;
            const uint64_t span = info.seen2 >= info.firstSeen2 ?
                info.seen2 - info.firstSeen2 : 0;
            out("matrix scan-observed beacon %012llx packets %d span %.3f sec maxgap %.3f sec rssi avg %d min %d max %d eligible %s",
                (unsigned long long)info.ssid, info.count,
                span / 1000000.0, info.maxGapUsec / 1000000.0,
                info.rssi, info.minRssi, info.maxRssi,
                info.rssi >= reportMinRssi &&
                info.count >= minimumCandidatePackets ? "yes" : "no");
        }
        for (const RemoteBeaconStats &stats : remoteStats) {
            if (stats.bssid == 0) continue;
            out("matrix beacon %012llx reports %u observations %u selected %u clients %u",
                (unsigned long long)stats.bssid, stats.reports,
                stats.observations, stats.selectedReports,
                stats.selectingClientCount);
            for (uint8_t i = 0; i < stats.selectingClientCount; ++i)
                out("matrix selected device %012llx beacon %012llx",
                    (unsigned long long)stats.selectingClientMacs[i],
                    (unsigned long long)stats.bssid);
        }
        const WifiBeaconCaptureStats capture = beaconCapture.getStats();
        const uint64_t scanSpan = micros() >= startUsec ? micros() - startUsec : 0;
        out("matrix capture callback %u mgmt-reject %u short %u nonbeacon %u beacon %u delivered %u parse-reject %u accepted %u target-hits %u scan-span %.3f sec",
            capture.callbackFrames, capture.nonManagementFrames,
            capture.shortFrames, capture.nonBeaconFrames, capture.beaconFrames,
            capture.deliveredFrames, scanParseRejects, scanAccepted, targetHits,
            scanSpan / 1000000.0);
        out("matrix end");
    }

    size_t supporterCount(uint64_t bssid) const {
        size_t count = 0;
        for (const BeaconClaim &claim : claims)
            if (claim.originMac != 0 && claim.bssid == bssid) count++;
        return count;
    }

    bool claimMayVote(const BeaconClaim &claim) const {
        return claim.originMac != 0 &&
            claim.learnedWakeGeneration != 0 &&
            claim.receivedWakeGeneration == wakeGeneration &&
            wakeGeneration - claim.learnedWakeGeneration <=
                claimFreshnessWakes;
    }

    size_t currentSupporterCount(uint64_t bssid) const {
        size_t count = 0;
        for (const BeaconClaim &claim : claims)
            if (claimMayVote(claim) && claim.bssid == bssid) count++;
        return count;
    }

    bool supportersInclude(uint64_t supersetBssid,
                           uint64_t subsetBssid) const {
        for (const BeaconClaim &subset : claims) {
            if (!claimMayVote(subset) || subset.bssid != subsetBssid) continue;
            bool found = false;
            for (const BeaconClaim &candidate : claims)
                if (claimMayVote(candidate) &&
                    candidate.originMac == subset.originMac &&
                    candidate.bssid == supersetBssid) {
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }

    bool retainedSupportersInclude(uint64_t supersetBssid,
                                   uint64_t subsetBssid) const {
        for (const BeaconClaim &subset : claims) {
            if (subset.originMac == 0 || subset.bssid != subsetBssid) continue;
            bool found = false;
            for (const BeaconClaim &candidate : claims)
                if (candidate.originMac == subset.originMac &&
                    candidate.bssid == supersetBssid) {
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }

    uint64_t reportOnlyCandidate(uint64_t homeBssid) const {
        uint64_t best = homeBssid;
        size_t bestSupport = currentSupporterCount(homeBssid);
        size_t bestRetainedSupport = supporterCount(homeBssid);
        size_t bestListeners = listenerCount(homeBssid);
        for (const BeaconInfo &visible : packetLog) {
            if (visible.ssid == 0 || visible.rssi < reportMinRssi ||
                visible.count < minimumCandidatePackets)
                continue;
            const size_t support = currentSupporterCount(visible.ssid);
            const size_t retainedSupport = supporterCount(visible.ssid);
            const size_t listeners = listenerCount(visible.ssid);
            if (!supportersInclude(visible.ssid, homeBssid) ||
                support < bestSupport)
                continue;
            // Rendezvous potential is the primary objective. Local packet
            // quality is only an eligibility gate; it must never defeat a
            // candidate with more peer support. When support is equal, use a
            // deterministic BSSID ordering so devices do not split because
            // their local packet counts differ.
            const bool strongerCurrentSet = support > bestSupport;
            const bool strongerRendezvousPool = support >= bestSupport &&
                listeners > bestListeners;
            const bool corroboratedRetainedSuperset = support >= 2 &&
                support == bestSupport &&
                retainedSupport > bestRetainedSupport &&
                retainedSupportersInclude(visible.ssid, best);
            const bool corroboratedEquivalentSets = support >= 2 &&
                support == bestSupport &&
                retainedSupport == bestRetainedSupport &&
                retainedSupportersInclude(visible.ssid, best) &&
                retainedSupportersInclude(best, visible.ssid) &&
                visible.ssid < best;
            if (strongerCurrentSet || strongerRendezvousPool ||
                corroboratedRetainedSuperset ||
                corroboratedEquivalentSets) {
                best = visible.ssid;
                bestSupport = support;
                bestRetainedSupport = retainedSupport;
                bestListeners = listeners;
            }
        }
        return best;
    }

    uint64_t chooseScoutBeacon(uint64_t homeBssid) const {
        uint64_t result = 0;
        for (const BeaconClaim &claim : claims) {
            if (claim.originMac != deviceMac || claim.bssid == 0 ||
                claim.bssid == homeBssid || claim.rssi < reportMinRssi)
                continue;
            // Deterministic ordering is useful in CSIM and remains fair once
            // later revisions add a rotating cursor for more than one target.
            if (result == 0 || claim.bssid < result) result = claim.bssid;
        }
        return result;
    }

    bool advanceProposal(uint64_t homeBssid, uint64_t candidateBssid) {
        if (candidateBssid == 0 || candidateBssid == homeBssid) {
            spiffsProposalBeacon = 0;
            spiffsProposalAge = 0;
            return false;
        }
        if (spiffsProposalBeacon.read() == candidateBssid)
            spiffsProposalAge = spiffsProposalAge.read() + 1;
        else {
            spiffsProposalBeacon = candidateBssid;
            spiffsProposalAge = 1;
        }
        // A lonely device joining an established pool is not the symmetric
        // pool-to-pool case that needs hysteresis.  Once this wake's healthy
        // exchange shows at least one other fresh supporter for a directly
        // visible candidate, move immediately toward that pool.  Equal or
        // closely matched pools still require two consecutive rounds.
        const bool singletonJoiningPool =
            listenerCount(homeBssid) == 1 &&
            listenerCount(candidateBssid) >= 2;
        if (spiffsProposalAge.read() < 2 && !singletonJoiningPool) return false;

        // The candidate is directly visible in this wake and has already
        // passed the strict-supporter-superset test. Commit while aligned to
        // its clock, then reset the old beacon's timing calibration.
        spiffsBeacon = candidateBssid;
        spiffsProposalBeacon = 0;
        spiffsProposalAge = 0;
        spiffsScoutPhase = 0;
        spiffsScoutBeacon = 0;
        spiffsCurrentGoal = defaultRendezvousUsec;
        spiffsCurrentRep = 0;
        spiffsScale = 1.004;
        return true;
    }

    static void broadScanCallback(const WifiBeaconPacket &packet, void *arg) {
        static_cast<BeaconRendezvousContext *>(arg)->onBroadScan(packet);
    }

    void mergeClaim(uint64_t originMac, uint64_t bssid,
                    uint32_t originGeneration, int8_t rssi) {
        if (originMac == 0 || bssid == 0) return;
        size_t empty = claimTableSize;
        for (size_t i = 0; i < claimTableSize; ++i) {
            BeaconClaim &claim = claims[i];
            if (claim.originMac == originMac && claim.bssid == bssid) {
                if (originGeneration < claim.originGeneration) return;
                if (originGeneration > claim.originGeneration)
                    claim.learnedWakeGeneration = wakeGeneration;
                claim.originGeneration = originGeneration;
                claim.rssi = rssi;
                claim.receivedWakeGeneration = wakeGeneration;
                return;
            }
            if (empty == claimTableSize && claim.originMac == 0) empty = i;
        }
        if (empty == claimTableSize) return;
        claims[empty] = {originMac, bssid, originGeneration, rssi,
                         wakeGeneration, wakeGeneration};
    }

    void onBroadScan(const WifiBeaconPacket &packet) {
        if (packet.length < 32) {
            scanParseRejects++;
            return;
        }
        const uint64_t bssid = beaconBssid(packet.data);
        scanAccepted++;
        if (bssid == targetBeacon) targetHits++;
        size_t i;
        for (i = 0; i < packetLogSize; ++i) {
            if (packetLog[i].ssid == bssid) {
                BeaconInfo &info = packetLog[i];
                recordBeacon(info, packet);
                mergeClaim(deviceMac, bssid, wakeGeneration, packet.rssi);
                return;
            }
        }
        size_t worst = 0;
        for (i = 0; i < packetLogSize; ++i) {
            if (packetLog[i].ssid == 0) { worst = i; break; }
            if (score(packetLog[i]) <= score(packetLog[worst])) worst = i;
        }
        packetLog[worst].ssid = bssid;
        onBroadScan(packet);
    }

    void onReport(const uint8_t *from, const uint8_t *data, int length) {
        const uint64_t reportLocalRx = micros();
        reportRxCount++;
        uint64_t sender = 0;
        if (length >= (int)sizeof(BeaconReportHeader)) {
            BeaconReportHeader peek;
            memcpy(&peek, data, sizeof(peek));
            sender = peek.senderMac;
        }
        if (sender == 0 && from != nullptr)
            for (int i = 0; i < 6; ++i) sender = (sender << 8) | from[i];
        int peerSlot = -1;
        uint64_t radioFrom = 0;
        if (from != nullptr)
            for (int i = 0; i < 6; ++i) radioFrom = (radioFrom << 8) | from[i];
        if (sender != 0 && sender != deviceMac) {
            for (uint8_t i = 0; i < reportSenderCount; ++i)
                if (reportSenders[i] == sender) peerSlot = i;
            if (peerSlot < 0 && reportSenderCount <
                sizeof(reportSenders) / sizeof(reportSenders[0])) {
                peerSlot = reportSenderCount;
                reportSenders[peerSlot] = sender;
                reportSenderRadioFrom[peerSlot] = radioFrom;
                reportSenderFirstUsec[peerSlot] = micros();
                reportSenderCount++;
            }
            if (peerSlot >= 0) {
                reportSenderRaw[peerSlot]++;
                if (radioFrom != 0 && radioFrom != reportSenders[peerSlot])
                    reportSenderRadioMismatch[peerSlot]++;
                reportSenderLastUsec[peerSlot] = micros();
            }
        }
        if (length < (int)sizeof(BeaconReportHeader)) {
            if (peerSlot >= 0) reportSenderShort[peerSlot]++;
            return;
        }
        BeaconReportHeader header;
        memcpy(&header, data, sizeof(header));
        if (header.version != 5) {
            if (peerSlot >= 0) reportSenderBadVersion[peerSlot]++;
            return;
        }
        // Validate the complete framing before changing any association.
        if (!validReportLength(header, (size_t)length)) {
            reportBadLength++;
            return;
        }
        // One sample per peer per exchange keeps serial output bounded.
        if (peerSlot >= 0 && reportSenderValid[peerSlot] == 0)
        out("report-clock-rx sender %012llx incarnation %08x wake %u packet %u local-rx %llu bssid %012llx clock-ms-low %u start-delta-ms %d planned-end-delta-ms %d valid %u",
            (unsigned long long)header.senderMac, header.incarnation,
            header.wakeGeneration, header.packetSequence,
            (unsigned long long)reportLocalRx,
            (unsigned long long)reportClockBssid(header), header.clockMsLow,
            (int)header.exchangeStartDeltaMs, (int)header.plannedEndDeltaMs,
            header.timingValid == 1 ? 1U : 0U);
        const bool refreshed = mergeAssociation(header.senderMac, header.selectedBeacon,
                         header.wakeGeneration, 0, true);
        if (peerSlot >= 0 && refreshed) reportSenderAssociationRefresh[peerSlot]++;
        const size_t claimBytesAvailable = length - sizeof(header);
        const size_t available = claimBytesAvailable /
            sizeof(BeaconClaimEntry);
        const size_t count = min((size_t)header.claimCount,
                                 min(available, reportMaxClaims));
        reportRxClaimCount += count;
        if (peerSlot >= 0) reportSenderClaimEntries[peerSlot] += count;
        sender = header.senderMac;
        if (sender == 0 && from != nullptr)
            for (int i = 0; i < 6; ++i) sender = (sender << 8) | from[i];
        if (sender == deviceMac) return;
        reportValidRxCount++;
        if (peerSlot >= 0) reportSenderValid[peerSlot]++;
        bool knownSender = false;
        for (uint8_t i = 0; i < reportSenderCount; ++i)
            if (reportSenders[i] == sender) knownSender = true;
        if (!knownSender && reportSenderCount <
            sizeof(reportSenders) / sizeof(reportSenders[0]))
            reportSenders[reportSenderCount++] = sender;
        for (size_t i = 0; i < count; ++i) {
            BeaconClaimEntry entry;
            memcpy(&entry, data + sizeof(header) +
                   i * sizeof(entry), sizeof(entry));
            mergeClaim(entry.originMac, entry.bssid,
                       entry.originGeneration, entry.rssi);
            size_t slot = 0;
            for (; slot < remoteStatsSize; ++slot) {
                if (remoteStats[slot].bssid == entry.bssid) break;
                if (remoteStats[slot].bssid == 0) break;
            }
            if (slot == remoteStatsSize) continue;
            RemoteBeaconStats &stats = remoteStats[slot];
            stats.bssid = entry.bssid;
            stats.reports++;
            stats.observations++;
            stats.lastRssi = entry.rssi;
            stats.strongestRssi = max(stats.strongestRssi, entry.rssi);
            stats.lastSender = sender;
            if (entry.originMac == sender &&
                entry.bssid == header.selectedBeacon) {
                stats.selectedReports++;
                bool knownClient = false;
                for (uint8_t j = 0; j < stats.selectingClientCount; ++j)
                    if (stats.selectingClientMacs[j] == sender)
                        knownClient = true;
                if (!knownClient && stats.selectingClientCount <
                    sizeof(stats.selectingClientMacs) /
                    sizeof(stats.selectingClientMacs[0])) {
                    stats.selectingClientMacs[stats.selectingClientCount++] =
                        sender;
                }
            }
        }
        const size_t associationOffset = sizeof(header) +
            count * sizeof(BeaconClaimEntry);
        const size_t associationAvailable = length >= (int)associationOffset ?
            (length - associationOffset) / sizeof(BeaconAssociationEntry) : 0;
        const size_t associationCount = min(
            (size_t)header.associationCount,
            min(associationAvailable, reportMaxAssociations));
        for (size_t i = 0; i < associationCount; ++i) {
            BeaconAssociationEntry entry;
            memcpy(&entry, data + associationOffset +
                   i * sizeof(entry), sizeof(entry));
            mergeAssociation(entry.originMac, entry.selectedBeacon,
                             entry.originGeneration, entry.ageCycles);
        }
    }

    void dumpExchangePeers() const {
        out("report-framing bad-length %u", reportBadLength);
        out("association-merge attempts %u accepted %u rejected %u invalid %u older-generation %u not-fresher %u table-full %u",
            associationMergeAttempts, associationMergeAccepted,
            associationMergeAttempts - associationMergeAccepted,
            associationMergeInvalid, associationMergeOlder,
            associationMergeNotFresher, associationMergeFull);
        size_t knownPeers = 0;
        for (const BeaconClaim &claim : claims) {
            if (claim.originMac == 0 || claim.originMac == deviceMac) continue;
            bool duplicate = false;
            for (const BeaconClaim &prior : claims)
                if (&prior < &claim && prior.originMac == claim.originMac)
                    duplicate = true;
            if (!duplicate) knownPeers++;
        }
        size_t heardPeers = 0;
        for (uint8_t i = 0; i < reportSenderCount; ++i)
            if (reportSenders[i] != deviceMac && reportSenderValid[i] != 0)
                heardPeers++;
        out("espnow heard %u/%u known peers", (unsigned)heardPeers,
            (unsigned)knownPeers);
        for (uint8_t i = 0; i < reportSenderCount; ++i)
            out("espnow peer %012llx raw %u valid %u first %llu last %llu",
                (unsigned long long)reportSenders[i], reportSenderRaw[i],
                reportSenderValid[i],
                (unsigned long long)reportSenderFirstUsec[i],
                (unsigned long long)reportSenderLastUsec[i]);
        for (uint8_t i = 0; i < reportSenderCount; ++i)
            out("espnow summary origin %012llx radio-from %012llx frames %u valid %u short %u bad-version %u association-refresh %u claim-entries %u radio-mismatch %u first %llu last %llu",
                (unsigned long long)reportSenders[i],
                (unsigned long long)reportSenderRadioFrom[i],
                reportSenderRaw[i], reportSenderValid[i],
                reportSenderShort[i], reportSenderBadVersion[i],
                reportSenderAssociationRefresh[i], reportSenderClaimEntries[i],
                reportSenderRadioMismatch[i],
                (unsigned long long)reportSenderFirstUsec[i],
                (unsigned long long)reportSenderLastUsec[i]);
        for (const BeaconClaim &claim : claims) {
            if (claim.originMac == 0 || claim.originMac == deviceMac) continue;
            bool duplicate = false;
            for (const BeaconClaim &prior : claims)
                if (&prior < &claim && prior.originMac == claim.originMac)
                    duplicate = true;
            if (duplicate) continue;
            bool heard = false;
            for (uint8_t i = 0; i < reportSenderCount; ++i)
                if (reportSenders[i] == claim.originMac &&
                    reportSenderValid[i] != 0) heard = true;
            if (!heard)
                out("espnow missing peer %012llx",
                    (unsigned long long)claim.originMac);
        }
    }

    void publishReport() {
        uint8_t buffer[sizeof(BeaconReportHeader) +
                       reportMaxClaims * sizeof(BeaconClaimEntry) +
                       reportMaxAssociations *
                           sizeof(BeaconAssociationEntry)] = {};
        mergeAssociation(deviceMac, spiffsBeacon.read(), wakeGeneration,
                         0, true);
        BeaconReportHeader header = {};
        header.version = 5;
        header.senderMac = deviceMac;
        header.selectedBeacon = spiffsBeacon.read();
        header.wakeGeneration = wakeGeneration;
        header.packetSequence = reportSequence++;
        header.incarnation = incarnation;
        // Advertise the actual target observation, never substitute home for
        // an unobserved scout target. Missing or out-of-range timing is invalid.
        for (const BeaconInfo &info : packetLog) {
            if (info.ssid != targetBeacon || info.count == 0) continue;
            setReportTiming(header, info.ssid, info.ts, info.seen2,
                            espNowStartUsec, espNowEndUsec);
            if (header.packetSequence == 0)
            out("report-clock-tx incarnation %08x wake %u packet %u bssid %012llx tsf %llu local-rx %llu valid %u",
                incarnation, wakeGeneration, header.packetSequence,
                (unsigned long long)info.ssid, (unsigned long long)info.ts,
                (unsigned long long)info.seen2, (unsigned)header.timingValid);
            break;
        }
        memcpy(buffer, &header, sizeof(header));
        size_t count = 0;
        // Rotate through the longer-lived claim table. Repeated 5 Hz packets
        // eventually advertise the complete table without exceeding ESP-NOW's
        // conservative packet limit.
        size_t visited = 0;
        while (visited < claimTableSize && count < reportMaxClaims) {
            const size_t i = claimTransmitCursor++ % claimTableSize;
            visited++;
            const BeaconClaim &claim = claims[i];
            if (claim.originMac == 0 || claim.rssi < reportMinRssi ||
                claim.learnedWakeGeneration == 0 ||
                wakeGeneration - claim.learnedWakeGeneration >
                    claimFreshnessWakes)
                continue;
            BeaconClaimEntry entry = {claim.originMac, claim.bssid,
                                      claim.originGeneration, claim.rssi};
            memcpy(buffer + sizeof(header) + count * sizeof(entry),
                   &entry, sizeof(entry));
            count++;
        }
        header.claimCount = (uint8_t)count;
        size_t associationCount = 0;
        size_t associationVisited = 0;
        const size_t associationOffset = sizeof(header) +
            count * sizeof(BeaconClaimEntry);
        while (associationVisited < associationTableSize &&
               associationCount < reportMaxAssociations) {
            const size_t i = associationTransmitCursor++ %
                associationTableSize;
            associationVisited++;
            const BeaconAssociation &association = associations[i];
            if (association.originMac == 0) continue;
            const uint32_t age = associationAgeCycles(association);
            if (age > associationFreshnessCycles) continue;
            BeaconAssociationEntry entry = {
                association.originMac, association.selectedBeacon,
                association.originGeneration,
                (uint16_t)min(age, (uint32_t)UINT16_MAX)};
            memcpy(buffer + associationOffset + associationCount *
                   sizeof(entry), &entry, sizeof(entry));
            associationCount++;
        }
        header.associationCount = (uint8_t)associationCount;
        memcpy(buffer, &header, sizeof(header));
        privMux.send("BRPT", buffer, sizeof(header) +
                     count * sizeof(BeaconClaimEntry) +
                     associationCount * sizeof(BeaconAssociationEntry));
        reportTxCount++;
    }

    bool exchangeHealthy() const {
        // Membership is unknown, so completeness cannot mean hearing every
        // client. Use only locally observable transport health.
        const uint32_t successes = privMux.getSendSuccesses();
        const uint32_t failures = privMux.getSendFailures();
        const bool txHealthy = reportTxCount >= 20 && successes >= 20 &&
            failures <= 5;
        return txHealthy && reportValidRxCount >= 3 && reportSenderCount > 0;
    }

    void configureBeaconRadio() {
#ifndef CSIM
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_wifi_init(&cfg);
        esp_wifi_start();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_disconnect();
        esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
#endif
    }

    void startOneShotCapture() {
        // Every wake is a broad scan. The target beacon is tracked as one of
        // the scan entries and the most recent target packet is used for
        // clock alignment when the window closes.
        beaconCapture.setCallback(broadScanCallback, this);
        beaconCapture.start();
    }

    int resetReason() const {
#ifdef CSIM
        return rtc_get_reset_reason(0);
#else
        return esp_rom_get_reset_reason(0);
#endif
    }

    int bestBeaconIndex() const {
        int best = 0;
        for (size_t i = 0; i < packetLogSize; ++i)
            if (packetLog[i].ssid != 0 && score(packetLog[i]) >= score(packetLog[best]))
                best = (int)i;
        return best;
    }

    static bool louderStartupBeacon(const BeaconInfo &a,
                                    const BeaconInfo &b) {
        if (a.rssi != b.rssi) return a.rssi > b.rssi;
        if (a.count != b.count) return a.count > b.count;
        return a.ssid < b.ssid;
    }

    int randomStartupBeaconIndex() const {
        int candidates[packetLogSize];
        size_t count = 0;
        for (size_t i = 0; i < packetLogSize; ++i) {
            const BeaconInfo &info = packetLog[i];
            if (info.ssid == 0 || info.rssi < reportMinRssi ||
                info.count < minimumCandidatePackets)
                continue;
            candidates[count++] = (int)i;
        }
        for (size_t i = 1; i < count; ++i) {
            const int value = candidates[i];
            size_t j = i;
            while (j > 0 && louderStartupBeacon(packetLog[value],
                                                  packetLog[candidates[j - 1]])) {
                candidates[j] = candidates[j - 1];
                --j;
            }
            candidates[j] = value;
        }
        const size_t topCount = min(count, testStartupTopN);
        if (topCount == 0) return bestBeaconIndex();
        uint32_t randomValue = 0;
#ifdef CSIM
        randomValue = (uint32_t)rand();
#else
        randomValue = esp_random();
#endif
        out("startup-random candidates %d top %d", (int)count,
            (int)topCount);
        for (size_t i = 0; i < topCount; ++i)
            out("startup-random candidate %d beacon %012llx rssi %d packets %d",
                (int)i, (unsigned long long)packetLog[candidates[i]].ssid,
                packetLog[candidates[i]].rssi, packetLog[candidates[i]].count);
        const int selected = candidates[randomValue % topCount];
        out("startup-random selected beacon %012llx rank %u of %u",
            (unsigned long long)packetLog[selected].ssid,
            (unsigned)((randomValue % topCount) + 1), (unsigned)topCount);
        return selected;
    }

    void out(const char *fmt, ...) const {
        va_list args;
        va_start(args, fmt);
#ifdef CSIM
        printf("%012llx ", (unsigned long long)context->mac);
        const double displaySeconds =
            (sim().bootTimeUsec + micros()) / 1000000.0;
#else
        const double displaySeconds = millis() / 1000.0;
#endif
        printf("%09.3f ", displaySeconds);
        vprintf(fmt, args);
        printf("\n");
        va_end(args);
    }

public:
    explicit BeaconRendezvousContext(uint64_t address = 0) :
        BeaconRendezvousContextBase(address) {
#ifdef CSIM
        // Keep the mapping deterministic and explicit: changing the number of
        // contexts or their MACs does not alter the RF environment definitions.
        const uint8_t environmentId = (uint8_t)((address & 0xff) - 1);
        beaconEnvironment.addDestination(&beaconCapture, wifiChannel,
                                         environmentId);
        currentContext = &defaultContext;
#endif
    }

#ifdef CSIM
    void setup() override {
#else
    void setup() {
#endif
#ifdef CSIM
        CSIM_ASSERT(currentContext == context);
#endif
        memset(packetLog, 0, sizeof(packetLog));
        memset(remoteStats, 0, sizeof(remoteStats));
        reportSequence = 0;
        reportTxCount = 0;
        reportRxCount = 0;
        reportRxClaimCount = 0;
        reportValidRxCount = 0;
        reportBadLength = 0;
        associationMergeAttempts = associationMergeAccepted = 0;
        associationMergeInvalid = associationMergeOlder = 0;
        associationMergeNotFresher = associationMergeFull = 0;
        memset(reportSenders, 0, sizeof(reportSenders));
        memset(reportSenderRadioFrom, 0, sizeof(reportSenderRadioFrom));
        memset(reportSenderRaw, 0, sizeof(reportSenderRaw));
        memset(reportSenderValid, 0, sizeof(reportSenderValid));
        memset(reportSenderFirstUsec, 0, sizeof(reportSenderFirstUsec));
        memset(reportSenderLastUsec, 0, sizeof(reportSenderLastUsec));
        memset(reportSenderShort, 0, sizeof(reportSenderShort));
        memset(reportSenderBadVersion, 0, sizeof(reportSenderBadVersion));
        memset(reportSenderAssociationRefresh, 0,
               sizeof(reportSenderAssociationRefresh));
        memset(reportSenderClaimEntries, 0, sizeof(reportSenderClaimEntries));
        memset(reportSenderRadioMismatch, 0, sizeof(reportSenderRadioMismatch));
        reportSenderCount = 0;
        scanParseRejects = 0;
        scanAccepted = 0;
        targetHits = 0;
        espNowStarted = false;
        espNowStartUsec = 0;
        espNowEndUsec = 0;
        beaconReceivedAtUsec = 0;
        loopCount = 0;
        startUsec = micros();
        nextReportUsec = startUsec;
        deviceMac =
#ifdef CSIM
            context->mac;
#else
            ESP.getEfuseMac();
#endif
        SPIFFSVariableESP32Base::begin();
        loadClaims();
        loadAssociations();
        wakeGeneration = spiffsClaimGeneration.read() + 1;
        incarnation = spiffsIncarnation.read();
        if (incarnation == 0 || wakeGeneration == 0) {
#ifdef CSIM
            incarnation = (uint32_t)rand();
#else
            incarnation = esp_random();
#endif
            if (incarnation == 0) incarnation = 1;
            spiffsIncarnation = incarnation;
            if (wakeGeneration == 0) wakeGeneration = 1;
        }
        spiffsClaimGeneration = wakeGeneration;
        out("report-identity incarnation %08x wake %u wire-version 5 max-packet-bytes 199",
            incarnation, wakeGeneration);
        const uint64_t homeBeacon = spiffsBeacon.read();
        targetBeacon = homeBeacon;
        scoutWake = false;
        scoutRendezvousWake = false;
        if (spiffsScoutPhase.read() == 1 &&
            spiffsScoutBeacon.read() != 0) {
            targetBeacon = spiffsScoutBeacon.read();
            scoutWake = true;
            scoutRendezvousWake = true;
            spiffsScoutPhase = 0;
        } else if (homeBeacon != 0 &&
                   wakeGeneration % scoutIntervalWakes == 0) {
            const uint64_t scoutBeacon = chooseScoutBeacon(homeBeacon);
            if (scoutBeacon != 0) {
                targetBeacon = scoutBeacon;
                scoutWake = true;
                spiffsScoutBeacon = scoutBeacon;
                // Revisit on the following wake, after this wake aligns the
                // sleep deadline to the scout beacon's clock.
                spiffsScoutPhase = 1;
            }
        }
#ifdef CSIM
        const double setupSeconds =
            (sim().bootTimeUsec + micros()) / 1000000.0;
#else
        const double setupSeconds = millis() / 1000.0;
#endif
        printf("%09.3f setup() %s waiting for %llx\n", setupSeconds,
               scoutRendezvousWake ? "scout-rendezvous" :
               (scoutWake ? "scout-acquire" : "home"),
               (unsigned long long)targetBeacon);
#ifndef CSIM
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        esp_task_wdt_config_t c;
        c.timeout_ms = 25 * 1000;
        c.idle_core_mask = 0x1;
        c.trigger_panic = true;
        esp_task_wdt_deinit();
        esp_task_wdt_init(&c);
        esp_task_wdt_add(NULL);
#endif
#endif
        // Configure before ESPNowMux initializes, matching the existing
        // hardware radio ordering used by beacon capture.
        configureBeaconRadio();
#ifndef CSIM
        // Keep ESP-NOW peer setup on the same fixed channel as promiscuous
        // beacon capture.  ESPNowMux otherwise defaults to channel 1.
        privMux.defaultChannel = wifiChannel;
#endif
        // Rendezvous reports are gossip: every nearby device must receive
        // every report.  Do not let ESPNowMux lock the BRPT route to the
        // first peer heard.
        privMux.alwaysBroadcast = true;
        startOneShotCapture();
    }

#ifdef CSIM
    void loop() override {
#else
    void loop() {
#endif
#ifdef CSIM
        CSIM_ASSERT(currentContext == context);
#endif
        loopCount++;
        esp_task_wdt_reset();
        const uint64_t nowUsec = micros();
        if (!espNowStarted && nowUsec - startUsec >= beaconSamplingWindowUsec) {
            // Hardware workaround: Jim observed that initializing ESP-NOW
            // before beacon acquisition subtly reduced, and sometimes nearly
            // eliminated, promiscuous Wi-Fi monitor callbacks. Preserve this
            // beacon-only acquisition phase before the mux initializes Wi-Fi/
            // ESP-NOW. The underlying driver interaction remains unproven;
            // do not move initialization earlier without a hardware regression
            // test measuring beacon callbacks before and after initialization.
            privMux.registerReadCallback("BRPT", [this](const uint8_t *from,
                                                         const uint8_t *data,
                                                         int length) {
                onReport(from, data, length);
            });
            espNowStarted = true;
            // Use the loop timestamp captured above; sampling micros() here
            // would make nowUsec - espNowStartUsec wrap as an unsigned value
            // on the transition iteration.
            espNowStartUsec = nowUsec;
            espNowEndUsec = nowUsec + exchangeWindowUsec;
            nextReportUsec = espNowStartUsec;
            out("ESP-NOW exchange phase started after beacon-only survey");
            delay(1);
            return;
        }
        if (!espNowStarted || nowUsec < espNowEndUsec) {
            if (espNowStarted && nowUsec >= nextReportUsec) {
                publishReport();
                nextReportUsec = nowUsec + reportPeriodUsec;
            }
            delay(1);
            return;
        }

        BeaconInfo result = {};
        BeaconInfo *beacon = nullptr;
        for (BeaconInfo &info : packetLog) {
            if (info.ssid == targetBeacon && info.count != 0) {
                result = info;
                beacon = &result;
                break;
            }
        }
        if (result.count == 0) {
            out("Target beacon not received in broad scan, picking best observed beacon");
            const bool uninitializedStartup = spiffsBeacon.read() == 0;
            const int best = uninitializedStartup ?
                randomStartupBeaconIndex() : bestBeaconIndex();
            out("best beacon: %02d %012llx %3d %6d %016llx %016llx", best,
                (unsigned long long)packetLog[best].ssid, packetLog[best].rssi,
                packetLog[best].count, (unsigned long long)packetLog[best].seen,
                (unsigned long long)packetLog[best].seen2);
            beacon = &packetLog[best];
            if (!scoutWake) spiffsBeacon = beacon->ssid;
        }

        if (resetReason() != 5) {
            spiffsCurrentGoal = defaultRendezvousUsec;
            spiffsCurrentRep = 0;
        }
        uint64_t goal = spiffsCurrentGoal;
        if (goal == 0) {
            goal = defaultRendezvousUsec;
            spiffsCurrentGoal = goal;
            spiffsCurrentRep = 0;
        }
        const uint64_t packetRxLocalUsec = beacon->seen2;
        const uint64_t packetRxTime = packetRxLocalUsec - startUsec;
        int beaconDistance = (int)(beacon->ts % goal);
        if (beaconDistance > (int)(goal / 2)) beaconDistance -= goal;
        int espDistance = (int)(packetRxTime % goal);
        if (espDistance > (int)(goal / 2)) espDistance -= goal;
        const int usecLate = beaconDistance - espDistance;
        const uint64_t previousSleep = spiffsSleepTime.read();
        const float percentLate = previousSleep ? abs(100.0 * usecLate / previousSleep) : 0.0;

        // Project the local ESP-NOW window onto the tracked beacon's TSF
        // clock. This makes two boards' logs directly comparable when they
        // are visiting the same beacon, even though their local micros()
        // clocks and serial log timestamps are unrelated.
        uint64_t beaconExchangeStart = 0, beaconExchangeEnd = 0;
        const bool projectionValid = beacon->count != 0 &&
            projectBeaconTsf(beacon->ts, packetRxLocalUsec,
                             espNowStartUsec, beaconExchangeStart) &&
            projectBeaconTsf(beacon->ts, packetRxLocalUsec,
                             espNowEndUsec, beaconExchangeEnd);
        out("beacon-clock-observation target %012llx tsf %llu local-rx %llu exchange-start-local %llu planned-end-local %llu completion-local %llu valid %u",
            (unsigned long long)beacon->ssid, (unsigned long long)beacon->ts,
            (unsigned long long)packetRxLocalUsec,
            (unsigned long long)espNowStartUsec,
            (unsigned long long)espNowEndUsec,
            (unsigned long long)nowUsec, projectionValid ? 1U : 0U);
        if (projectionValid) {
            const uint64_t beaconCycle = beaconExchangeStart / goal;
            const uint64_t beaconCycleStart = beaconCycle * goal;
            const uint64_t beaconCycleStop = beaconCycleStart + goal;
            // Half-open intervals ending at the boundary do not cross it.
            const unsigned crossesBoundary = beaconExchangeEnd > beaconCycleStop ? 1U : 0U;
            out("beacon-clock target %012llx tsf-packet %llu exchange %llu-%llu cycle %llu start %llu stop %llu crosses-boundary %u",
                (unsigned long long)beacon->ssid,
                (unsigned long long)beacon->ts,
                (unsigned long long)beaconExchangeStart,
                (unsigned long long)beaconExchangeEnd,
                (unsigned long long)beaconCycle,
                (unsigned long long)beaconCycleStart,
                (unsigned long long)beaconCycleStop,
                crossesBoundary);
        }

        if (resetReason() == 5 || loopCount > 1) {
            out("slept %lld (%.1fs) rssi %d goal %.2fs rep %d beacon offset %d esp offset %d difference %d late (%.3f%%) scale %f",
                (long long)previousSleep, previousSleep / 1000000.0, beacon->rssi,
                goal / 1000000.0, spiffsCurrentRep.read(), beaconDistance,
                espDistance, usecLate, percentLate, spiffsScale.read());
            if (previousSleep > 0) {
                spiffsScale = spiffsScale - (1.0 * usecLate / previousSleep) * 0.3;
                spiffsScale = min(1.1F, max(0.9F, spiffsScale.read()));
            }
            spiffsCurrentRep = spiffsCurrentRep + 1;
        } else {
            spiffsScale = 1.004;
        }

        goal = spiffsCurrentGoal;
        uint64_t timeToGoal = goal - (beacon->ts % goal);
        if (timeToGoal % goal < goal / 2) timeToGoal += goal;
        uint64_t awakeSincePacket = micros() - startUsec - packetRxTime;
        while (timeToGoal <= awakeSincePacket) timeToGoal += goal;
        uint64_t sleepUsec =
            (timeToGoal - awakeSincePacket) * spiffsScale;
        const uint64_t homeBssid = spiffsBeacon.read();
        // The older "late" diagnostic is relative to the beacon that
        // anchored this sleep calculation. During a scout wake that may not
        // be the persisted home beacon, so record both identities and the
        // complete deadline relationship for offline analysis.
        out("rendezvous timing mode %s home %012llx target %012llx anchor %012llx target-hit %u beacon-rx-usec %llu exchange-usec %llu-%llu awake-after-beacon-usec %llu deadline-usec %llu sleep-usec %llu late-usec %d late-pct %.3f",
            scoutRendezvousWake ? "scout-rendezvous" :
            (scoutWake ? "scout-acquire" : "home"),
            (unsigned long long)homeBssid,
            (unsigned long long)targetBeacon,
            (unsigned long long)beacon->ssid,
            (unsigned)targetHits,
            (unsigned long long)packetRxTime,
            (unsigned long long)espNowStartUsec,
            (unsigned long long)espNowEndUsec,
            (unsigned long long)awakeSincePacket,
            (unsigned long long)timeToGoal,
            (unsigned long long)sleepUsec,
            usecLate, percentLate);
        const bool healthyExchange = exchangeHealthy();
        const uint64_t candidateBssid = healthyExchange ?
            reportOnlyCandidate(homeBssid) : homeBssid;
        bool switched = false;
        if (healthyExchange)
            switched = advanceProposal(homeBssid, candidateBssid);
        else {
            // Proposal rounds must be consecutive and healthy. Treat packet
            // loss as delayed convergence, never as evidence to move.
            spiffsProposalBeacon = 0;
            spiffsProposalAge = 0;
        }
        out("gossip %s exchange %s claims %d home %012llx listeners %d visibility current %d retained %d proposal %012llx listeners %d visibility current %d retained %d age %d espnow tx %u ok %u fail %u busy %u rawrx %u rx %u valid %u peers %u claims %u scan accepted %u target %u parse-reject %u channel %d exchange-usec %llu-%llu last-rx %012llx%s",
            scoutRendezvousWake ? "scout-rendezvous" :
            (scoutWake ? "scout-acquire" : "home"),
            healthyExchange ? "healthy" : "incomplete",
            (int)claimCount(), (unsigned long long)homeBssid,
            (int)listenerCount(homeBssid),
            (int)currentSupporterCount(homeBssid),
            (int)supporterCount(homeBssid),
            (unsigned long long)candidateBssid,
            (int)listenerCount(candidateBssid),
            (int)currentSupporterCount(candidateBssid),
            (int)supporterCount(candidateBssid), spiffsProposalAge.read(),
            reportTxCount, privMux.getSendSuccesses(),
            privMux.getSendFailures(), privMux.getSendBusyDrops(),
            privMux.getReceiveCallbacks(),
            reportRxCount, reportValidRxCount, reportSenderCount,
            reportRxClaimCount, scanAccepted, targetHits,
            scanParseRejects,
            wifiChannel,
            (unsigned long long)espNowStartUsec,
            (unsigned long long)espNowEndUsec,
            (unsigned long long)privMux.getLastReceiveMac(),
            switched ? " SWITCH" : "");
        dumpExchangePeers();
        dumpAssociationTable(homeBssid);
        dumpDeviceBeaconMatrix();
        out("deep sleep %.1f sec, goal %.1f scale %f", sleepUsec / 1000000.0,
            goal / 1000000.0, spiffsScale.read());
        fflush(stdout);
#ifndef CSIM
        uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
#endif
        awakeSincePacket = micros() - startUsec - packetRxTime;
        while (timeToGoal <= awakeSincePacket) timeToGoal += goal;
        sleepUsec = (timeToGoal - awakeSincePacket) * spiffsScale;
        spiffsSleepTime = sleepUsec;
        // Ensure observations made late in this wake are advertised at least
        // once before sleeping. Periodic reports alone can otherwise miss a
        // one-shot capture followed immediately by deep sleep.
        publishReport();
        maybeResetAfterStableReunion(homeBssid, healthyExchange);
        saveClaims();
        saveAssociations();
        beaconCapture.stop();
        esp_sleep_enable_timer_wakeup(sleepUsec);
        esp_deep_sleep_start();
    }
};

#ifdef CSIM
struct ContextFleet {
    BeaconRendezvousContext *contexts[CONTEXT_COUNT] = {};

    ContextFleet() {
        for (uint8_t i = 0; i < CONTEXT_COUNT; ++i)
            contexts[i] = new BeaconRendezvousContext(
                0xddeeff000001ULL + i);
    }
};
static ContextFleet rendezvousFleet;
#else
BeaconRendezvousContext rendezvous0(0xddeeff000001ULL);
#endif

void setup() {
#ifndef CSIM
    rendezvous0.setup();
#endif
}

void loop() {
#ifndef CSIM
    rendezvous0.loop();
#else
    delay(1);
#endif
}
