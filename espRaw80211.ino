#include "jimlib.h"
#include "raw80211Capture.h"
#ifndef ESP32
#error Only the ESP32 is supported
#endif 
#ifndef CSIM
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <rom/uart.h>
#endif

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

class HardwareContext {
public:
#ifdef CSIM
    CsimWifiBeaconCaptureSource wifiBeaconCapture{&defaultContext};
#else
    Esp32WifiBeaconCaptureSource wifiBeaconCapture;
#endif
};

static HardwareContext hardware;

#ifdef CSIM
// Application-owned synthetic RF environment.  The capture HAL only accepts
// injected beacon packets; cadence and beacon identities remain in the sketch.
static void serviceSyntheticBeacons() {
    struct SimBeacon {
        uint64_t bssid;
        int8_t rssi;
        uint32_t intervalUsec;
        uint64_t nextUsec;
        uint64_t tsfOrigin;
    };
    static SimBeacon beacons[] = {
        {0x000096ce8362ULL, -43, 102400, 0, 1000000},
        {0x000096ce8363ULL, -58, 102400, 0, 2000000},
        {0x000096ce8364ULL, -67, 204800, 0, 3000000},
    };
    const uint64_t now = micros();
    for (SimBeacon &beacon : beacons) {
        if (beacon.nextUsec == 0) beacon.nextUsec = now;
        while (now >= beacon.nextUsec) {
            uint8_t frame[36] = {};
            frame[0] = 0x80; // management beacon
            for (int i = 0; i < 6; ++i)
                frame[10 + i] = beacon.bssid >> (40 - 8 * i);
            const uint64_t tsf = beacon.tsfOrigin + beacon.nextUsec;
            memcpy(frame + 24, &tsf, sizeof(tsf));
            WifiBeaconPacket packet;
            packet.driverTimestampUsec = beacon.nextUsec;
            packet.localTimestampUsec = beacon.nextUsec;
            packet.rssi = beacon.rssi;
            packet.channel = 4;
            packet.data = frame;
            packet.length = sizeof(frame);
            hardware.wifiBeaconCapture.inject(packet);
            beacon.nextUsec += beacon.intervalUsec;
        }
    }
}
#else
static void serviceSyntheticBeacons() {}
#endif

struct Info { 
    uint64_t ssid = 0;
    int rssi;
    uint64_t ts;
    int count = 0;
    uint64_t seen;
    uint64_t seen2;
};

Info pktLog[64];


SPIFFSVariable<uint64_t> spiffsBeacon("/beaconX", 0);//0x000096ce8362);
SPIFFSVariable<uint64_t> spiffsSleepTime("/sleepTimeX", 0);
SPIFFSVariable<float> spiffsScale("/scaleX", 1.0);
SPIFFSVariable<uint64_t> spiffsCurrentGoal("/currentGoal", 0);
SPIFFSVariable<int> spiffsCurrentRep("/currentRep", 0);
uint64_t intr_beacon; 
int wifi_channel = 4;
static constexpr uint64_t DEFAULT_RENDEZVOUS_US = 60ULL * 1000000ULL;


void intr_oneShot(const WifiBeaconPacket &packet, void *) {
    if (packet.length < 32 || (packet.data[0] & 0xfc) != 0x80) return;
    const uint64_t bssid = beaconBssid(packet.data);

    if (bssid == intr_beacon) {
        int i = 0;
        pktLog[i].ssid = bssid;
        pktLog[i].seen2 = packet.localTimestampUsec;
        pktLog[i].seen = packet.driverTimestampUsec;
        pktLog[i].rssi = packet.rssi;
        pktLog[i].count++;
        pktLog[i].ts = beaconTsf(packet.data);
        hardware.wifiBeaconCapture.setCallback(nullptr);
    }
}

//int score(const Info &i) { return (110 - i.rssi) * i.count; }
int score(const Info &i) { return i.count; }
void intr_collect(const WifiBeaconPacket &packet, void *) {
    if (packet.length < 32 || (packet.data[0] & 0xfc) != 0x80) return;
    const uint64_t bssid = beaconBssid(packet.data);
    {
        //printf("%07.3f MAC: %06llx ts: %016llx RSSI: % 4d\n", 
        //    millis() / 1000.0, pk->send_addr, pk->timestamp, pt->rx_ctrl.rssi);
        int i;
        for(i = 0; i < sizeof(pktLog)/sizeof(pktLog[0]); i++) {
            if(pktLog[i].ssid == bssid) {
                pktLog[i].seen2 = packet.localTimestampUsec;
                pktLog[i].seen = packet.driverTimestampUsec;
                pktLog[i].rssi = (packet.rssi + pktLog[i].count * pktLog[i].rssi) / (pktLog[i].count + 1);
                pktLog[i].count++;
                pktLog[i].ts = beaconTsf(packet.data);
                break;
            } 
        }
        if (i == sizeof(pktLog)/sizeof(pktLog[0])) {
            int worst = 0;
            for(i = 0; i < sizeof(pktLog)/sizeof(pktLog[0]); i++) {
                if (pktLog[i].ssid == 0) { 
                    worst = i;
                    break;
                }
                if (score(pktLog[i]) <= score(pktLog[worst]))
                    worst = i;
            }
            pktLog[worst].ssid = bssid;
            intr_collect(packet, nullptr);
            return;
        }
        int best = 0;
        for(i = 0; i < sizeof(pktLog)/sizeof(pktLog[0]); i++) {
            if (pktLog[i].ssid != 0 && score(pktLog[i]) >= score(pktLog[best]))
                best = i;
        }
        //printf("best: %02d %012llx %3d %6d %016llx %016llx\n", 
        //    best, pktLog[best].ssid, pktLog[best].rssi, pktLog[best].count);
    }
}  

void pretty_packet_handler(const WifiBeaconPacket &packet, void *);

static void configureBeaconRadio() {
#ifndef CSIM
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_wifi_init(&cfg);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_disconnect();
    esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
#endif
}

void setupPromisc() {
    intr_beacon = spiffsBeacon;
    configureBeaconRadio();
    hardware.wifiBeaconCapture.setCallback(intr_oneShot);
    hardware.wifiBeaconCapture.start();
}

//JStuff j;

void setupPromisc2() { 
    intr_beacon = spiffsBeacon;
    hardware.wifiBeaconCapture.setCallback(intr_collect);
    hardware.wifiBeaconCapture.start();
}


void println(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("%09.3f ", millis()/1000.0);
    vprintf(fmt, args);
    printf("\n");
}
#define OUT println
//JStuff j;
void setup() {
    //j.begin();
    //Serial.begin(921600);
    // SPIFFSVariable is backed by LittleFS in jimlib.  Mount it before any
    // persisted variable is read or written.
    SPIFFSVariableESP32Base::begin();
    printf("%09.3f setup() waiting for %llx\n", millis()/1000.0, spiffsBeacon.read());
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        esp_task_wdt_config_t c;
        c.timeout_ms = (25)*1000;
        c.idle_core_mask = 0x1;
        c.trigger_panic = true;
        esp_task_wdt_deinit();
        esp_task_wdt_init(&c);  // include jimlib.h last or this will cause compile errors in other headers
        esp_task_wdt_add(NULL);
#endif
    setupPromisc();
}


static uint64_t startUs = 0;
static int loopCount = 0;

static int resetReason() {
#ifdef CSIM
    return rtc_get_reset_reason(0);
#else
    return esp_rom_get_reset_reason(0);
#endif
}

void loop() {
    loopCount++;
#ifndef CSIM
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#endif
    esp_task_wdt_reset();
    serviceSyntheticBeacons();
    if (pktLog[0].count == 0 && millis() - startUs / 1000 < 10000) {
        delay(1);
        return;
    }
    Info resultPkt = pktLog[0];
    Info *b = &resultPkt;

    if (resultPkt.count == 0) {
        // Bootstrap-only discovery for standalone timing experiments. A real
        // rendezvous deployment would use a pre-assigned BSSID/channel and
        // must not switch to an unrelated AP after a missed beacon: peers
        // could then follow different TSF clocks. Without its assigned beacon
        // a coordinated device must retain calibration and retry later; this
        // prototype selects a visible beacon so data collection can continue.
        OUT("No beacon packet received, picking new beacon", millis()); 
        hardware.wifiBeaconCapture.stop();
        configureBeaconRadio();
        hardware.wifiBeaconCapture.setCallback(intr_collect);
        hardware.wifiBeaconCapture.start();
        delay(250);
        serviceSyntheticBeacons();
        hardware.wifiBeaconCapture.stop();

        int best = 0, i;
        for(i = 0; i < sizeof(pktLog)/sizeof(pktLog[0]); i++) {
            if (pktLog[i].ssid != 0 && score(pktLog[i]) >= score(pktLog[best]))
                best = i;
        }
        OUT("best beacon: %02d %012llx %3d %6d %016llx %016llx", 
            best, pktLog[best].ssid, pktLog[best].rssi, pktLog[best].count);
        
#ifndef CSIM
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_wifi_init(&cfg);
#endif
        b = &pktLog[best];
        spiffsBeacon = b->ssid;
    }

    if (resetReason() != 5) {
        spiffsCurrentGoal = DEFAULT_RENDEZVOUS_US;
        spiffsCurrentRep = 0;
    }
    uint64_t goal = spiffsCurrentGoal;
    // A freshly erased SPIFFS (or an older image) can leave this persisted
    // value at zero.  Do not use it as a modulo divisor.
    if (goal == 0) {
        goal = DEFAULT_RENDEZVOUS_US;
        spiffsCurrentGoal = goal;
        spiffsCurrentRep = 0;
    }
    b->seen -= 0; //startUs; // b->seen seems to be counting from esp_wifi_init calls, not from boot 
    b->seen2 -= startUs;
    uint64_t pktRxTime = b->seen2;

    int beaconDist = (int)(b->ts % goal);
    if (beaconDist > goal / 2) { 
        beaconDist -= goal;
    }
    int espDist = (int)(pktRxTime % goal);
    if (espDist > goal / 2) { 
        espDist -= goal;
    }

    int usecLate = beaconDist - espDist;
    uint64_t sleepTime = spiffsSleepTime.read();
    float percentLate = sleepTime ? abs(100.0 * usecLate / sleepTime) : 0.0;

    if (resetReason() == 5 || loopCount > 1) {
        OUT("slept %lld (%.1fs) rssi %d goal %.2fs rep %d beacon offset %d esp offset %d difference %d late (%.3f%%) scale %f", 
            spiffsSleepTime.read(), spiffsSleepTime.read()/1000000.0, b->rssi, 
            goal / 1000000.0, spiffsCurrentRep.read(), beaconDist, espDist, 
            usecLate, percentLate, spiffsScale.read());
        
        // Apply a conservative correction for the ESP32 sleep-clock rate.
        // Do not calibrate from the first wake if there is no prior interval.
        if (sleepTime > 0) {
            spiffsScale = spiffsScale - (1.0 * usecLate / sleepTime) * 0.3;
            spiffsScale = min(1.1F, max(0.9F, spiffsScale.read()));
        }
        
        // set for next sleep result
        spiffsCurrentRep = spiffsCurrentRep + 1;
        if (spiffsCurrentRep > 50 && b->ts % goal == b->ts % (goal * 2)) {
            spiffsCurrentGoal = spiffsCurrentGoal * 5;
            spiffsCurrentRep = 0;
        }        
    } else {
        spiffsScale = 1.004;
    }

    goal = spiffsCurrentGoal; // might have changed
    uint64_t ttg = goal - (b->ts % goal);
    if (ttg % goal < goal / 2)
        ttg += goal;
    uint64_t us = (ttg - (micros() - startUs - pktRxTime)) * spiffsScale;
    OUT("deep sleep %.1f sec, goal %.1f scale %f", us / 1000000.0, goal / 1000000.0, spiffsScale.read());
    fflush(stdout);
    uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
    
    // freshen up sleep calculation
    us = (ttg - (micros() - startUs - pktRxTime)) * spiffsScale;
    spiffsSleepTime = us;
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();        

    pktLog[0].count = 0;
    startUs = micros();
    setupPromisc();
}

int checks = 0;
#ifndef CSIM
void check(int ms) { 
    uint32_t startMs = millis();
    checks++;
    int s;
    while((s = WiFi.status()) != WL_CONNECTED && millis() - startMs < ms) {
        printf("WiFi.status() %d\n", s);
        delay(100);
        wdtReset();
    }
    if (WiFi.status() == WL_CONNECTED) { 
        OUT("connected check=%d", checks);
        delay(2000);
        ESP.restart();

    }
    OUT("not connected after %d", checks);
}
#endif

#define CK(x) err = (x); if (err != ESP_OK) printf("Error %d line %d\n", err, __LINE__)
#ifndef CSIM
void loop2() { // side investigation, try different wifi init methods to reliably connect 
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;
    //j.mqtt.active = false;
    OUT("connecting...\n");

    WiFi.begin("Station 54", "Local1747");
    check(20000);
    WiFi.disconnect();

    for(int i = 0; i < 3; i++) { 
        WiFi.begin("Station 54", "Local1747");
        check(2000);
        WiFi.disconnect();
    }
    WiFi.begin("Station 54", "Local1747");
    check(10000);
    WiFi.disconnect();

    WiFi.begin("Station 54", "Local1747");
    check(20000);
    //CK(esp_wifi_stop());
    WiFi.disconnect();
    CK(esp_wifi_stop());
    CK(esp_wifi_deinit());
    CK(esp_wifi_init(&cfg));
    CK(esp_wifi_start());
    CK(WiFi.disconnect());
    WiFi.begin("Station 54", "Local1747");
    check(10000);

    OUT("failed, rebooting\n");
    ESP.restart();

}
#endif

#include <map>
#if 1
void pretty_packet_handler(const WifiBeaconPacket &packet, void *) {
  static std::map<uint64_t,Info> beacons;

  if (packet.length >= 32 && (packet.data[0] & 0xfc) == 0x80) {
    uint64_t ts = beaconTsf(packet.data);
    uint64_t mac = beaconBssid(packet.data);
    if (!beacons.count(mac)) 
        beacons[mac] = Info();
    beacons[mac].ts = ts;
    beacons[mac].rssi = packet.rssi;
    beacons[mac].seen2 = packet.localTimestampUsec;
    beacons[mac].seen = packet.driverTimestampUsec;
    beacons[mac].count++;

    int maxCount = 0;
    for(auto p : beacons) {
        maxCount = max(maxCount, p.second.count);
    }
    int x = 1;
    int tilt = 1;
    printf("\033[%d;%dH", x*tilt, x++);
    printf("\\   % 12s % 15s   (% 7s) (% 5s) (% 6s)  \\ \n", 
        "Name", "Clock", "Strength", "Age", "Count");
    printf("\033[%d;%dH", x*tilt, x++);
    printf("\\---------------------------------------------------------------\\ \n");
    for(auto p : beacons) {
        if (p.second.count < maxCount / 100) 
            continue; 
        printf("\e[?25l");
        printf("\033[%d;%dH", x*tilt, x++);
        int h = floor(p.second.ts / 3600);
        int m = floor(p.second.ts - h * 3600) / 60;
        int s = (int)p.second.ts % 60;
        printf("\\   %012llx % 9d:%02d:%02d   (% 7d) (% 5.0fs) (% 6d)  \\ \n", 
            p.first, h,m,s, 
            p.second.rssi, min(9999.0, (micros() - p.second.seen) / 1000000.0), 
            min(p.second.count, 99999));
    }
    printf("\033[%d;%dH", x*tilt, x++);
    printf("\\---------------------------------------------------------------\\ \n"); 
  }
  return;

}
#endif
