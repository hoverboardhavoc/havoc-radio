// Download mode. TLS to GitHub needs more heap than the full app leaves, so the download runs in a boot of its own:
// the display, the knob and WiFi (station mode, the configured network) start; the receiver stays powered down and
// neither the web server nor the access point starts. mbedTLS is built for RSA key exchange only and the server's
// certificate is not verified: a guide table is checked as untrusted input, as an uploaded one is, and a firmware image
// is checked against its manifest's SHA-256 and by the SDK's image check, as the web upload's is by that check.
#include "download.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
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
#include "nvs.h"
#include "sdkconfig.h"
#include "ssd1306.h"
#include "store.h"
#include "tcpip_adapter.h"
#include "ui.h"

namespace download {

namespace {

const char* TAG = "download";
const char* kNamespace = "download";
constexpr uint32_t kJoinMs = 20000;           // joining the network
constexpr uint32_t kGuideMs = 90000;          // the whole guide download
constexpr uint32_t kFirmwareMs = 240000;      // the whole firmware update: about 1 MB
constexpr int kReceiveTimeoutMs = 15000;      // each request
constexpr uint32_t kFrameMs = 50;
constexpr uint32_t kTaskStack = 8192;
constexpr int kGotIp = 1;

EventGroupHandle_t s_events;
char s_ssid[33], s_pass[65];
Request s_request = Request::None;
const char* s_running = "";

// The download task's progress, read by the screen.
volatile ui::Download::Step s_step = ui::Download::Step::Join;
volatile uint32_t s_done = 0, s_total = 0;
volatile bool s_finished = false;
volatile uint8_t s_result = 0;
volatile Step s_at = Step::Join;  // the step the download is in, or ended in
// The install prompt: the version offered while the task waits, and the screen loop's answer.
char s_offered[kVersionMax];
volatile bool s_asking = false;
volatile ui::PromptAnswer s_answer = ui::PromptAnswer::None;
// The new slot is being selected: Back no longer cancels, so the result cannot say cancelled for an installed image.
volatile bool s_selecting = false;

uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
unsigned freeHeap() { return static_cast<unsigned>(esp_get_free_heap_size()); }

// ---- NVS: the request, the result for home and last_update ----

uint8_t getU8(const char* key) {
    nvs_handle h;
    uint8_t v = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}

void setU8(const char* key, uint8_t v) {
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    if (v)
        nvs_set_u8(h, key, v);
    else
        nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

class NvsRequest : public RequestStore {
public:
    Request get() override { return static_cast<Request>(getU8("req")); }
    void set(Request r) override { setU8("req", static_cast<uint8_t>(r)); }
    void clear() override { setU8("req", 0); }
};

// ---- the download task ----

void onEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) esp_wifi_connect();
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_events, kGotIp);
}

// One HTTPS GET to raw.githubusercontent.com per connection, with SNI and a 15 s receive timeout; a resumed body's
// request asks for its rest with Range.
class TlsHttp : public Http {
public:
    ~TlsHttp() { close(); }
    bool open(const char* path, uint32_t from) override {
        close();
        tls_ = esp_tls_init();
        if (!tls_) return false;
        esp_tls_cfg_t cfg = {};
        cfg.timeout_ms = kReceiveTimeoutMs;
        if (esp_tls_conn_new_sync(kHost, std::strlen(kHost), 443, &cfg, tls_) != 1) {
            close();
            return false;
        }
        ESP_LOGI(TAG, "connected, free heap %u", freeHeap());
        char range[40] = "";
        if (from) std::snprintf(range, sizeof range, "Range: bytes=%u-\r\n", static_cast<unsigned>(from));
        char req[288];
        const int n = snprintf(req, sizeof req,
                               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: havoc-radio\r\n%sConnection: close\r\n\r\n",
                               path, kHost, range);
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
        return static_cast<int>(esp_tls_conn_read(tls_, buf, n));  // below 0: the esp-tls/mbedTLS error
    }
    void close() override {
        if (tls_) esp_tls_conn_delete(tls_);
        tls_ = nullptr;
    }

private:
    esp_tls_t* tls_ = nullptr;
};

// The inactive app slot, written as the image arrives: esp_ota_write takes each piece straight to flash.
class OtaSlot : public Ota {
public:
    uint32_t capacity() override { return part_ ? part_->size : 0; }
    Err begin(uint32_t length) override {
        if (!part_) return Err::Write;
        ESP_LOGI(TAG, "erasing %s for %u bytes, free heap %u", part_->label, static_cast<unsigned>(length), freeHeap());
        return esp_ota_begin(part_, length, &h_) == ESP_OK ? Err::Ok : Err::Write;
    }
    Err write(const void* data, uint32_t n) override {
        return esp_ota_write(h_, data, n) == ESP_OK ? Err::Ok : Err::Write;
    }
    Err end() override {
        ESP_LOGI(TAG, "image check, free heap %u", freeHeap());
        const esp_err_t e = esp_ota_end(h_);
        return e == ESP_OK ? Err::Ok : e == ESP_ERR_OTA_VALIDATE_FAILED ? Err::Image : Err::Write;
    }
    Err activate() override {
        s_selecting = true;
        return esp_ota_set_boot_partition(part_) == ESP_OK ? Err::Ok : Err::Write;
    }
    void abort() override { esp_ota_end(h_); }

private:
    const esp_partition_t* part_ = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t h_ = 0;
};

class Screen : public Progress {
public:
    void step(Step s) override {
        s_step = s == Step::Table || s == Step::Image ? ui::Download::Step::Get
                 : s == Step::Prompt                  ? ui::Download::Step::Prompt
                                                      : ui::Download::Step::Check;
        s_at = s;
        ESP_LOGI(TAG, "%s, free heap %u", stepName(s), freeHeap());
    }
    void bytes(uint32_t done, uint32_t total) override {
        s_total = total;
        s_done = done;
    }
    void cut(uint32_t done, uint32_t total, int cut, int error) override {
        ESP_LOGW(TAG, "cut %d after %u of %u bytes, error %d (-0x%04x), free heap %u; %s", cut, static_cast<unsigned>(done),
                 static_cast<unsigned>(total), error, static_cast<unsigned>(-error), freeHeap(),
                 cut <= kResumes ? "resuming" : "no resumes left");
    }
};

// Waits for the screen loop's answer.
class KnobPrompt : public Prompt {
public:
    bool confirm(const char* version) override {
        std::snprintf(s_offered, sizeof s_offered, "%s", version);
        s_answer = ui::PromptAnswer::None;
        s_asking = true;
        ESP_LOGI(TAG, "offering %s", s_offered);
        while (s_answer == ui::PromptAnswer::None) vTaskDelay(pdMS_TO_TICKS(20));
        s_asking = false;
        return s_answer == ui::PromptAnswer::Install;
    }
};

void finish(Result r) {
    s_result = static_cast<uint8_t>(r);
    s_finished = true;
}

void task(void*) {
    if (!(xEventGroupWaitBits(s_events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(kJoinMs)) & kGotIp)) {
        ESP_LOGW(TAG, "result %d at step join: not joined", static_cast<int>(Result::NoNetwork));
        finish(Result::NoNetwork);
        vTaskDelete(nullptr);
    }
    ESP_LOGI(TAG, "joined, free heap %u", freeHeap());
    Report rep;
    {
        TlsHttp http;
        Screen screen;
        if (s_request == Request::Guide) {
            guide::Update update;
            rep = fetchGuide(http, update, guide::info(), screen);
        } else {
            char manifest[kPathMax], image[kPathMax];
            firmwarePaths(CONFIG_HAVOC_FIRMWARE_BRANCH, manifest, image);
            OtaSlot ota;
            KnobPrompt prompt;
            rep = fetchFirmware(http, ota, s_running, manifest, image, s_request == Request::Firmware ? &prompt : nullptr,
                                screen);
        }
    }
    ESP_LOGI(TAG, "result %d at step %s, error %d (%s); lowest free heap %u", static_cast<int>(rep.result),
             stepName(rep.step), static_cast<int>(rep.err), guide::errText(rep.err),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    s_at = rep.step;
    finish(rep.result);
    vTaskDelete(nullptr);
}

// Stores the result for home (not a guide's cancel) and for last_update, then restarts.
[[noreturn]] void restart(Result result) {
    const Kind kind = kindOf(s_request);
    LastUpdate u;
    u.result = result;
    u.step = s_at;
    u.kind = kind;
    u.lowestHeap = esp_get_minimum_free_heap_size();
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, "last", &u, sizeof u) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    if (result != Result::Cancelled || kind == Kind::Firmware) setU8("res", packResult(kind, result));
    esp_restart();
    for (;;) vTaskDelay(portMAX_DELAY);
}

}  // namespace

Request takeRequest() {
    // last_update covers the runs since power-up: a software restart (into and out of download mode) keeps it.
    if (esp_reset_reason() == ESP_RST_POWERON) {
        nvs_handle h;
        if (nvs_open(kNamespace, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_erase_key(h, "last") == ESP_OK) nvs_commit(h);
            nvs_close(h);
        }
    }
    NvsRequest r;
    return download::takeRequest(r);
}

void request(Request r) { setU8("req", static_cast<uint8_t>(r)); }

int requestUpdate(Request r, bool netSaved, char* body, size_t n) {
    NvsRequest s;
    return download::requestUpdate(r, netSaved, s, body, n);
}

bool lastUpdate(LastUpdate& out) {
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof out;
    const bool ok = nvs_get_blob(h, "last", &out, &n) == ESP_OK && n == sizeof out;
    nvs_close(h);
    return ok && resultName(out.result);
}

const char* takeResultText(int lang) {
    const uint8_t v = getU8("res");
    if (!v) return nullptr;
    setU8("res", 0);
    const bool fw = resultKind(v) == Kind::Firmware;
    using i18n::Str;
    switch (resultOf(v)) {
        case Result::Updated: return i18n::tr(lang, fw ? Str::firmware_updated : Str::guide_updated);
        case Result::UpToDate: return i18n::tr(lang, Str::up_to_date);
        case Result::NoNetwork: return i18n::tr(lang, Str::no_network);
        case Result::Failed: return i18n::tr(lang, Str::download_failed);
        case Result::CheckFailed: return i18n::tr(lang, fw ? Str::image_check_failed : Str::guide_check_failed);
        case Result::Cancelled: return fw ? i18n::tr(lang, Str::toast_cancelled) : nullptr;
        case Result::None: break;
    }
    return nullptr;
}

void run(int lang, Request req, const char* version) {
    s_request = req;
    s_running = version;
    const Kind kind = kindOf(req);
    ESP_LOGI(TAG, "%s download, free heap %u", kindName(kind), freeHeap());
    if (!store::loadWifi(s_ssid, sizeof s_ssid, s_pass, sizeof s_pass)) restart(Result::NoNetwork);
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
    xTaskCreate(task, "download", kTaskStack, nullptr, 5, nullptr);

    const uint32_t start = millis(), limit = kind == Kind::Guide ? kGuideMs : kFirmwareMs;
    uint32_t lastFrame = 0, askedAt = 0;
    bool asked = false;
    gfx::Frame frame;
    for (;;) {
        const uint32_t now = millis();
        input::EventQueue q;
        input::poll(now, q);
        if (s_asking && !asked) {
            asked = true;
            askedAt = now;
        }
        // The prompt: a press installs; a long press, the side button or 30 s without a press cancel. Elsewhere Back
        // cancels at any step until the new slot is being selected: the old table or the running firmware stays.
        for (int i = 0; i < q.n; i++) {
            const ui::EventType e = q.ev[i].type;
            if (s_asking && s_answer == ui::PromptAnswer::None) {
                s_answer = ui::promptAnswer(e);
            } else if (e == ui::EventType::Side && !s_asking && !s_selecting) {
                ESP_LOGI(TAG, "cancelled");
                restart(Result::Cancelled);
            }
        }
        if (s_asking && s_answer == ui::PromptAnswer::None && now - askedAt > ui::kPromptMs) {
            ESP_LOGI(TAG, "no answer in %u s", static_cast<unsigned>(ui::kPromptMs / 1000));
            s_answer = ui::PromptAnswer::Cancel;
        }
        if (s_finished) restart(static_cast<Result>(s_result));
        if (now - start > limit && !s_selecting) {
            ESP_LOGW(TAG, "result %d: the mode took longer than %u s", static_cast<int>(Result::Failed),
                     static_cast<unsigned>(limit / 1000));
            restart(Result::Failed);
        }
        if (now - lastFrame >= kFrameMs) {
            lastFrame = now;
            ui::Download d;
            d.step = s_step;
            d.firmware = kind == Kind::Firmware;
            d.ssid = s_ssid;
            d.version = s_offered;
            d.done = s_done;
            d.total = s_total;
            if (d.step == ui::Download::Step::Join && (xEventGroupGetBits(s_events) & kGotIp)) d.step = ui::Download::Step::Check;
            // The prompt shows once the task offers its version.
            if (d.step == ui::Download::Step::Prompt && !s_asking) d.step = ui::Download::Step::Check;
            ui::drawDownload(frame, lang, d);
            ssd1306::push(frame.buf);
        }
        vTaskDelay(1);
    }
}

}  // namespace download
