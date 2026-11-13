#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "clock_core.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "lwip/apps/sntp.h"
#include "lwip/tcpip.h"
#include "store.h"
#include "tcpip_adapter.h"  // before mdns.h, which needs its lwIP address types
#include "mdns.h"

namespace net {

namespace {

const char* TAG = "net";
SemaphoreHandle_t s_lock;
Info s_info;
char s_ssid[33];
char s_pass[65];
bool s_haveCreds = false;
TimerHandle_t s_joinTimer;   // fires when the station has not joined in time
TimerHandle_t s_retryTimer;  // periodic rejoin while the access point is up

clk::SntpRule s_sntp;  // the UI loop's view of the client
// The client's server name. lwIP keeps the pointer, so it lives here, written only in the TCP/IP task.
char s_sntpServer[clk::SntpRule::kServerMax + 1];

// A change for the client that lwIP has not taken yet: the latest one stands for all before it, since the job
// stops the client and starts it again unless the change is a stop.
clk::Sntp s_sntpPending = clk::Sntp::None;

struct SntpJob {
    clk::Sntp action;
    char server[clk::SntpRule::kServerMax + 1];
};

// Runs in the TCP/IP task, where lwIP's SNTP calls belong.
void sntpJob(void* arg) {
    auto* job = static_cast<SntpJob*>(arg);
    if (sntp_enabled()) sntp_stop();
    if (job->action != clk::Sntp::Stop) {
        std::strcpy(s_sntpServer, job->server);
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, s_sntpServer);
        sntp_init();
    }
    ESP_LOGI(TAG, "sntp %s %s", job->action == clk::Sntp::Stop ? "stopped" : "started", s_sntpServer);
    delete job;
}

class Lock {
public:
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

void startAp() {
    {
        Lock lock;
        if (s_info.ap) return;
        s_info.ap = true;
    }
    wifi_config_t ap = {};
    std::strcpy(reinterpret_cast<char*>(ap.ap.ssid), kApSsid);
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApSsid));
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap);
    ESP_LOGI(TAG, "access point %s up", kApSsid);
    if (s_haveCreds) xTimerStart(s_retryTimer, 0);
    Lock lock;
    if (!s_info.joined) std::strcpy(s_info.ip, "192.168.4.1");
}

void stopAp() {
    {
        Lock lock;
        if (!s_info.ap) return;
        s_info.ap = false;
    }
    xTimerStop(s_retryTimer, 0);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "access point down");
}

void connect() {
    if (!s_haveCreds) return;
    wifi_config_t sta = {};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), s_ssid, sizeof sta.sta.ssid);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), s_pass, sizeof sta.sta.password);
    esp_wifi_disconnect();
    esp_wifi_set_config(ESP_IF_WIFI_STA, &sta);
    esp_wifi_connect();
    xTimerStart(s_joinTimer, 0);
}

uint8_t barsFor(int rssi) { return rssi > -60 ? 3 : rssi > -75 ? 2 : 1; }

void scanDone() {
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    static wifi_ap_record_t recs[32];
    esp_wifi_scan_get_ap_records(&n, recs);
    std::sort(recs, recs + n, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    Lock lock;
    s_info.netCount = 0;
    for (int i = 0; i < n && s_info.netCount < kMaxNets; i++) {
        const char* ssid = reinterpret_cast<const char*>(recs[i].ssid);
        if (!ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < s_info.netCount; k++) dup |= !std::strcmp(s_info.nets[k].ssid, ssid);
        if (dup) continue;
        ui::Network& net = s_info.nets[s_info.netCount++];
        std::strncpy(net.ssid, ssid, sizeof net.ssid - 1);
        net.ssid[sizeof net.ssid - 1] = 0;
        net.bars = barsFor(recs[i].rssi);
    }
}

void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_haveCreds)
            connect();
        else
            startAp();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was;
        {
            Lock lock;
            was = s_info.joined;
            s_info.joined = false;
            if (s_info.ap) std::strcpy(s_info.ip, "192.168.4.1");
        }
        if (was) connect();  // lost an established link: try again, the join timer starts the AP if needed
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scanDone();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        xTimerStop(s_joinTimer, 0);
        {
            Lock lock;
            s_info.joined = true;
            std::strncpy(s_info.ssid, s_ssid, sizeof s_info.ssid - 1);
            snprintf(s_info.ip, sizeof s_info.ip, "%s", ip4addr_ntoa(&ev->ip_info.ip));
        }
        ESP_LOGI(TAG, "joined %s as %s", s_ssid, s_info.ip);
        stopAp();
    }
}

}  // namespace

void start() {
    s_lock = xSemaphoreCreateMutex();
    s_haveCreds = store::loadWifi(s_ssid, sizeof s_ssid, s_pass, sizeof s_pass);
    tcpip_adapter_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);
    s_joinTimer = xTimerCreate("join", pdMS_TO_TICKS(kJoinTimeoutMs), pdFALSE, nullptr, [](TimerHandle_t) {
        bool joined;
        {
            Lock lock;
            joined = s_info.joined;
        }
        if (!joined) startAp();
    });
    s_retryTimer = xTimerCreate("rejoin", pdMS_TO_TICKS(kRejoinIntervalMs), pdTRUE, nullptr,
                                [](TimerHandle_t) { connect(); });
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    mdns_init();
    mdns_hostname_set("radio");
    mdns_instance_name_set("Havoc Radio");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

void join(const char* ssid, const char* password) {
    std::strncpy(s_ssid, ssid, sizeof s_ssid - 1);
    s_ssid[sizeof s_ssid - 1] = 0;
    std::strncpy(s_pass, password, sizeof s_pass - 1);
    s_pass[sizeof s_pass - 1] = 0;
    s_haveCreds = true;
    store::saveWifi(s_ssid, s_pass);
    connect();
}

void scan() {
    wifi_scan_config_t cfg = {};
    esp_wifi_scan_start(&cfg, false);
}

void info(Info& out) {
    Lock lock;
    out = s_info;
}

void timeSync(bool on, const char* server) {
    bool joined, ap, hasIp;
    {
        Lock lock;
        joined = s_info.joined;
        ap = s_info.ap;
        hasIp = joined && s_info.ip[0];
    }
    const clk::Sntp action = s_sntp.update(on, joined, ap, hasIp, server);
    if (action != clk::Sntp::None) s_sntpPending = action;
    if (s_sntpPending == clk::Sntp::None) return;
    // The UI loop never waits on lwIP: with its mailbox full or no memory, the change waits for the next pass.
    auto* job = new (std::nothrow) SntpJob{s_sntpPending, {}};
    if (!job) return;
    std::strcpy(job->server, s_sntp.server());
    if (tcpip_try_callback(sntpJob, job) != ERR_OK) {
        delete job;
        return;
    }
    s_sntpPending = clk::Sntp::None;
}

}  // namespace net
