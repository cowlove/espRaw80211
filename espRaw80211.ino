#ifndef ESP32
#error Only the ESP32 is supported
#endif 
#include <WiFi.h>
#include <esp_wifi.h>

typedef struct {
    unsigned protocol:2;
    unsigned type:2;
    unsigned subtype:4;
    unsigned ignore1:8;
    unsigned recv_addr:48; 
    unsigned send_addr:48; 
    unsigned ignore2:32;
    uint64_t timestamp;
} raw_beacon_packet_t;

void raw_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *pt = (wifi_promiscuous_pkt_t*)buf; 
    const raw_beacon_packet_t *pk = (raw_beacon_packet_t*)pt->payload;
    if (pk->subtype == 0x8) {
        printf("%07.3f MAC: %06llx timestamp: %016llx RSSI: % 4d\n", 
            millis() / 1000.0, pk->send_addr, pk->timestamp, pt->rx_ctrl.rssi);
    }
}  

void pretty_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type);

void setup() {
    Serial.begin(921600);
    int wifi_channel = 1;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_disconnect();
    esp_wifi_set_promiscuous(1);
    esp_wifi_set_promiscuous_rx_cb(pretty_packet_handler);
    wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter); 
    esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
}

void loop() {
    delay(1);
}

#include <map>
void pretty_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *pt = (wifi_promiscuous_pkt_t*)buf; 
    const raw_beacon_packet_t *pk = (raw_beacon_packet_t*)pt->payload;

  struct Info { 
    Info() {}
    Info(double _ts, int _rssi, uint32_t _last) : ts(_ts), rssi(_rssi), lastSeen(_last) {}
    double ts;
    int rssi;
    int count = 0;
    uint32_t lastSeen;
  };

  static std::map<uint64_t,Info> beacons;

  if (pk->subtype == 0x8) {
    uint64_t ts = pk->timestamp;
    uint64_t mac = pk->send_addr;
    if (!beacons.count(mac)) 
        beacons[mac] = Info();
    beacons[mac].ts = ts / 1000000.0;
    beacons[mac].rssi = pt->rx_ctrl.rssi;
    beacons[mac].lastSeen = millis();
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
