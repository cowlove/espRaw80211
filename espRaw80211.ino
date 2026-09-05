#include "jimlib.h"
#include "espNowMux.h"
#include "raw80211Capture.h"
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
};

// Application-level rendezvous advertisement.  Keep this deliberately small:
// ESPNowMux reserves four bytes for the routing prefix and uses a conservative
// 200-byte physical packet size.
struct __attribute__((packed)) BeaconReportHeader {
    uint8_t version;
    uint64_t senderMac;
    uint64_t selectedBeacon;
    uint8_t beaconCount;
};

struct __attribute__((packed)) BeaconReportEntry {
    uint64_t bssid;
    int8_t rssi;
    uint16_t observations;
    uint8_t selectingClients;
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
        SimBeacon beacons[3];
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
            environments[i].beacons[1] =
                {bssidBase + 3, (int8_t)(-60 - (i % 4)), 4, 204800,
                 tsfOrigin + 6000000, tsfRatePpm, 0};
            // One weak infrastructure beacon is common to every simulated
            // RF environment.  Keep it weaker and less frequent than the
            // per-context beacon so bootstrap selection remains local while
            // later tests can reason about common competing infrastructure.
            environments[i].beacons[2] =
                {0x000096ce0fEEULL, -82, 4, 204800,
                 5000000, 1000000, 0};
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
    static constexpr size_t packetLogSize = 64;
    static constexpr size_t remoteStatsSize = 64;
    // Header + 14 entries, including the four-byte BRPT prefix, stays within
    // ESPNowMux's conservative 200-byte physical packet limit.
    static constexpr size_t reportMaxBeacons = 14;
    static constexpr int reportMinRssi = -85;
    static constexpr uint64_t reportPeriodUsec = 200000;
    static constexpr uint64_t defaultRendezvousUsec = 60ULL * 1000000ULL;

    BeaconInfo packetLog[packetLogSize] = {};
    SPIFFSVariable<uint64_t> spiffsBeacon{"/beaconX", 0};
    SPIFFSVariable<uint64_t> spiffsSleepTime{"/sleepTimeX", 0};
    SPIFFSVariable<float> spiffsScale{"/scaleX", 1.0};
    SPIFFSVariable<uint64_t> spiffsCurrentGoal{"/currentGoal", 0};
    SPIFFSVariable<int> spiffsCurrentRep{"/currentRep", 0};
    uint64_t targetBeacon = 0;
    int wifiChannel = 4;
    uint64_t startUsec = 0;
    uint64_t nextReportUsec = 0;
    uint64_t deviceMac = 0;
    int loopCount = 0;
    RemoteBeaconStats remoteStats[remoteStatsSize] = {};

    static int score(const BeaconInfo &info) { return info.count; }

    static void oneShotCallback(const WifiBeaconPacket &packet, void *arg) {
        static_cast<BeaconRendezvousContext *>(arg)->onOneShot(packet);
    }

    static void collectCallback(const WifiBeaconPacket &packet, void *arg) {
        static_cast<BeaconRendezvousContext *>(arg)->onCollect(packet);
    }

    void onOneShot(const WifiBeaconPacket &packet) {
        if (packet.length < 32 || beaconBssid(packet.data) != targetBeacon) return;
        BeaconInfo &info = packetLog[0];
        info.ssid = targetBeacon;
        info.seen2 = packet.localTimestampUsec;
        info.seen = packet.driverTimestampUsec;
        info.rssi = packet.rssi;
        info.count++;
        info.ts = beaconTsf(packet.data);
        beaconCapture.setCallback(nullptr);
    }

    void onCollect(const WifiBeaconPacket &packet) {
        if (packet.length < 32) return;
        const uint64_t bssid = beaconBssid(packet.data);
        size_t i;
        for (i = 0; i < packetLogSize; ++i) {
            if (packetLog[i].ssid == bssid) {
                BeaconInfo &info = packetLog[i];
                info.seen2 = packet.localTimestampUsec;
                info.seen = packet.driverTimestampUsec;
                info.rssi = (packet.rssi + info.count * info.rssi) / (info.count + 1);
                info.count++;
                info.ts = beaconTsf(packet.data);
                return;
            }
        }
        size_t worst = 0;
        for (i = 0; i < packetLogSize; ++i) {
            if (packetLog[i].ssid == 0) { worst = i; break; }
            if (score(packetLog[i]) <= score(packetLog[worst])) worst = i;
        }
        packetLog[worst].ssid = bssid;
        onCollect(packet);
    }

    void onReport(const uint8_t *from, const uint8_t *data, int length) {
        if (length < (int)sizeof(BeaconReportHeader)) return;
        BeaconReportHeader header;
        memcpy(&header, data, sizeof(header));
        if (header.version != 2) return;
        const size_t available = (length - sizeof(header)) /
            sizeof(BeaconReportEntry);
        const size_t count = min((size_t)header.beaconCount,
                                 min(available, reportMaxBeacons));
        uint64_t sender = header.senderMac;
        if (sender == 0 && from != nullptr)
            for (int i = 0; i < 6; ++i) sender = (sender << 8) | from[i];
        if (sender == deviceMac) return;
        for (size_t i = 0; i < count; ++i) {
            BeaconReportEntry entry;
            memcpy(&entry, data + sizeof(header) +
                   i * sizeof(entry), sizeof(entry));
            size_t slot = 0;
            for (; slot < remoteStatsSize; ++slot) {
                if (remoteStats[slot].bssid == entry.bssid) break;
                if (remoteStats[slot].bssid == 0) break;
            }
            if (slot == remoteStatsSize) continue;
            RemoteBeaconStats &stats = remoteStats[slot];
            stats.bssid = entry.bssid;
            stats.reports++;
            stats.observations += entry.observations;
            stats.lastRssi = entry.rssi;
            stats.strongestRssi = max(stats.strongestRssi, entry.rssi);
            stats.lastSender = sender;
            if (entry.bssid == header.selectedBeacon) {
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
    }

    void publishReport() {
        uint8_t buffer[sizeof(BeaconReportHeader) +
                       reportMaxBeacons * sizeof(BeaconReportEntry)] = {};
        BeaconReportHeader header = {2, deviceMac, spiffsBeacon.read(), 0};
        memcpy(buffer, &header, sizeof(header));
        size_t count = 0;
        // packetLog is maintained in observation-count order only loosely; the
        // cap is intentional and keeps one report within ESP-NOW's physical
        // packet limit.
        for (size_t i = 0; i < packetLogSize && count < reportMaxBeacons; ++i) {
            const BeaconInfo &info = packetLog[i];
            if (info.ssid == 0 || info.rssi < reportMinRssi) continue;
            BeaconReportEntry entry = {
                info.ssid, (int8_t)info.rssi,
                (uint16_t)min<uint64_t>(info.count, 0xffff), 0};
            for (size_t j = 0; j < remoteStatsSize; ++j)
                if (remoteStats[j].bssid == info.ssid) {
                    entry.selectingClients = remoteStats[j].selectingClientCount;
                    break;
                }
            memcpy(buffer + sizeof(header) + count * sizeof(entry),
                   &entry, sizeof(entry));
            count++;
        }
        header.beaconCount = (uint8_t)count;
        memcpy(buffer, &header, sizeof(header));
        privMux.send("BRPT", buffer, sizeof(header) +
                     count * sizeof(BeaconReportEntry));
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
        targetBeacon = spiffsBeacon;
        beaconCapture.setCallback(oneShotCallback, this);
        beaconCapture.start();
    }

    void startCollection() {
        targetBeacon = spiffsBeacon;
        beaconCapture.setCallback(collectCallback, this);
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
#ifdef CSIM
        const double setupSeconds =
            (sim().bootTimeUsec + micros()) / 1000000.0;
#else
        const double setupSeconds = millis() / 1000.0;
#endif
        printf("%09.3f setup() waiting for %llx\n", setupSeconds,
               (unsigned long long)spiffsBeacon.read());
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
        privMux.registerReadCallback("BRPT", [this](const uint8_t *from,
                                                     const uint8_t *data,
                                                     int length) {
            onReport(from, data, length);
        });
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
        if (nowUsec >= nextReportUsec) {
            publishReport();
            nextReportUsec = nowUsec + reportPeriodUsec;
        }
        if (packetLog[0].count == 0 && micros() - startUsec < 10000000) {
            delay(1);
            return;
        }

        BeaconInfo result = packetLog[0];
        BeaconInfo *beacon = &result;
        if (result.count == 0) {
            out("No beacon packet received, picking new beacon");
            beaconCapture.stop();
            startCollection();
            delay(250);
            beaconCapture.stop();
            const int best = bestBeaconIndex();
            out("best beacon: %02d %012llx %3d %6d %016llx %016llx", best,
                (unsigned long long)packetLog[best].ssid, packetLog[best].rssi,
                packetLog[best].count, (unsigned long long)packetLog[best].seen,
                (unsigned long long)packetLog[best].seen2);
            beacon = &packetLog[best];
            spiffsBeacon = beacon->ssid;
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
        beacon->seen2 -= startUsec;
        const uint64_t packetRxTime = beacon->seen2;
        int beaconDistance = (int)(beacon->ts % goal);
        if (beaconDistance > (int)(goal / 2)) beaconDistance -= goal;
        int espDistance = (int)(packetRxTime % goal);
        if (espDistance > (int)(goal / 2)) espDistance -= goal;
        const int usecLate = beaconDistance - espDistance;
        const uint64_t previousSleep = spiffsSleepTime.read();
        const float percentLate = previousSleep ? abs(100.0 * usecLate / previousSleep) : 0.0;

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
            if (spiffsCurrentRep > 50 && beacon->ts % goal == beacon->ts % (goal * 2)) {
                spiffsCurrentGoal = spiffsCurrentGoal * 5;
                spiffsCurrentRep = 0;
            }
        } else {
            spiffsScale = 1.004;
        }

        goal = spiffsCurrentGoal;
        uint64_t timeToGoal = goal - (beacon->ts % goal);
        if (timeToGoal % goal < goal / 2) timeToGoal += goal;
        uint64_t sleepUsec =
            (timeToGoal - (micros() - startUsec - packetRxTime)) * spiffsScale;
        out("deep sleep %.1f sec, goal %.1f scale %f", sleepUsec / 1000000.0,
            goal / 1000000.0, spiffsScale.read());
        fflush(stdout);
#ifndef CSIM
        uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
#endif
        sleepUsec =
            (timeToGoal - (micros() - startUsec - packetRxTime)) * spiffsScale;
        spiffsSleepTime = sleepUsec;
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
