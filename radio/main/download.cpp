// Download mode. TLS to GitHub needs more heap than the full app leaves, so the download runs in a boot of its own:
// the display, the knob and WiFi (station mode, the configured network) start; the receiver stays powered down and
// neither the web server nor the access point starts. mbedTLS is built for RSA key exchange only and the server's
// certificate is not verified: the table is checked as untrusted input, as an uploaded one is.
#include "download.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "guide.h"
#include "i18n_strings.h"
#include "input.h"
#include "ssd1306.h"
#include "store.h"
#include "tcpip_adapter.h"
#include "ui.h"

namespace download {

namespace {

const char* TAG = "download";
constexpr uint32_t kJoinMs = 20000;     // joining the network
constexpr uint32_t kModeMs = 90000;     // the whole mode
constexpr int kReceiveTimeoutMs = 15000;  // each request
constexpr uint32_t kFrameMs = 50;
constexpr uint32_t kTaskStack = 8192;
constexpr int kGotIp = 1;

EventGroupHandle_t s_events;
char s_ssid[33], s_pass[65];

// The download task's progress, read by the screen.
volatile ui::Download::Step s_step = ui::Download::Step::Join;
volatile uint32_t s_done = 0, s_total = 0;
volatile bool s_finished = false;
volatile uint8_t s_result = 0;
volatile guide::Step s_at = guide::Step::Join;  // the step the download is in, or ended in

uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void onEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) esp_wifi_connect();
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_events, kGotIp);
}

// One HTTPS GET to raw.githubusercontent.com per connection, with SNI and a 15 s receive timeout.
class TlsHttp : public guide::Http {
public:
    ~TlsHttp() { close(); }
    bool open(const char* path) override {
        close();
        tls_ = esp_tls_init();
        if (!tls_) return false;
        esp_tls_cfg_t cfg = {};
        cfg.timeout_ms = kReceiveTimeoutMs;
        if (esp_tls_conn_new_sync(guide::kHost, std::strlen(guide::kHost), 443, &cfg, tls_) != 1) {
            close();
            return false;
        }
        char req[256];
        const int n = snprintf(req, sizeof req,
                               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: havoc-radio\r\nConnection: close\r\n\r\n",
                               path, guide::kHost);
        for (int sent = 0; sent < n;) {
            const ssize_t w = esp_tls_conn_write(tls_, req + sent, n - sent);
            if (w <= 0) {
                close();
                return false;
            }
            sent += w;
        }
        return true;
    }
    int read(uint8_t* buf, int n) override {
        if (!tls_) return -1;
        const ssize_t r = esp_tls_conn_read(tls_, buf, n);
        return r < 0 ? -1 : static_cast<int>(r);
    }
    void close() override {
        if (tls_) esp_tls_conn_delete(tls_);
        tls_ = nullptr;
    }

private:
    esp_tls_t* tls_ = nullptr;
};

class Screen : public guide::Progress {
public:
    void step(guide::Step s) override {
        s_step = s == guide::Step::Table ? ui::Download::Step::Get : ui::Download::Step::Check;
        s_at = s;
        ESP_LOGI(TAG, "%s, free heap %u", guide::stepName(s), static_cast<unsigned>(esp_get_free_heap_size()));
    }
    void bytes(uint32_t done, uint32_t total) override {
        s_total = total;
        s_done = done;
    }
};

void finish(guide::Result r) {
    s_result = static_cast<uint8_t>(r);
    s_finished = true;
}

void task(void*) {
    if (!(xEventGroupWaitBits(s_events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(kJoinMs)) & kGotIp)) {
        ESP_LOGW(TAG, "result %d at step join: not joined", static_cast<int>(guide::Result::NoNetwork));
        finish(guide::Result::NoNetwork);
        vTaskDelete(nullptr);
    }
    ESP_LOGI(TAG, "joined, free heap %u", static_cast<unsigned>(esp_get_free_heap_size()));
    guide::Report rep;
    {
        guide::Update update;
        TlsHttp http;
        Screen screen;
        rep = guide::fetch(http, update, guide::info(), screen);
    }
    ESP_LOGI(TAG, "result %d at step %s, error %d (%s); lowest free heap %u", static_cast<int>(rep.result),
             guide::stepName(rep.step), static_cast<int>(rep.err), guide::errText(rep.err),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    s_at = rep.step;
    finish(rep.result);
    vTaskDelete(nullptr);
}

// Stores the result for home (not for a cancel) and for GET /api/guide's last_update, then restarts.
[[noreturn]] void restart(guide::Result result) {
    guide::LastUpdate u;
    u.result = result;
    u.step = s_at;
    u.lowestHeap = esp_get_minimum_free_heap_size();
    store::setLastUpdate(u);
    if (result != guide::Result::Cancelled) store::setGuideResult(static_cast<uint8_t>(result));
    esp_restart();
    for (;;) vTaskDelay(portMAX_DELAY);
}

}  // namespace

const char* resultText(int lang, uint8_t result) {
    switch (static_cast<guide::Result>(result)) {
        case guide::Result::Updated: return i18n::tr(lang, i18n::Str::guide_updated);
        case guide::Result::UpToDate: return i18n::tr(lang, i18n::Str::up_to_date);
        case guide::Result::NoNetwork: return i18n::tr(lang, i18n::Str::no_network);
        case guide::Result::Failed: return i18n::tr(lang, i18n::Str::download_failed);
        case guide::Result::CheckFailed: return i18n::tr(lang, i18n::Str::guide_check_failed);
        case guide::Result::Cancelled:
        case guide::Result::None: break;
    }
    return nullptr;
}

void run(int lang) {
    ESP_LOGI(TAG, "guide download, free heap %u", static_cast<unsigned>(esp_get_free_heap_size()));
    if (!store::loadWifi(s_ssid, sizeof s_ssid, s_pass, sizeof s_pass)) restart(guide::Result::NoNetwork);
    input::start();
    s_events = xEventGroupCreate();
    tcpip_adapter_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);
    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), s_ssid, sizeof wc.sta.ssid);
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), s_pass, sizeof wc.sta.password);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wc);
    esp_wifi_start();
    xTaskCreate(task, "guide", kTaskStack, nullptr, 5, nullptr);

    const uint32_t start = millis();
    uint32_t lastFrame = 0;
    gfx::Frame frame;
    for (;;) {
        const uint32_t now = millis();
        input::EventQueue q;
        input::poll(now, q);
        // Back cancels at any step: the old table stays in use, as the switch comes only after every check.
        for (int i = 0; i < q.n; i++)
            if (q.ev[i].type == ui::EventType::Side) {
                ESP_LOGI(TAG, "cancelled");
                restart(guide::Result::Cancelled);
            }
        if (s_finished) restart(static_cast<guide::Result>(s_result));
        if (now - start > kModeMs) {
            ESP_LOGW(TAG, "result %d: the mode took longer than %u s", static_cast<int>(guide::Result::Failed),
                     static_cast<unsigned>(kModeMs / 1000));
            restart(guide::Result::Failed);
        }
        if (now - lastFrame >= kFrameMs) {
            lastFrame = now;
            ui::Download d;
            d.step = s_step;
            d.ssid = s_ssid;
            d.done = s_done;
            d.total = s_total;
            if (d.step == ui::Download::Step::Join && (xEventGroupGetBits(s_events) & kGotIp)) d.step = ui::Download::Step::Check;
            ui::drawDownload(frame, lang, d);
            ssd1306::push(frame.buf);
        }
        vTaskDelay(1);
    }
}

}  // namespace download
