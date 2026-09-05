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

#ifdef CSIM
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
    } environments[4] = {
        {{{0x000096ce8362ULL, -38, 4,  51200, 1000000, 1000000, 0},
          {0x000096ce8363ULL, -61, 4, 204800, 2000000, 1000000, 0},
          {0x000096ce8364ULL, -72, 4, 204800, 3000000, 1000000, 0}}},
        {{{0x000096ce8372ULL, -40, 4,  51200, 17000000, 1000150, 0},
          {0x000096ce8373ULL, -59, 4, 204800, 23000000, 1000150, 0},
          {0x000096ce8374ULL, -70, 4, 204800, 29000000, 1000150, 0}}},
        {{{0x000096ce8382ULL, -42, 4,  51200, 33000000, 999850, 0},
          {0x000096ce8383ULL, -57, 4, 204800, 39000000, 999850, 0},
          {0x000096ce8384ULL, -69, 4, 204800, 45000000, 999850, 0}}},
        {{{0x000096ce8392ULL, -39, 4,  51200, 61000000, 1000300, 0},
          {0x000096ce8393ULL, -56, 4, 204800, 67000000, 1000300, 0},
          {0x000096ce8394ULL, -68, 4, 204800, 73000000, 1000300, 0}}},
    };
    struct Destination {
        CsimWifiBeaconCaptureSource *capture;
        uint8_t channel;
        uint8_t environmentId;
    } destinations[8] = {};
    size_t destinationCount = 0;

    uint64_t absoluteUsec() const { return sim().bootTimeUsec + micros(); }

    void emit(const SimBeacon &beacon, uint64_t emissionUsec,
              const Destination &destination) {
        if (destination.environmentId >= 4 || destination.channel != beacon.channel)
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

public:
    void addDestination(CsimWifiBeaconCaptureSource *capture, uint8_t channel,
                        uint8_t environmentId) {
        CSIM_ASSERT(destinationCount < sizeof(destinations) / sizeof(destinations[0]));
        destinations[destinationCount++] = {capture, channel, environmentId};
    }

    void loop() override {
        const uint64_t now = absoluteUsec();
        for (uint8_t environmentId = 0; environmentId < 4; ++environmentId) {
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
    int loopCount = 0;

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
        configureBeaconRadio();
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
        const uint8_t environmentId = (uint8_t)((address - 1) & 3);
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
        loopCount = 0;
        startUsec = micros();
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
        if (packetLog[0].count == 0 && micros() - startUsec < 10000000) {
            delay(1);
            return;
        }

        BeaconInfo result = packetLog[0];
        BeaconInfo *beacon = &result;
        if (result.count == 0) {
            out("No beacon packet received, picking new beacon");
            beaconCapture.stop();
            configureBeaconRadio();
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

BeaconRendezvousContext rendezvous0(0xddeeff000001ULL);
#ifdef CSIM
BeaconRendezvousContext rendezvous1(0xddeeff000002ULL);
BeaconRendezvousContext rendezvous2(0xddeeff000003ULL);
BeaconRendezvousContext rendezvous3(0xddeeff000004ULL);
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
