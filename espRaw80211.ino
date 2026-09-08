#include "jimlib.h"
#include "espNowMux.h"
#include "raw80211Capture.h"
#include "rendezvousTiming.h"
#include "beaconReport.h"
#include "originEpoch.h"
#include "macIdentity.h"
#include "rendezvousPlanner.h"
#include "singletonJoinPolicy.h"
#include "rendezvousExecutor.h"
#include "testSwarmConfig.h"
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
              2 * sizeof(BeaconClaimEntry) +
              3 * sizeof(BeaconAssociationEntry) + 4 <= ESPNowMux::physicalPacketBytes,
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
    uint32_t originEpoch = 0;
};

struct BeaconAssociation {
    uint64_t originMac = 0;
    uint64_t selectedBeacon = 0;
    uint32_t originGeneration = 0;
    uint32_t ageCycles = 0;
    uint64_t storedAtUsec = 0;
    uint32_t originEpoch = 0;
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
    static constexpr size_t reportMaxClaims = 2;
    static constexpr size_t reportMaxAssociations = 3;
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
    // ARTIFICIAL test oracle, not knowledge available to a real deployment.
    // Counts unlogged boards too; never derive this from USB connections.
    // Reset after ten qualified logical rounds, then a three-round delay.
    static constexpr size_t testClusterSize = ARTIFICIAL_TEST_SWARM_BOARD_COUNT;
    SPIFFSVariable<uint32_t> spiffsTestSwarmCount{"/testSwarmCount", 0};
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
    SPIFFSVariable<uint64_t> spiffsRoundElapsed{"/roundElapsed7", 0};
    SPIFFSVariable<uint32_t> spiffsExchangeSequence{"/exchangeSeq7", 0};
    SPIFFSVariable<uint64_t> spiffsScoutCursor{"/scoutCursor7", 0};
    SPIFFSVariable<uint32_t> spiffsLastScoutRound{"/lastScout7", 0};
    SPIFFSVariable<int> spiffsRoundHealthy{"/roundHealthy7", 0};
    SPIFFSVariable<uint64_t> spiffsRoundHome{"/roundHome7", 0};
    RendezvousExecutor::RoundClock roundClock;
    RendezvousPlanner::Plan<4> executionPlan{1000000};
    RendezvousExecutor::Coverage coverage[4] = {};
    size_t executionInterval = 0;
    bool executionPlanned = false, exchangeActive = false;
    uint32_t exchangeSequence = 0;
    uint64_t plannedHome = 0;
    uint64_t plannedPeriod = 0;
    uint64_t nextPlanUsec = 0;
    uint32_t intervalRawStart = 0, intervalOkStart = 0, intervalFailStart = 0;
    uint32_t coverageFailures[4] = {};
    SPIFFSVariable<string> spiffsClaims{"/claims6", ""};
    SPIFFSVariable<string> spiffsAssociations{"/associations6", ""};
    SPIFFSVariable<string> spiffsOrigins{"/origins6", ""};
    OriginEpoch originEpochs[32] = {};
    uint32_t epochRejected = 0;
    SPIFFSVariable<int> spiffsScoutPhase{"/scoutPhase", 0};
    SPIFFSVariable<uint64_t> spiffsScoutBeacon{"/scoutBeacon", 0};
    SPIFFSVariable<uint64_t> spiffsProposalBeacon{"/proposal9", 0};
    SPIFFSVariable<uint64_t> spiffsProposalHome{"/proposalHome9", 0};
    SPIFFSVariable<uint32_t> spiffsProposalActRound{"/proposalAct9", 0};
    SPIFFSVariable<uint32_t> spiffsProposalMembers{"/proposalMembers9", 0};
    SPIFFSVariable<uint64_t> spiffsCredibilityHome{"/credHome9", 0};
    SPIFFSVariable<uint32_t> spiffsHomeCredibility{"/homeCred9", 0};
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
        spiffsOrigins = string("");
        memset(originEpochs, 0, sizeof(originEpochs));
        spiffsClaimGeneration = (uint32_t)0;
        spiffsIncarnation = (uint32_t)0;
        spiffsRoundElapsed = (uint64_t)0;
        spiffsExchangeSequence = (uint32_t)0;
        spiffsLastScoutRound = (uint32_t)0;
        spiffsRoundHealthy = 0;
        spiffsRoundHome = (uint64_t)0;
        spiffsScoutPhase = 0;
        spiffsScoutBeacon = (uint64_t)0;
        spiffsProposalBeacon = (uint64_t)0;
        spiffsProposalHome = (uint64_t)0;
        spiffsProposalActRound = (uint32_t)0;
        spiffsProposalMembers = (uint32_t)0;
        spiffsCredibilityHome = (uint64_t)0;
        spiffsHomeCredibility = (uint32_t)0;
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

    bool epochMatches(uint64_t origin, uint32_t epoch) const {
        for (const OriginEpoch &entry : originEpochs)
            if (entry.origin == origin) return epoch != 0 && entry.epoch == epoch;
        return false;
    }

    bool acceptOrigin(uint64_t origin, uint32_t epoch, bool direct) {
        // Relays must never replace or refresh this board's own evidence.
        if (origin == deviceMac && (!direct || epoch != incarnation)) {
            epochRejected++;
            return false;
        }
        const EpochDecision decision = acceptEpoch(originEpochs, 32, origin, epoch, direct);
        if (decision == EpochDecision::rejected) {
            epochRejected++;
            return false;
        }
        if (decision == EpochDecision::introduced || decision == EpochDecision::replaced) {
            for (BeaconClaim &claim : claims)
                if (claim.originMac == origin && claim.originEpoch != epoch) claim = {};
            for (BeaconAssociation &association : associations)
                if (association.originMac == origin && association.originEpoch != epoch)
                    association = {};
            out("origin-incarnation origin %012llx epoch %08x direct %u replaced %u",
                (unsigned long long)origin, epoch, direct ? 1U : 0U,
                decision == EpochDecision::replaced ? 1U : 0U);
        }
        return true;
    }

    void loadOrigins() {
        memset(originEpochs, 0, sizeof(originEpochs));
        const string encoded = spiffsOrigins.read();
        size_t offset = 0;
        for (OriginEpoch &entry : originEpochs) {
            unsigned long long origin = 0;
            unsigned epoch = 0;
            int consumed = 0;
            if (sscanf(encoded.c_str() + offset, "%llx,%x;%n", &origin,
                       &epoch, &consumed) != 2 || consumed <= 0) break;
            entry.origin = origin;
            entry.epoch = epoch;
            offset += consumed;
            if (offset >= encoded.size()) break;
        }
    }

    void saveOrigins() {
        string encoded;
        char record[40];
        for (const OriginEpoch &entry : originEpochs) {
            if (!entry.origin) continue;
            snprintf(record, sizeof(record), "%llx,%x;",
                     (unsigned long long)entry.origin, entry.epoch);
            encoded += record;
        }
        spiffsOrigins = encoded;
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
            unsigned epoch = 0;
            int consumed = 0;
            int fields = sscanf(encoded.c_str() + offset,
                "%llx,%llx,%x,%d,%x,%x;%n", &origin, &bssid, &generation,
                &rssi, &learned, &epoch, &consumed);
            if (fields != 6 || consumed <= 0) break;
            if (epochMatches(origin, epoch))
            claims[slot++] = {(uint64_t)origin, (uint64_t)bssid,
                              (uint32_t)generation, (int8_t)rssi,
                              (uint32_t)learned, 0, epoch};
            offset += (size_t)consumed;
        }
    }

    void saveClaims() {
        string encoded;
        char record[80];
        for (const BeaconClaim &claim : claims) {
            if (claim.originMac == 0) continue;
            snprintf(record, sizeof(record), "%llx,%llx,%x,%d,%x,%x;",
                     (unsigned long long)claim.originMac,
                     (unsigned long long)claim.bssid,
                     claim.originGeneration, (int)claim.rssi,
                     claim.learnedWakeGeneration, claim.originEpoch);
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
            unsigned epoch = 0;
            int consumed = 0;
            const int fields = sscanf(encoded.c_str() + offset,
                "%llx,%llx,%x,%x,%x;%n", &origin, &selected, &generation,
                &age, &epoch, &consumed);
            if (fields != 5 || consumed <= 0) break;
            // One persisted record spans one completed sleep/wake boundary.
            const uint64_t aged = (uint64_t)age;
            if (epochMatches(origin, epoch)) associations[slot++] = {
                (uint64_t)origin, (uint64_t)selected,
                (uint32_t)generation,
                aged > UINT32_MAX ? UINT32_MAX : (uint32_t)aged,
                startUsec, epoch};
            offset += (size_t)consumed;
        }
    }

    void saveAssociations() {
        string encoded;
        char record[64];
        for (const BeaconAssociation &association : associations) {
            if (association.originMac == 0) continue;
            snprintf(record, sizeof(record), "%llx,%llx,%x,%x,%x;",
                     (unsigned long long)association.originMac,
                     (unsigned long long)association.selectedBeacon,
                     association.originGeneration,
                     associationAgeCycles(association), association.originEpoch);
            encoded += record;
        }
        spiffsAssociations = encoded;
    }

    bool mergeAssociation(uint64_t originMac, uint64_t selectedBeacon,
                          uint32_t originGeneration, uint32_t ageCycles,
                          bool direct = false, uint32_t originEpoch = 0) {
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
                               storedAge, micros(), originEpoch};
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
                               storedAge, micros(), originEpoch};
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

    void clearMigrationProposal() {
        spiffsProposalBeacon = (uint64_t)0;
        spiffsProposalHome = (uint64_t)0;
        spiffsProposalActRound = (uint32_t)0;
        spiffsProposalMembers = (uint32_t)0;
    }

    bool commitHome(uint64_t candidateBssid) {
        if (!candidateBssid || candidateBssid == spiffsBeacon.read()) return false;
        spiffsBeacon = candidateBssid;
        clearMigrationProposal();
        spiffsCredibilityHome = candidateBssid;
        spiffsHomeCredibility = (uint32_t)0;
        spiffsScoutPhase = 0;
        spiffsScoutBeacon = 0;
        spiffsCurrentGoal = defaultRendezvousUsec;
        spiffsCurrentRep = 0;
        spiffsScale = 1.004;
        return true;
    }

    void updateHomeCredibility(uint64_t homeBssid, bool healthy, bool full) {
        uint32_t credibility = spiffsCredibilityHome.read() == homeBssid ?
            spiffsHomeCredibility.read() : 0;
        if (healthy && full)
            credibility = SingletonJoinPolicy::reinforce(credibility,
                reportSenderCount > 0);
        else
            credibility = SingletonJoinPolicy::decay(credibility);
        spiffsCredibilityHome = homeBssid;
        spiffsHomeCredibility = credibility;
        out("home-credibility home %012llx score %u healthy %u full %u direct-peers %u",
            (unsigned long long)homeBssid, credibility, healthy ? 1U : 0U,
            full ? 1U : 0U, (unsigned)reportSenderCount);
    }

    void observeDirectScoutForMigration(uint64_t homeBssid, uint64_t targetBssid) {
        const size_t homeMembers = listenerCount(homeBssid);
        const size_t targetMembers = listenerCount(targetBssid);
        if (SingletonJoinPolicy::mayAdopt(homeMembers, targetMembers)) {
            out("singleton-join direct target %012llx members %u from %012llx",
                (unsigned long long)targetBssid, (unsigned)targetMembers,
                (unsigned long long)homeBssid);
            commitHome(targetBssid);
            return;
        }
        if (!SingletonJoinPolicy::mayPropose(homeMembers, targetMembers)) {
            if (spiffsProposalBeacon.read() == targetBssid) {
                out("migration-proposal canceled target %012llx target-members %u home-members %u reason not-larger",
                    (unsigned long long)targetBssid, (unsigned)targetMembers,
                    (unsigned)homeMembers);
                clearMigrationProposal();
            } else {
                out("migration-rejected target %012llx target-members %u home-members %u reason not-larger",
                    (unsigned long long)targetBssid, (unsigned)targetMembers,
                    (unsigned)homeMembers);
            }
            return;
        }
        const uint32_t credibility = spiffsCredibilityHome.read() == homeBssid ?
            spiffsHomeCredibility.read() : 0;
        const uint64_t pending = spiffsProposalBeacon.read();
        if (pending == targetBssid && spiffsProposalHome.read() == homeBssid) {
            spiffsProposalMembers = (uint32_t)targetMembers;
            out("migration-proposal refreshed target %012llx target-members %u home-members %u act-round %u",
                (unsigned long long)targetBssid, (unsigned)targetMembers,
                (unsigned)homeMembers, spiffsProposalActRound.read());
            return;
        }
        if (pending && targetMembers <= spiffsProposalMembers.read()) {
            out("migration-rejected target %012llx target-members %u pending %012llx pending-members %u reason weaker-than-pending",
                (unsigned long long)targetBssid, (unsigned)targetMembers,
                (unsigned long long)pending, spiffsProposalMembers.read());
            return;
        }
        const uint32_t delay = SingletonJoinPolicy::proposalDelay(credibility);
        spiffsProposalBeacon = targetBssid;
        spiffsProposalHome = homeBssid;
        spiffsProposalActRound = wakeGeneration + delay;
        spiffsProposalMembers = (uint32_t)targetMembers;
        out("migration-proposal direct target %012llx target-members %u home %012llx home-members %u credibility %u act-round %u",
            (unsigned long long)targetBssid, (unsigned)targetMembers,
            (unsigned long long)homeBssid, (unsigned)homeMembers,
            credibility, wakeGeneration + delay);
    }

    bool maybeCommitMigration(uint64_t homeBssid) {
        const uint64_t target = spiffsProposalBeacon.read();
        if (!target) return false;
        if (spiffsProposalHome.read() != homeBssid) {
            out("migration-proposal canceled target %012llx reason home-changed",
                (unsigned long long)target);
            clearMigrationProposal();
            return false;
        }
        const size_t homeMembers = listenerCount(homeBssid);
        if (homeMembers >= spiffsProposalMembers.read()) {
            out("migration-proposal canceled target %012llx target-snapshot %u home-members %u reason home-caught-up",
                (unsigned long long)target, spiffsProposalMembers.read(),
                (unsigned)homeMembers);
            clearMigrationProposal();
            return false;
        }
        if (int32_t(wakeGeneration-spiffsProposalActRound.read()) < 0) return false;
        out("migration-proposal committed target %012llx target-snapshot %u home-members %u credibility %u",
            (unsigned long long)target, spiffsProposalMembers.read(),
            (unsigned)homeMembers, spiffsHomeCredibility.read());
        return commitHome(target);
    }

    bool adoptDirectlyObservedGroup(uint64_t homeBssid, uint64_t targetBssid) {
        observeDirectScoutForMigration(homeBssid, targetBssid);
        return spiffsBeacon.read() == targetBssid;
    }

    static void broadScanCallback(const WifiBeaconPacket &packet, void *arg) {
        static_cast<BeaconRendezvousContext *>(arg)->onBroadScan(packet);
    }

    void mergeClaim(uint64_t originMac, uint64_t bssid,
                    uint32_t originGeneration, int8_t rssi, uint32_t originEpoch) {
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
                claim.originEpoch = originEpoch;
                return;
            }
            if (empty == claimTableSize && claim.originMac == 0) empty = i;
        }
        if (empty == claimTableSize) return;
        claims[empty] = {originMac, bssid, originGeneration, rssi,
                         wakeGeneration, wakeGeneration, originEpoch};
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
                mergeClaim(deviceMac, bssid, wakeGeneration, packet.rssi, incarnation);
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
        int peerSlot = -1;
        uint64_t radioFrom = 0;
        if (from != nullptr)
            for (int i = 0; i < 6; ++i) radioFrom = (radioFrom << 8) | from[i];
        if (sender == 0) sender = protocolRadioMac(radioFrom);
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
                if (radioFrom != 0 && radioFrom != protocolRadioMac(reportSenders[peerSlot]))
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
        if (header.version != 7) {
            if (peerSlot >= 0) reportSenderBadVersion[peerSlot]++;
            return;
        }
        // Validate the complete framing before changing any association.
        if (!validReportLength(header, (size_t)length)) {
            reportBadLength++;
            return;
        }
        if (header.senderMac == deviceMac ||
            !acceptOrigin(header.senderMac, header.incarnation, true)) return;
        // One sample per peer per exchange keeps serial output bounded.
        if (peerSlot >= 0 && reportSenderValid[peerSlot] == 0)
        out("report-clock-rx sender %012llx incarnation %08x wake %u packet %u local-rx %llu bssid %012llx clock-ms-low %u start-delta-ms %d planned-end-delta-ms %d valid %u exchange %u",
            (unsigned long long)header.senderMac, header.incarnation,
            header.wakeGeneration, header.packetSequence,
            (unsigned long long)reportLocalRx,
            (unsigned long long)reportClockBssid(header), header.clockMsLow,
            (int)header.exchangeStartDeltaMs, (int)header.plannedEndDeltaMs,
            header.timingValid == 1 ? 1U : 0U, header.exchangeSequence);
        const bool refreshed = mergeAssociation(header.senderMac, header.selectedBeacon,
                         header.wakeGeneration, 0, true, header.incarnation);
        if (peerSlot >= 0 && refreshed) reportSenderAssociationRefresh[peerSlot]++;
        const size_t claimBytesAvailable = length - sizeof(header);
        const size_t available = claimBytesAvailable /
            sizeof(BeaconClaimEntry);
        const size_t count = min((size_t)header.claimCount,
                                 min(available, reportMaxClaims));
        reportRxClaimCount += count;
        if (peerSlot >= 0) reportSenderClaimEntries[peerSlot] += count;
        sender = header.senderMac;
        if (sender == 0) sender = protocolRadioMac(radioFrom);
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
            // Header already established the sender incarnation. Entries,
            // including the sender's own, may not contradict it.
            if (!acceptOrigin(entry.originMac, entry.incarnation, false)) continue;
            mergeClaim(entry.originMac, entry.bssid,
                       entry.originGeneration, entry.rssi, entry.incarnation);
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
            if (!acceptOrigin(entry.originMac, entry.incarnation, false)) continue;
            mergeAssociation(entry.originMac, entry.selectedBeacon,
                             entry.originGeneration, entry.ageCycles, false, entry.incarnation);
        }
    }

    void dumpExchangePeers() const {
        out("origin-incarnation rejected %u", epochRejected);
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
            out("espnow summary origin %012llx radio-from %012llx frames %u valid %u short %u bad-version %u association-refresh %u claim-entries %u radio-mismatch %u first %llu last %llu origin-radio %012llx",
                (unsigned long long)reportSenders[i],
                (unsigned long long)reportSenderRadioFrom[i],
                reportSenderRaw[i], reportSenderValid[i],
                reportSenderShort[i], reportSenderBadVersion[i],
                reportSenderAssociationRefresh[i], reportSenderClaimEntries[i],
                reportSenderRadioMismatch[i],
                (unsigned long long)reportSenderFirstUsec[i],
                (unsigned long long)reportSenderLastUsec[i],
                (unsigned long long)protocolRadioMac(reportSenders[i]));
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
                         0, true, incarnation);
        BeaconReportHeader header = {};
        header.version = 7;
        header.senderMac = deviceMac;
        header.selectedBeacon = spiffsBeacon.read();
        header.wakeGeneration = wakeGeneration;
        header.packetSequence = reportSequence++;
        header.incarnation = incarnation;
        header.exchangeSequence = exchangeSequence;
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
                                      claim.originGeneration, claim.rssi, claim.originEpoch};
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
                (uint16_t)min(age, (uint32_t)UINT16_MAX), association.originEpoch};
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

    void advanceRoundClock(uint64_t now) {
        const uint64_t elapsed = roundClock.tick(now, defaultRendezvousUsec);
        if (!elapsed) return;
        // Finalize exactly once per elapsed logical round, even if continuous
        // awake execution or several sleeps occurred within that round.
        const bool qualified = spiffsRoundHealthy.read() &&
            spiffsRoundHome.read() == spiffsBeacon.read();
        spiffsRoundHealthy = 0;
        for (uint64_t i = 0; i < elapsed; ++i) {
            maybeResetAfterStableReunion(spiffsBeacon.read(), i == 0 && qualified);
            if (!spiffsIncarnation.read()) return;
        }
        for (BeaconAssociation &a : associations)
            if (a.originMac) a.ageCycles = RendezvousExecutor::age(a.ageCycles, elapsed);
        // Incarnation rollover remains vanishingly rare; clear this origin's
        // evidence through the normal direct-origin transition on wrap.
        if (elapsed > UINT32_MAX - wakeGeneration) {
#ifdef CSIM
            incarnation = (uint32_t)rand();
#else
            incarnation = esp_random();
#endif
            if (!incarnation) incarnation = 1;
            spiffsIncarnation = incarnation;
            wakeGeneration = 1;
            acceptOrigin(deviceMac, incarnation, true);
        } else wakeGeneration += (uint32_t)elapsed;
        spiffsClaimGeneration = wakeGeneration;
        out("logical-round generation %u elapsed %llu", wakeGeneration,
            (unsigned long long)elapsed);
    }

    void resetIntervalStats() {
        reportSequence = 0;
        reportTxCount = reportRxCount = reportRxClaimCount = reportValidRxCount = 0;
        reportBadLength = epochRejected = 0;
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
        memset(reportSenderAssociationRefresh, 0, sizeof(reportSenderAssociationRefresh));
        memset(reportSenderClaimEntries, 0, sizeof(reportSenderClaimEntries));
        memset(reportSenderRadioMismatch, 0, sizeof(reportSenderRadioMismatch));
        reportSenderCount = 0;
        intervalRawStart = privMux.getReceiveCallbacks();
        intervalOkStart = privMux.getSendSuccesses();
        intervalFailStart = privMux.getSendFailures();
    }

    const BeaconInfo *freshTiming(uint64_t bssid, uint64_t now) const {
        for (const BeaconInfo &info : packetLog)
            if (info.ssid == bssid && info.count && now >= info.seen2 &&
                now-info.seen2 <= beaconSamplingWindowUsec) return &info;
        return nullptr;
    }

    bool makeExecutionPlan(uint64_t now) {
        uint64_t home = spiffsBeacon.read();
        if (!home) {
            const int best = randomStartupBeaconIndex();
            if (!packetLog[best].count || !packetLog[best].ssid) return false;
            home = packetLog[best].ssid;
            spiffsBeacon = home;
        }
        const BeaconInfo *timing = freshTiming(home, now);
        if (!timing) return false; // stay awake acquiring home, never substitute a scout
        executionPlan = RendezvousPlanner::Plan<4>(1000000);
        const uint64_t period = defaultRendezvousUsec;
        RendezvousPlanner::Appointment first, second, scout;
        if (!RendezvousPlanner::nextAppointment(home, timing->ts, timing->seen2,
            now, period, beaconSamplingWindowUsec, exchangeWindowUsec, true, first) ||
            !RendezvousPlanner::nextAppointment(home, timing->ts, timing->seen2,
            first.end, period, beaconSamplingWindowUsec, exchangeWindowUsec, true, second) ||
            !executionPlan.addHome(first) || !executionPlan.addHome(second)) return false;
        if (wakeGeneration - spiffsLastScoutRound.read() >= scoutIntervalWakes) {
            uint64_t candidates[packetLogSize];
            size_t count = 0;
            for (const BeaconInfo &info : packetLog)
                if (info.ssid && info.rssi >= reportMinRssi &&
                    info.count >= minimumCandidatePackets && freshTiming(info.ssid, now))
                    candidates[count++] = info.ssid;
            uint64_t selected = spiffsScoutBeacon.read();
            bool eligible = false;
            for (size_t i = 0; i < count; ++i)
                if (candidates[i] == selected && selected != home) eligible = true;
            if (!eligible) selected = RendezvousPlanner::chooseScout(candidates, count,
                home, spiffsScoutCursor.read());
            const BeaconInfo *other = freshTiming(selected, now);
            if (other && RendezvousPlanner::nextAppointment(selected, other->ts,
                other->seen2, now, period, beaconSamplingWindowUsec,
                exchangeWindowUsec, false, scout)) {
                if (executionPlan.addScout(scout, exchangeWindowUsec + 2000000))
                    spiffsScoutBeacon = selected;
                else {
                    spiffsScoutCursor = selected;
                    spiffsScoutBeacon = (uint64_t)0;
                    spiffsLastScoutRound = wakeGeneration;
                    out("scout deferred budget target %012llx", (unsigned long long)selected);
                }
            }
        }
        for (auto &state : coverage) state = {};
        executionInterval = 0;
        plannedHome = home;
        plannedPeriod = period;
        executionPlanned = true;
        out("interval-plan home %012llx appointments %u intervals %u awake-usec %llu",
            (unsigned long long)home, (unsigned)executionPlan.appointmentCount(),
            (unsigned)executionPlan.intervalCount(), (unsigned long long)executionPlan.awakeUsec());
        return true;
    }

    void sleepForExecutor(uint64_t duration) {
        advanceRoundClock(micros());
        if (!spiffsIncarnation.read()) return;
        saveClaims(); saveAssociations(); saveOrigins();
        out("deep sleep %.3f sec executor", duration / 1000000.0);
        fflush(stdout);
#ifndef CSIM
        uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
#endif
        // Account for save/log time before deciding whether sleep is still safe.
        const uint64_t now = micros();
        duration = executionPlan.sleepUntilNext(now, beaconSamplingWindowUsec + 500000);
        if (duration < 1000000) return;
        spiffsRoundElapsed = roundClock.remainder + (now-roundClock.last) + duration;
        spiffsSleepTime = duration;
        spiffsClaimGeneration = wakeGeneration;
        beaconCapture.stop();
        esp_sleep_enable_timer_wakeup(duration);
        esp_deep_sleep_start();
    }

    void intervalExecutorLoop(uint64_t now) {
        advanceRoundClock(now);
        if (!spiffsIncarnation.read()) {
            saveClaims(); saveAssociations(); saveOrigins();
            beaconCapture.stop();
            esp_sleep_enable_timer_wakeup(1000);
            esp_deep_sleep_start();
            return;
        }
        if (now-startUsec < beaconSamplingWindowUsec) { delay(1); return; }
        if (!espNowStarted) {
            // Preserve beacon-only acquisition before the first ESP-NOW init:
            // Jim observed early init suppressing promiscuous beacon callbacks.
            // Keep the initialized radio across merged/nearby appointments.
            privMux.registerReadCallback("BRPT", [this](const uint8_t *from,
                const uint8_t *data, int length) { onReport(from, data, length); });
            espNowStarted = true;
            return;
        }
        if (!executionPlanned) {
            if (now < nextPlanUsec) { delay(1); return; }
            if (!makeExecutionPlan(now)) {
                nextPlanUsec = now + 1000000;
                out("interval-plan waiting for fresh home timing");
                delay(1); return;
            }
        }
        if (executionInterval >= executionPlan.intervalCount()) {
            executionPlanned = false;
            return;
        }
        const auto &interval = executionPlan.interval(executionInterval);
        if (!exchangeActive && now < interval.start) {
            const uint64_t sleep = executionPlan.sleepUntilNext(now, beaconSamplingWindowUsec+500000);
            if (sleep >= 1000000) sleepForExecutor(sleep);
            delay(1); return;
        }
        if (!exchangeActive) {
            resetIntervalStats();
            exchangeSequence = spiffsExchangeSequence.read() + 1;
            spiffsExchangeSequence = exchangeSequence;
            targetBeacon = plannedHome;
            bool homePresent = false;
            for (size_t i = 0; i < executionPlan.appointmentCount(); ++i) {
                if (!(interval.appointments & (1ULL << i))) continue;
                const auto &a = executionPlan.appointment(i);
                if (a.home) homePresent = true;
                else targetBeacon = a.bssid;
            }
            if (homePresent) targetBeacon = plannedHome;
            espNowStartUsec = now;
            espNowEndUsec = interval.end;
            nextReportUsec = now;
            exchangeActive = true;
            out("report-identity incarnation %08x wake %u wire-version 7 exchange %u",
                incarnation, wakeGeneration, exchangeSequence);
            out("ESP-NOW exchange phase started interval %u planned %llu-%llu",
                exchangeSequence, (unsigned long long)interval.start, (unsigned long long)interval.end);
        }
        for (size_t i = 0; i < executionPlan.appointmentCount(); ++i) {
            if (!(interval.appointments & (1ULL << i))) continue;
            const auto &a = executionPlan.appointment(i);
            auto &state = coverage[i];
            if (!state.started && now >= a.start) {
                state.begin(now, a.start, a.late, privMux.getSendSuccesses(), reportValidRxCount, 20000);
                coverageFailures[i] = privMux.getSendFailures();
            }
            if (state.started && !state.finished && now >= a.end) {
                state.finished = true;
                const bool healthy = state.healthy(privMux.getSendSuccesses(), reportValidRxCount) &&
                    privMux.getSendFailures()-coverageFailures[i] <= 5;
                out("appointment complete exchange %u kind %s target %012llx full %u healthy %u",
                    exchangeSequence, a.home ? "home" : "scout",
                    (unsigned long long)a.bssid, state.full ? 1U : 0U, healthy ? 1U : 0U);
                if (a.home && spiffsBeacon.read() == a.bssid) {
                    updateHomeCredibility(a.bssid, healthy, state.full);
                    if (healthy && state.full)
                        maybeCommitMigration(a.bssid);
                    if (healthy && state.full) {
                        spiffsRoundHealthy = 1;
                        spiffsRoundHome = a.bssid;
                    }
                } else if (!a.home) {
                    if (healthy && state.full)
                        adoptDirectlyObservedGroup(plannedHome, a.bssid);
                    spiffsScoutCursor = a.bssid;
                    spiffsScoutBeacon = (uint64_t)0;
                    spiffsLastScoutRound = wakeGeneration;
                }
            }
        }
        if (now < interval.end) {
            if (now >= nextReportUsec) {
                publishReport();
                nextReportUsec = now + reportPeriodUsec;
            }
            delay(1); return;
        }
        const BeaconInfo *timing = freshTiming(targetBeacon, now);
        uint64_t projectedStart = 0, projectedEnd = 0;
        if (timing && projectBeaconTsf(timing->ts, timing->seen2, espNowStartUsec, projectedStart) &&
            projectBeaconTsf(timing->ts, timing->seen2, espNowEndUsec, projectedEnd))
            out("beacon-clock target %012llx tsf-packet %llu exchange %llu-%llu",
                (unsigned long long)targetBeacon, (unsigned long long)timing->ts,
                (unsigned long long)projectedStart, (unsigned long long)projectedEnd);
        const uint64_t home = spiffsBeacon.read();
        const bool transportHealthy = reportValidRxCount >= 3 &&
            privMux.getSendSuccesses()-intervalOkStart >= 20;
        out("gossip interval exchange %s home %012llx listeners %u rawrx %u rx %u valid %u peers %u",
            transportHealthy ? "healthy" : "incomplete", (unsigned long long)home,
            (unsigned)listenerCount(home), privMux.getReceiveCallbacks()-intervalRawStart,
            reportRxCount, reportValidRxCount, reportSenderCount);
        dumpExchangePeers();
        dumpAssociationTable(home);
        out("exchange complete interval %u", exchangeSequence);
        exchangeActive = false;
        ++executionInterval;
        if (home != plannedHome || !spiffsIncarnation.read()) executionPlanned = false;
        if (!spiffsIncarnation.read()) {
            // Test reset changes incarnation in the next setup; restart through
            // deep sleep without pretending a normal appointment caused it.
            saveClaims(); saveAssociations(); saveOrigins();
            beaconCapture.stop();
            esp_sleep_enable_timer_wakeup(1000);
            esp_deep_sleep_start();
        }
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
        epochRejected = 0;
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
        if (spiffsTestSwarmCount.read() != testClusterSize) {
            // Do not inherit a streak/committed reset from a different oracle.
            spiffsTestConsensusCycles = 0;
            spiffsTestConsensusMisses = 0;
            spiffsTestResetCommitted = 0;
            spiffsTestResetDelayCycles = 0;
            spiffsRoundHealthy = 0;
            spiffsTestSwarmCount = testClusterSize;
        }
        out("artificial-test-swarm boards %u (includes unlogged boards; not production knowledge)",
            (unsigned)testClusterSize);
        loadOrigins();
        loadClaims();
        loadAssociations();
        wakeGeneration = spiffsClaimGeneration.read();
        if (!wakeGeneration) wakeGeneration = 1;
        roundClock = {spiffsRoundElapsed.read(), micros()};
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
        acceptOrigin(deviceMac, incarnation, true);
        out("report-identity incarnation %08x wake %u wire-version 7 max-packet-bytes 180",
            incarnation, wakeGeneration);
        const uint64_t homeBeacon = spiffsBeacon.read();
        targetBeacon = homeBeacon;
        scoutWake = false;
        scoutRendezvousWake = false;
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
        executionPlanned = exchangeActive = false;
        nextPlanUsec = 0;
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
        intervalExecutorLoop(nowUsec);
        return;
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
