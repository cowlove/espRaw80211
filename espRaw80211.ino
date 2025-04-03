#include "jimlib.h"
#ifndef ESP32
#error Only the ESP32 is supported
#endif 
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <rom/uart.h>

typedef struct {
    unsigned protocol:2;
    unsigned type:2;
    unsigned subtype:4;
    unsigned ignore1:8;
    unsigned long recv_addr:48; 
    unsigned long send_addr:48; 
    unsigned ignore2:32;
    uint64_t timestamp;
} raw_beacon_packet_t;

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

template<> string toString(const uint64_t &x) { return sfmt("%ullx", x); }
template<> bool fromString(const string &s, uint64_t &x) { return sscanf(s.c_str(), "%ullx", &x) == 1; }

void intr_oneShot(void *buf, wifi_promiscuous_pkt_type_t type) {
    uint64_t seen2 = micros();
    const wifi_promiscuous_pkt_t *pt = (wifi_promiscuous_pkt_t*)buf; 
    const raw_beacon_packet_t *pk = (raw_beacon_packet_t*)pt->payload;

    if (pk->subtype == 0x8 && pk->send_addr == intr_beacon) {
        int i = 0;
        pktLog[i].ssid = pk->send_addr;
        pktLog[i].seen2 = seen2;
        pktLog[i].seen = pt->rx_ctrl.timestamp;
        pktLog[i].rssi = pt->rx_ctrl.rssi;
        pktLog[i].count++;
        pktLog[i].ts = pk->timestamp;
        esp_wifi_set_promiscuous_rx_cb(NULL);
    }
}

//int score(const Info &i) { return (110 - i.rssi) * i.count; }
int score(const Info &i) { return i.count; }
void intr_collect(void *buf, wifi_promiscuous_pkt_type_t type) {
    uint64_t seen2 = micros();
    const wifi_promiscuous_pkt_t *pt = (wifi_promiscuous_pkt_t*)buf; 
    const raw_beacon_packet_t *pk = (raw_beacon_packet_t*)pt->payload;
    if (pk->subtype == 0x8) {
        //printf("%07.3f MAC: %06llx ts: %016llx RSSI: % 4d\n", 
        //    millis() / 1000.0, pk->send_addr, pk->timestamp, pt->rx_ctrl.rssi);
        int i;
        for(i = 0; i < sizeof(pktLog)/sizeof(pktLog[0]); i++) {
            if(pktLog[i].ssid == pk->send_addr) {
                pktLog[i].seen2 = seen2;
                pktLog[i].seen = pt->rx_ctrl.timestamp;
                pktLog[i].rssi = (pt->rx_ctrl.rssi + pktLog[i].count * pktLog[i].rssi) / (pktLog[i].count + 1);
                pktLog[i].count++;
                pktLog[i].ts = pk->timestamp;
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
            pktLog[worst].ssid = pk->send_addr;
            intr_collect(buf, type);
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

void pretty_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type);

void setupPromisc() { 
    intr_beacon = spiffsBeacon; 
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_wifi_init(&cfg);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_disconnect();
    esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(1);
    wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(intr_oneShot);
}

//JStuff j;

void setupPromisc2() { 
    intr_beacon = spiffsBeacon;
    esp_wifi_set_promiscuous(1);
    wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter); 
    esp_wifi_set_promiscuous_rx_cb(intr_collect);
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
    printf("%09.3f setup() waiting for %llx\n", millis()/1000.0, spiffsBeacon.read());
    esp_task_wdt_init(25, true);
    esp_task_wdt_add(NULL);
    setupPromisc();
}


static uint64_t startUs = 0;
static int loopCount = 0;

void loop() {
    loopCount++;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_task_wdt_reset();
    if (pktLog[0].count == 0 && millis() - startUs / 1000 < 10000) {
        delay(1);
        return;
    }
    Info resultPkt = pktLog[0];
    Info *b = &resultPkt;

    if (resultPkt.count == 0) {
        OUT("No beacon packet received, picking new beacon", millis()); 
        esp_wifi_set_promiscuous(0);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_wifi_init(&cfg);
        esp_wifi_start();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_disconnect();
        esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
        wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT};
        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(intr_collect);
        esp_wifi_set_promiscuous(1);
        delay(250);
        esp_wifi_set_promiscuous(0);
        esp_wifi_set_promiscuous_rx_cb(NULL);

        int best = 0, i;
        for(i = 0; i < sizeof(pktLog)/sizeof(pktLog[0]); i++) {
            if (pktLog[i].ssid != 0 && score(pktLog[i]) >= score(pktLog[best]))
                best = i;
        }
        OUT("best beacon: %02d %012llx %3d %6d %016llx %016llx", 
            best, pktLog[best].ssid, pktLog[best].rssi, pktLog[best].count);
        
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_wifi_init(&cfg);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        b = &pktLog[best];
        spiffsBeacon = b->ssid;
    }

    if (esp_rom_get_reset_reason(0) != 5) {
        spiffsCurrentGoal = 2.4 * 1000000;
        spiffsCurrentRep = 0;
    }
    uint64_t goal = spiffsCurrentGoal;
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
    float percentLate = abs(100.0 * usecLate / spiffsSleepTime.read());

    if (esp_rom_get_reset_reason(0) == 5 || loopCount > 1) { 
        OUT("slept %lld (%.1fs) rssi %d goal %.2fs rep %d beacon offset %d esp offset %d difference %d late (%.3f%%) scale %f", 
            spiffsSleepTime.read(), spiffsSleepTime.read()/1000000.0, b->rssi, 
            goal / 1000000.0, spiffsCurrentRep.read(), beaconDist, espDist, 
            usecLate, percentLate, spiffsScale.read());
        
        spiffsScale = spiffsScale - (1.0 * usecLate / spiffsSleepTime) * 0.3;
        spiffsScale = min(1.1F, max(0.9F, spiffsScale.read()));
        
        // set for next sleep result
        spiffsCurrentRep = spiffsCurrentRep + 1;
        if (spiffsCurrentRep > 50 && b->ts % goal == b->ts % (goal * 2)) {
            spiffsCurrentGoal = spiffsCurrentGoal * 5;
            spiffsCurrentRep = 0;
        }        
    } else {
        spiffsScale = 1.004;
    }
    spiffsScale = 1.0; // TMP disable scaling for data collection

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

#define CK(x) err = (x); if (err != ESP_OK) printf("Error %d line %d\n", err, __LINE__)
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

#include <map>
#if 1
void pretty_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *pt = (wifi_promiscuous_pkt_t*)buf; 
    const raw_beacon_packet_t *pk = (raw_beacon_packet_t*)pt->payload;


  static std::map<uint64_t,Info> beacons;

  if (pk->subtype == 0x8) {
    uint64_t ts = pk->timestamp;
    uint64_t mac = pk->send_addr;
    if (!beacons.count(mac)) 
        beacons[mac] = Info();
    beacons[mac].ts = ts;
    beacons[mac].rssi = pt->rx_ctrl.rssi;
    beacons[mac].seen2 = micros();
    beacons[mac].seen = pt->rx_ctrl.timestamp;
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