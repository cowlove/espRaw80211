//#include "jimlib.h"
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
void raw_packet_handler3(void *buf, wifi_promiscuous_pkt_type_t type) {
    uint64_t seen2 = micros();
    const wifi_promiscuous_pkt_t *pt = (wifi_promiscuous_pkt_t*)buf; 
    const raw_beacon_packet_t *pk = (raw_beacon_packet_t*)pt->payload;

    if (pk->subtype == 0x8 && pk->send_addr == 0x0000d36d6092) {
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

int score(const Info &i) { return (110 - i.rssi) * i.count; }
void raw_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type) {
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
            raw_packet_handler(buf, type);
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
    int wifi_channel = 1;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_disconnect();
    esp_wifi_set_promiscuous(1);
    wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter); 
    esp_wifi_set_promiscuous_rx_cb(raw_packet_handler3);
    esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
}

//JStuff j;

void setupPromisc2() { 
    esp_wifi_set_promiscuous(1);
    wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter); 
    esp_wifi_set_promiscuous_rx_cb(raw_packet_handler);
}

void setup() {
    printf("%09.3f setup()\n", millis()/1000.0);
    //setupPromisc();
    //j.onConn = ([]() { setupPromisc2(); });
    //j.begin();
    esp_task_wdt_init(5, true);
    esp_task_wdt_add(NULL);
    setupPromisc();
}

void loop() {
    esp_task_wdt_reset();
    if (pktLog[0].count > 0) { 
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_task_wdt_reset();

        Info *b = &pktLog[0];
        uint64_t goal = 0x1000000; // 0x1000000 is about 16 sec

        int beaconDist = (int)(b->ts % goal);
        if (beaconDist > goal / 2) { 
            beaconDist -= goal;
        }
        int espDist = (int)(b->seen2 % goal);
        if (espDist > goal / 2) { 
            espDist -= goal;
        }
        uint64_t ttg = 4 * goal - (b->ts % goal);
        if (ttg % goal < goal / 2)
            ttg += goal;

        uint64_t us = ttg - (micros() - b->seen);

        if (esp_rom_get_reset_reason(0) == 5) {
        printf("%09.3f sleep %.3f mac %012llx %d goal %x beacon %08llx (%d) esp %08llx (%d) difference %d reset reason %d\n", 
            millis()/1000.0, us/1000000.0, b->ssid, b->rssi, (int)goal, b->ts % goal, beaconDist, b->seen2 % goal, espDist, 
            espDist - beaconDist, esp_rom_get_reset_reason(0));
        }
        printf("%09.3f deep sleep %.2f sec\n", millis()/1000.0, us / 1000000.0);

        fflush(stdout);
        uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
        
        us = ttg - (micros() - b->seen);
        esp_sleep_enable_timer_wakeup(us);
        esp_deep_sleep_start();        
        pktLog[0].count = 0;
        setupPromisc();
    }
    delay(1);
}
#include <map>
#if 0
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
    beacons[mac].lastSeen = microseconds();
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
            p.second.rssi, min(9999.0, (millis() - p.second.lastSeen) / 1000.0), 
            min(p.second.count, 99999));
    }
    printf("\033[%d;%dH", x*tilt, x++);
    printf("\\---------------------------------------------------------------\\ \n"); 
  }
  return;

}
#endif