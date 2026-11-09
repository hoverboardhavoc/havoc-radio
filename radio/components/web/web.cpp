#include "web.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "i18n_strings.h"
#include "lwip/sockets.h"
#include "net.h"
#include "power.h"
#include "radio.h"
#include "store.h"

extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

namespace web {

namespace {

const char* TAG = "web";
httpd_handle_t s_server;
const char* s_version = "";

// ---- JSON output ----

class Json {
public:
    Json(char* buf, size_t cap) : b_(buf), cap_(cap) { b_[0] = 0; }
    void raw(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(b_ + len_, len_ < cap_ ? cap_ - len_ : 0, fmt, ap);
        va_end(ap);
        if (n > 0) len_ = len_ + n < cap_ ? len_ + n : cap_ - 1;
    }
    void str(const char* s) {
        raw("\"");
        for (; *s; s++) {
            const unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\')
                raw("\\%c", c);
            else if (c < 0x20)
                raw("\\u%04x", c);
            else
                raw("%c", c);
        }
        raw("\"");
    }
    const char* c_str() const { return b_; }
    size_t size() const { return len_; }

private:
    char* b_;
    size_t cap_;
    size_t len_ = 0;
};

void stateJson(Json& j, bool full) {
    radio::State s;
    radio::snapshot(s);
    const radio::Live live = radio::live();
    net::Info ni;
    net::info(ni);
    char num[16];
    const char* unit;
    radio::freqText(s, num, sizeof num, &unit);
    const radio::Family fam = radio::family(s.mode);
    j.raw("{\"hz\":%ld,\"freq\":\"%s\",\"unit\":\"%s\",\"mode\":\"%s\",\"band\":", static_cast<long>(s.hz), num, unit,
          radio::modeName(s.mode));
    j.str(radio::bandLabel(s.hz, s.mode));
    int n;
    const radio::Step* steps = radio::steps(fam, &n);
    j.raw(",\"stepIndex\":%d,\"stepHz\":%ld,\"steps\":[", s.stepIdx[fam], static_cast<long>(radio::currentStep(s).hz));
    for (int i = 0; i < n; i++) j.raw("%s\"%s\"", i ? "," : "", steps[i].label);
    const char* const* bws = radio::bandwidths(fam, &n);
    j.raw("],\"bwIndex\":%d,\"bandwidths\":[", s.bwIdx[fam]);
    for (int i = 0; i < n; i++) j.raw("%s\"%s\"", i ? "," : "", bws[i]);
    j.raw("],\"volume\":%d,\"muted\":%s,\"agc\":%s,\"rssi\":%d,\"snr\":%d,\"bars\":%d,\"seeking\":%s,\"ssb\":%s",
          s.volume, s.muted ? "true" : "false", s.agc ? "true" : "false", live.rssi, live.snr, live.bars,
          live.seeking ? "true" : "false", live.ssb ? "true" : "false");
    j.raw(",\"lang\":\"%s\",\"version\":\"%s\",\"battery\":%d,\"wifi\":{\"joined\":%s,\"ap\":%s,\"ssid\":",
          i18n::kLangCode[s.lang < i18n::kLangCount ? s.lang : 0], s_version, power::percent(),
          ni.joined ? "true" : "false", ni.ap ? "true" : "false");
    j.str(ni.joined ? ni.ssid : "");
    j.raw(",\"ip\":\"%s\"},\"presets\":[", ni.ip);
    for (int i = 0; i < s.presetCount; i++) {
        const radio::Preset& p = s.presets[i];
        char text[16];
        radio::presetFreqText(p, text, sizeof text);
        j.raw("%s{\"name\":", i ? "," : "");
        j.str(p.name);
        j.raw(",\"hz\":%ld,\"mode\":\"%s\",\"text\":\"%s\"}", static_cast<long>(p.hz), radio::modeName(p.mode), text);
    }
    j.raw("]");
    if (full) {
        j.raw(",\"bands\":[");
        for (int i = 0; i < radio::kBandCount; i++) j.raw("%s\"%s\"", i ? "," : "", radio::kBands[i].name);
        j.raw("]");
    }
    j.raw("}");
}

// ---- request helpers ----

constexpr size_t kBodyMax = 512;

cJSON* body(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > kBodyMax) return nullptr;
    char buf[kBodyMax + 1];
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return nullptr;
        got += r;
    }
    buf[got] = 0;
    return cJSON_Parse(buf);
}

esp_err_t sendState(httpd_req_t* req, bool full) {
    static char buf[3072];
    Json j(buf, sizeof buf);
    stateJson(j, full);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, j.c_str(), j.size());
}

esp_err_t badRequest(httpd_req_t* req) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "bad request", 11);
}

void apply(radio::Cmd c, int32_t v) {
    if (radio::submit({c, v})) store::touch();
}

int num(const cJSON* o, const char* key, bool* ok) {
    const cJSON* it = cJSON_GetObjectItem(o, key);
    *ok = cJSON_IsNumber(it);
    return *ok ? it->valueint : 0;
}

const char* text(const cJSON* o, const char* key) {
    const cJSON* it = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(it) ? it->valuestring : nullptr;
}

// ---- handlers ----

esp_err_t getPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

esp_err_t getState(httpd_req_t* req) { return sendState(req, true); }

esp_err_t getStrings(httpd_req_t* req) {
    radio::State s;
    radio::snapshot(s);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    const char* json = i18n::kJson[s.lang < i18n::kLangCount ? s.lang : 0];
    return httpd_resp_send(req, json, std::strlen(json));
}

// One handler for the simple setters; the URI picks the field.
esp_err_t postSetting(httpd_req_t* req) {
    cJSON* o = body(req);
    if (!o) return badRequest(req);
    bool ok = false;
    const char* uri = req->uri;
    if (!std::strcmp(uri, "/api/tune")) {
        const int hz = num(o, "hz", &ok);
        if (ok) apply(radio::Cmd::Tune, hz);
    } else if (!std::strcmp(uri, "/api/band")) {
        const char* name = text(o, "band");
        for (int i = 0; name && i < radio::kBandCount; i++)
            if (!std::strcmp(name, radio::kBands[i].name)) {
                apply(radio::Cmd::Band, i);
                ok = true;
            }
    } else if (!std::strcmp(uri, "/api/mode")) {
        radio::Mode m;
        const char* name = text(o, "mode");
        if (name && radio::parseMode(name, &m)) {
            apply(radio::Cmd::SetMode, static_cast<int>(m));
            ok = true;
        }
    } else if (!std::strcmp(uri, "/api/step")) {
        const int i = num(o, "index", &ok);
        if (ok) apply(radio::Cmd::Step, i);
    } else if (!std::strcmp(uri, "/api/bandwidth")) {
        const int i = num(o, "index", &ok);
        if (ok) apply(radio::Cmd::Bandwidth, i);
    } else if (!std::strcmp(uri, "/api/volume")) {
        const int v = num(o, "volume", &ok);
        if (ok) apply(radio::Cmd::Volume, v);
        const cJSON* m = cJSON_GetObjectItem(o, "mute");
        if (cJSON_IsBool(m)) {
            apply(radio::Cmd::Mute, cJSON_IsTrue(m));
            ok = true;
        }
    }
    cJSON_Delete(o);
    return ok ? sendState(req, false) : badRequest(req);
}

struct PresetEdit {
    const cJSON* body;
    bool tune;
    int tuneIndex;
};

bool editPresets(radio::State& s, void* arg) {
    auto* e = static_cast<PresetEdit*>(arg);
    const char* action = text(e->body, "action");
    if (!action) return false;
    bool ok1, ok2;
    if (!std::strcmp(action, "add")) {
        const char* name = text(e->body, "name");
        return radio::presetAdd(s, name && *name ? name : radio::bandLabel(s.hz, s.mode), s.hz, s.mode);
    }
    if (!std::strcmp(action, "rename")) {
        const char* name = text(e->body, "name");
        return name && radio::presetRename(s, num(e->body, "index", &ok1), name) && ok1;
    }
    if (!std::strcmp(action, "move")) {
        const int from = num(e->body, "from", &ok1), to = num(e->body, "to", &ok2);
        return ok1 && ok2 && radio::presetMove(s, from, to);
    }
    if (!std::strcmp(action, "tune")) {
        e->tuneIndex = num(e->body, "index", &ok1);
        e->tune = ok1;
    }
    return false;
}

esp_err_t postPresets(httpd_req_t* req) {
    cJSON* o = body(req);
    if (!o) return badRequest(req);
    PresetEdit e{o, false, 0};
    const bool changed = radio::edit(editPresets, &e);
    cJSON_Delete(o);
    if (changed) store::touch();
    if (e.tune) apply(radio::Cmd::TunePreset, e.tuneIndex);
    if (!changed && !e.tune) return badRequest(req);
    return sendState(req, false);
}

esp_err_t getPresets(httpd_req_t* req) { return sendState(req, false); }

bool deletePreset(radio::State& s, void* arg) { return radio::presetDelete(s, *static_cast<int*>(arg)); }

esp_err_t delPresets(httpd_req_t* req) {
    char q[32], v[8];
    int i = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "index", v, sizeof v) == ESP_OK)
        i = std::atoi(v);
    if (!radio::edit(deletePreset, &i)) return badRequest(req);
    store::touch();
    return sendState(req, false);
}

esp_err_t postWifi(httpd_req_t* req) {
    cJSON* o = body(req);
    if (!o) return badRequest(req);
    const char* ssid = text(o, "ssid");
    const char* pass = text(o, "password");
    const bool ok = ssid && *ssid && std::strlen(ssid) <= 32 && (!pass || std::strlen(pass) <= 64);
    if (ok) net::join(ssid, pass ? pass : "");
    cJSON_Delete(o);
    return ok ? sendState(req, false) : badRequest(req);
}

// ---- server-sent events ----

constexpr int kMaxStreams = 3;
int s_streams[kMaxStreams] = {-1, -1, -1};

bool sendChunk(int fd, const char* data, size_t n) {
    char head[12];
    const int h = snprintf(head, sizeof head, "%x\r\n", static_cast<unsigned>(n));
    return send(fd, head, h, 0) == h && send(fd, data, n, 0) == static_cast<int>(n) && send(fd, "\r\n", 2, 0) == 2;
}

esp_err_t getEvents(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const int fd = httpd_req_to_sockfd(req);
    int slot = -1;
    for (int i = 0; i < kMaxStreams; i++)
        if (s_streams[i] < 0 && slot < 0) slot = i;
    if (slot < 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "busy", 4);
    }
    // The first chunk sends the headers; later events go out as further chunks on the same socket.
    const char hello[] = "retry: 2000\n\n";
    if (httpd_resp_send_chunk(req, hello, sizeof hello - 1) != ESP_OK) return ESP_FAIL;
    s_streams[slot] = fd;
    return ESP_OK;
}

void pushEvents(void*) {
    static char buf[2560];
    Json j(buf, sizeof buf);
    j.raw("event: state\ndata: ");
    stateJson(j, false);
    j.raw("\n\n");
    for (int i = 0; i < kMaxStreams; i++) {
        if (s_streams[i] < 0) continue;
        if (!sendChunk(s_streams[i], j.c_str(), j.size())) {
            httpd_sess_trigger_close(s_server, s_streams[i]);
            s_streams[i] = -1;
        }
    }
}

void onClose(httpd_handle_t, int fd) {
    for (int i = 0; i < kMaxStreams; i++)
        if (s_streams[i] == fd) s_streams[i] = -1;
    close(fd);
}

// ---- firmware upload ----

SemaphoreHandle_t s_decided;
volatile bool s_confirmReq = false;
volatile bool s_install = false;
constexpr int kConfirmWaitMs = 31000;

esp_err_t otaFail(httpd_req_t* req, const char* status, const char* msg) {
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, msg, std::strlen(msg));
}

esp_err_t postOta(httpd_req_t* req) {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part || req->content_len == 0 || req->content_len > part->size)
        return otaFail(req, "400 Bad Request", "bad image size");
    esp_ota_handle_t h;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK) return otaFail(req, "500 Internal Server Error", "begin failed");
    static char buf[1024];
    size_t left = req->content_len;
    while (left) {
        const int r = httpd_req_recv(req, buf, left < sizeof buf ? left : sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || esp_ota_write(h, buf, r) != ESP_OK) {
            esp_ota_end(h);
            return otaFail(req, "400 Bad Request", "upload failed");
        }
        left -= r;
    }
    if (esp_ota_end(h) != ESP_OK) return otaFail(req, "400 Bad Request", "image not valid");
    ESP_LOGI(TAG, "image verified, waiting for the knob");
    xSemaphoreTake(s_decided, 0);
    s_install = false;
    s_confirmReq = true;
    const bool answered = xSemaphoreTake(s_decided, pdMS_TO_TICKS(kConfirmWaitMs)) == pdTRUE;
    if (!answered || !s_install) return otaFail(req, "403 Forbidden", "not confirmed");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return otaFail(req, "500 Internal Server Error", "boot switch failed");
    httpd_resp_send(req, "installing", 10);
    xTimerStart(xTimerCreate("reboot", pdMS_TO_TICKS(1000), pdFALSE, nullptr, [](TimerHandle_t) { esp_restart(); }), 0);
    return ESP_OK;
}

void handle(const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = fn;
    httpd_register_uri_handler(s_server, &u);
}

}  // namespace

bool takeConfirmRequest() {
    if (!s_confirmReq) return false;
    s_confirmReq = false;
    return true;
}

void decide(bool install) {
    s_install = install;
    xSemaphoreGive(s_decided);
}

void start(const char* version) {
    s_version = version;
    s_decided = xSemaphoreCreateBinary();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.close_fn = onClose;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "server start failed");
        return;
    }
    handle("/", HTTP_GET, getPage);
    handle("/api/state", HTTP_GET, getState);
    handle("/api/strings", HTTP_GET, getStrings);
    for (const char* u : {"/api/tune", "/api/band", "/api/mode", "/api/step", "/api/bandwidth", "/api/volume"})
        handle(u, HTTP_POST, postSetting);
    handle("/api/presets", HTTP_GET, getPresets);
    handle("/api/presets", HTTP_POST, postPresets);
    handle("/api/presets", HTTP_DELETE, delPresets);
    handle("/api/wifi", HTTP_POST, postWifi);
    handle("/api/events", HTTP_GET, getEvents);
    handle("/api/ota", HTTP_POST, postOta);
    // Twice a second: state with RSSI/SNR to every open event stream, sent from the server task.
    xTimerStart(xTimerCreate("sse", pdMS_TO_TICKS(500), pdTRUE, nullptr,
                             [](TimerHandle_t) { httpd_queue_work(s_server, pushEvents, nullptr); }),
                0);
}

}  // namespace web
