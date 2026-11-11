#include "web.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

#include "cJSON.h"
#include "download.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "gfx.h"
#include "guide.h"
#include "i18n_json.h"
#include "i18n_strings.h"
#include "lwip/sockets.h"
#include "net.h"
#include "power.h"
#include "radio.h"
#include "ssd1306.h"
#include "store.h"
#include "streams.h"
#include "utc_clock.h"

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
    // A 64-bit number, or null when negative; the SDK's printf has no %lld.
    void ms(int64_t v) {
        if (v < 0) return raw("null");
        char d[20];
        int n = 0;
        do {
            d[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (n) raw("%c", d[--n]);
    }
    const char* c_str() const { return b_; }
    size_t size() const { return len_; }
    size_t room() const { return cap_ - 1 - len_; }

private:
    char* b_;
    size_t cap_;
    size_t len_ = 0;
};

// RDS in FM, else null. The texts are the full UTF-8, as the receiver sent them.
void rdsJson(Json& j, const radio::State& s) {
    if (!radio::isFm(s.mode)) return j.raw("null");
    rds::Decoder d;
    radio::rdsState(d);
    char text[rds::kRtBuf];
    j.raw("{\"sync\":%s,\"pi\":", d.sync() ? "true" : "false");
    if (d.hasPi())
        j.raw("\"%04X\"", d.pi());
    else
        j.raw("null");
    d.ps(text, sizeof text, false);
    j.raw(",\"ps\":");
    j.str(text);
    d.rt(text, sizeof text, false);
    j.raw(",\"rt\":");
    j.str(text);
    j.raw(",\"pty\":%d,\"tp\":%s,\"ta\":%s,\"af\":[", d.pty(), d.tp() ? "true" : "false", d.ta() ? "true" : "false");
    for (int i = 0; i < d.afCount(); i++) j.raw("%s%ld", i ? "," : "", static_cast<long>(d.afHz(i)));
    j.raw("]}");
}

// The guide entries for the tuned frequency in AM, LSB and USB, as the Station screen orders them; null in FM. An
// entry that would not leave `reserve` bytes for the rest of the state is left out.
struct GuideOut {
    Json& j;
    size_t reserve;
};

void guideJson(const guide::Hits& h, bool am, void* arg) {
    auto* o = static_cast<GuideOut*>(arg);
    Json& j = o->j;
    if (!am) return j.raw("null");
    j.raw("[");
    // The longest entry: three 64-byte strings, each escaped to at most twice its length, and the fields.
    constexpr size_t kEntryMax = 3 * 2 * guide::kStrMax + 160;
    for (int i = 0; i < h.n && j.room() > kEntryMax + o->reserve; i++) {
        const guide::Hit& e = h.hit[i];
        j.raw("%s{\"name\":", i ? "," : "");
        j.str(e.name);
        j.raw(",\"start\":%u,\"end\":%u,\"days\":%u,\"irregular\":%s,\"language\":", e.e.start, e.e.end,
              e.e.days & guide::kEveryDay, e.e.days & guide::kIrregular ? "true" : "false");
        j.str(e.lang);
        j.raw(",\"target\":");
        j.str(e.target);
        j.raw(",\"on_air\":%s}", e.onAir ? "true" : "false");
    }
    j.raw("]");
}

// The guide scan: null before the first since power-up, else its scope, the band scanned (null for Scan all), whether
// it runs, the counts and the results strongest first with their names. A result that would not leave `reserve` bytes
// for the rest of the state is left out.
void scanJson(Json& j, size_t reserve) {
    scan::State sc;
    radio::scanState(sc);
    if (sc.phase == scan::Phase::None) return j.raw("null");
    j.raw("{\"scope\":\"%s\",\"band\":", sc.scope == scan::Scope::Band ? "band" : "all");
    if (sc.band >= 0 && sc.band < radio::kBandCount)
        j.str(radio::kBands[sc.band].name);
    else
        j.raw("null");
    j.raw(",\"running\":%s,\"done\":%u,\"total\":%u,\"results\":[", sc.phase == scan::Phase::Running ? "true" : "false",
          static_cast<unsigned>(sc.done), static_cast<unsigned>(sc.total));
    // The longest result: a 64-byte name escaped to at most twice its length, and the fields.
    constexpr size_t kResultMax = 2 * guide::kStrMax + 64;
    char name[guide::kStrMax + 1];
    for (int i = 0; i < sc.results.n && j.room() > kResultMax + reserve; i++) {
        const scan::Result& r = sc.results.r[i];
        guide::stringOf(sc.generation, r.name, name);
        j.raw("%s{\"khz\":%u,\"rssi\":%u,\"snr\":%u,\"name\":", i ? "," : "", static_cast<unsigned>(r.khz),
              static_cast<unsigned>(r.rssi), static_cast<unsigned>(r.snr));
        j.str(name);
        j.raw("}");
    }
    j.raw("]}");
}

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
    j.raw("],\"rds\":");
    rdsJson(j, s);
    const clk::Now now = clk::now();
    j.raw(",\"time\":{\"utc_ms\":");
    j.ms(now.utcMs);
    j.raw(",\"last_sync_ms\":");
    j.ms(now.lastSyncMs);
    j.raw(",\"sync\":%s,\"server\":", s.timeSync ? "true" : "false");
    j.str(s.ntpServer);
    j.raw("},\"scan\":");
    scanJson(j, 400);
    j.raw(",\"guide\":");
    GuideOut out{j, 320};
    guide::withHits(guideJson, &out);
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

// One buffer for every state JSON, allocated by start(): the handlers and the event push all run in the server task.
constexpr size_t kJsonCap = 4864;
char* s_json;

esp_err_t sendState(httpd_req_t* req, bool full) {
    if (!s_json) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "busy", 4);
    }
    Json j(s_json, kJsonCap);
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

bool sendChunk(void* req, const char* data, size_t len) {
    return httpd_resp_send_chunk(static_cast<httpd_req_t*>(req), data, len) == ESP_OK;
}

esp_err_t getStrings(httpd_req_t* req) {
    radio::State s;
    radio::snapshot(s);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (!i18n::writeJson(s.lang, sendChunk, req)) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
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

// The last RDS groups received, oldest first, each as "<ms> AAAA BBBB CCCC DDDD EE" (arrival time in ms since
// boot, blocks A to D and the block error levels BLEA..BLED in hex): the form the decoder's host tests replay.
esp_err_t getRdsGroups(httpd_req_t* req) {
    constexpr size_t kLine = 32;
    auto* raw = static_cast<rds::Raw*>(std::malloc(sizeof(rds::Raw) * rds::Ring::kLen));
    char* out = static_cast<char*>(std::malloc(rds::Ring::kLen * (kLine + 4) + 32));
    if (!raw || !out) {
        std::free(raw);
        std::free(out);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "busy", 4);
    }
    const int n = radio::rdsGroups(raw, rds::Ring::kLen);
    size_t len = std::strlen(std::strcpy(out, "{\"groups\":["));
    for (int i = 0; i < n; i++) {
        char line[kLine];
        rds::Ring::format(raw[i], line, sizeof line);
        len += std::sprintf(out + len, "%s\n\"%s\"", i ? "," : "", line);
    }
    len += std::sprintf(out + len, "\n]}\n");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_send(req, out, len);
    std::free(raw);
    std::free(out);
    return r;
}

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

bool setServer(radio::State& s, void* arg) { return radio::setNtpServer(s, static_cast<const char*>(arg)); }

// {"sync": bool} and/or {"server": s}; a server that fails the check answers 400 and nothing changes.
esp_err_t postTime(httpd_req_t* req) {
    cJSON* o = body(req);
    if (!o) return badRequest(req);
    const cJSON* sync = cJSON_GetObjectItem(o, "sync");
    const cJSON* server = cJSON_GetObjectItem(o, "server");
    const bool ok = (sync || server) && (!sync || cJSON_IsBool(sync)) &&
                    (!server || (cJSON_IsString(server) && radio::validNtpServer(server->valuestring)));
    if (ok) {
        if (sync) apply(radio::Cmd::TimeSync, cJSON_IsTrue(sync));
        if (server && radio::edit(setServer, server->valuestring)) store::touch();
    }
    cJSON_Delete(o);
    return ok ? sendState(req, false) : badRequest(req);
}

// ---- the guide scan ----

esp_err_t scanFail(httpd_req_t* req, const char* status, const char* error) {
    char b[64];
    const int n = snprintf(b, sizeof b, "{\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, n);
}

// {"scope": "band" | "all"}: starts a scan, answered with the state; 409 while one runs, in FM or (for the band)
// outside a band, 400 without a guide.
esp_err_t postScan(httpd_req_t* req) {
    cJSON* o = body(req);
    if (!o) return badRequest(req);
    const char* scope = text(o, "scope");
    const bool band = scope && !std::strcmp(scope, "band"), all = scope && !std::strcmp(scope, "all");
    cJSON_Delete(o);
    if (!band && !all) return badRequest(req);
    if (!guide::info().valid) return scanFail(req, "400 Bad Request", "no guide");
    switch (radio::startScan(band ? scan::Scope::Band : scan::Scope::All, clk::now().utcMs)) {
        case radio::ScanStart::Ok: return sendState(req, false);
        case radio::ScanStart::Busy: return scanFail(req, "409 Conflict", "scan running");
        case radio::ScanStart::NotAm: return scanFail(req, "409 Conflict", "not in AM, LSB or USB");
        case radio::ScanStart::NoBand: return scanFail(req, "409 Conflict", "not in a band");
    }
    return badRequest(req);
}

// Stops the scan running, with its results so far.
esp_err_t delScan(httpd_req_t* req) {
    radio::stopScan(false);
    return sendState(req, false);
}

// ---- server-sent events ----

// The open streams, touched only in the server task: the events handler, the push and the close callback.
Streams s_streams;
// A send to a stream waits at most this long; a send that does not finish in that time closes the stream.
constexpr int kStreamSendTimeoutMs = 200;
constexpr int kPushIntervalMs = 500;

bool sendChunk(int fd, const char* data, size_t n) {
    char head[12];
    const int h = snprintf(head, sizeof head, "%x\r\n", static_cast<unsigned>(n));
    return send(fd, head, h, 0) == h && send(fd, data, n, 0) == static_cast<int>(n) && send(fd, "\r\n", 2, 0) == 2;
}

// Closes a stream at once. With linger on and a zero timeout, lwIP resets the connection and frees its unsent data
// instead of keeping it for the retransmissions. The server closes the socket, so its sessions and onClose agree.
void dropStream(int fd, void* = nullptr) {
    const linger off = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &off, sizeof off);
    const esp_err_t r = httpd_sess_trigger_close(s_server, fd);
    ESP_LOGI(TAG, "event stream %d closed%s", fd, r == ESP_OK ? "" : ", close not queued");
}

esp_err_t getEvents(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (s_streams.full()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "busy", 4);
    }
    const int fd = httpd_req_to_sockfd(req);
    const timeval wait = {0, kStreamSendTimeoutMs * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
    // The first chunk sends the headers; later events go out as further chunks on the same socket.
    const char hello[] = "retry: 2000\n\n";
    if (httpd_resp_send_chunk(req, hello, sizeof hello - 1) != ESP_OK) return ESP_FAIL;
    s_streams.add(fd);
    return ESP_OK;
}

// An event goes only to a stream that can take it now: lwIP calls a socket writable once all its earlier data is
// acknowledged. Otherwise the stream skips this push.
Streams::Send sendEvent(int fd, void* arg) {
    const Json* j = static_cast<const Json*>(arg);
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval now = {0, 0};
    if (select(fd + 1, nullptr, &writable, nullptr, &now) != 1) return Streams::Send::Skipped;
    return sendChunk(fd, j->c_str(), j->size()) ? Streams::Send::Sent : Streams::Send::Failed;
}

void pushEvents(void*) {
    if (!s_json || !s_streams.count()) return;
    Json j(s_json, kJsonCap);
    j.raw("event: state\ndata: ");
    stateJson(j, false);
    j.raw("\n\n");
    s_streams.push(sendEvent, dropStream, &j);
}

void onClose(httpd_handle_t, int fd) {
    s_streams.remove(fd);
    close(fd);
}

// Twice a second: state with RSSI/SNR to every open event stream, sent from the server task. The push is queued
// from this task rather than a timer, so the timer task never waits on lwIP.
void sseTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPushIntervalMs));
        httpd_queue_work(s_server, pushEvents, nullptr);
    }
}

// ---- the frequency guide ----

// Uploads (guide tables and firmware) arrive in pieces of this size, into a buffer that lives for the request.
constexpr size_t kRecvBuf = 1024;

// `,"last_update":{...}` for the last download since power-up, of either kind, or nothing before one.
void lastUpdate(char* out, size_t n) {
    out[0] = 0;
    download::LastUpdate u;
    if (!download::lastUpdate(u)) return;
    out[0] = ',';
    download::lastUpdateJson(u, out + 1, n - 1);
}

// {"season", "entries", "length", "sha256", "source_modified"} for the loaded table, or {"entries": 0}.
esp_err_t sendGuide(httpd_req_t* req) {
    const guide::Info i = guide::info();
    char buf[384], last[160];
    Json j(buf, sizeof buf);
    lastUpdate(last, sizeof last);
    if (!i.valid) {
        j.raw("{\"entries\":0%s}", last);
    } else {
        char sha[65], when[24];
        for (int k = 0; k < 32; k++) snprintf(sha + 2 * k, 3, "%02x", i.sha256[k]);
        const time_t t = i.modified;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
        j.raw("{\"season\":");
        j.str(i.season);
        j.raw(",\"entries\":%u,\"length\":%u,\"sha256\":\"%s\",\"source_modified\":\"%s\"%s}",
              static_cast<unsigned>(i.entries), static_cast<unsigned>(i.length), sha, when, last);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, j.c_str(), j.size());
}

esp_err_t getGuide(httpd_req_t* req) { return sendGuide(req); }

volatile bool s_restart = false;

// A download request: 409 without a configured network, else stored, 202, and the UI loop restarts once the answer
// is sent.
esp_err_t requestDownload(httpd_req_t* req, download::Request r) {
    net::Info ni;
    net::info(ni);
    char body[48];
    const int status = download::requestUpdate(r, ni.saved, body, sizeof body);
    httpd_resp_set_status(req, status == 202 ? "202 Accepted" : "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t e = httpd_resp_send(req, body, std::strlen(body));
    if (status == 202) s_restart = true;
    return e;
}

// The same request as Update guide on the radio.
esp_err_t postGuideUpdate(httpd_req_t* req) { return requestDownload(req, download::Request::Guide); }

esp_err_t guideFail(httpd_req_t* req, const char* why) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, why, std::strlen(why));
}

// The raw table, at most one slot, with an optional X-Guide-SHA256. It is written to the free slot and checked there;
// the slots switch only when it passes, so a failed upload leaves the old table in use.
esp_err_t postGuide(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > guide::kSlotSize) return guideFail(req, guide::errText(guide::Err::TooLong));
    uint8_t sha[32];
    bool haveSha = false;
    char hex[72];
    if (httpd_req_get_hdr_value_str(req, "X-Guide-SHA256", hex, sizeof hex) == ESP_OK) {
        if (std::strlen(hex) != 64) return guideFail(req, "X-Guide-SHA256 is not 64 hex digits");
        for (int k = 0; k < 32; k++) {
            unsigned v;
            if (std::sscanf(hex + 2 * k, "%2x", &v) != 1) return guideFail(req, "X-Guide-SHA256 is not 64 hex digits");
            sha[k] = static_cast<uint8_t>(v);
        }
        haveSha = true;
    }
    guide::Update update;
    guide::Err e = update.begin(req->content_len);
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(kRecvBuf));
    if (!buf) e = guide::Err::Write;
    struct Free {
        uint8_t* p;
        ~Free() { std::free(p); }
    } release{buf};
    size_t left = req->content_len;
    while (e == guide::Err::Ok && left) {
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(buf), left < kRecvBuf ? left : kRecvBuf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            e = guide::Err::Cut;
            break;
        }
        e = update.add(buf, r);
        left -= r;
    }
    if (e == guide::Err::Ok) e = update.finish(haveSha ? sha : nullptr);
    if (e != guide::Err::Ok) {
        ESP_LOGW(TAG, "guide upload refused: %s", guide::errText(e));
        return guideFail(req, guide::errText(e));
    }
    return sendGuide(req);
}

// ---- the screen ----

// The last frame sent to the display, as a binary PBM with lit pixels 1.
esp_err_t getScreen(httpd_req_t* req) {
    static const char kHead[] = "P4\n128 32\n";
    constexpr size_t kHeadLen = sizeof kHead - 1;
    static_assert(kHeadLen + 2 * gfx::kBytes <= kJsonCap, "the PBM and the frame fit the JSON buffer");
    uint8_t* frame = reinterpret_cast<uint8_t*>(s_json) + kHeadLen + gfx::kBytes;
    if (!s_json || !ssd1306::lastFrame(frame)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "busy", 4);
    }
    std::memcpy(s_json, kHead, kHeadLen);
    gfx::pbmRows(frame, reinterpret_cast<uint8_t*>(s_json) + kHeadLen);
    httpd_resp_set_type(req, "image/x-portable-bitmap");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, s_json, kHeadLen + gfx::kBytes);
}

// ---- firmware ----

// {"version", "slot", "last_update"}: the running version and app slot, and the last download since power-up.
esp_err_t getFirmware(httpd_req_t* req) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    char buf[256], last[160];
    lastUpdate(last, sizeof last);
    Json j(buf, sizeof buf);
    j.raw("{\"version\":");
    j.str(s_version);
    j.raw(",\"slot\":");
    j.str(running ? running->label : "");
    j.raw("%s}", last);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, j.c_str(), j.size());
}

// The same request as Update in Settings > Firmware, without the install prompt.
esp_err_t postFirmwareUpdate(httpd_req_t* req) { return requestDownload(req, download::Request::FirmwareApi); }

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
    char* buf = static_cast<char*>(std::malloc(kRecvBuf));
    if (!buf) {
        esp_ota_end(h);
        return otaFail(req, "500 Internal Server Error", "out of memory");
    }
    size_t left = req->content_len;
    while (left) {
        const int r = httpd_req_recv(req, buf, left < kRecvBuf ? left : kRecvBuf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || esp_ota_write(h, buf, r) != ESP_OK) {
            std::free(buf);
            esp_ota_end(h);
            return otaFail(req, "400 Bad Request", "upload failed");
        }
        left -= r;
    }
    std::free(buf);
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

bool takeRestart() {
    if (!s_restart) return false;
    s_restart = false;
    return true;
}

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
    s_json = static_cast<char*>(std::malloc(kJsonCap));
    s_decided = xSemaphoreCreateBinary();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 26;
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
    handle("/api/rds/groups", HTTP_GET, getRdsGroups);
    handle("/api/presets", HTTP_POST, postPresets);
    handle("/api/presets", HTTP_DELETE, delPresets);
    handle("/api/wifi", HTTP_POST, postWifi);
    handle("/api/time", HTTP_POST, postTime);
    handle("/api/events", HTTP_GET, getEvents);
    handle("/api/ota", HTTP_POST, postOta);
    handle("/api/guide", HTTP_GET, getGuide);
    handle("/api/guide", HTTP_POST, postGuide);
    handle("/api/guide/update", HTTP_POST, postGuideUpdate);
    handle("/api/firmware", HTTP_GET, getFirmware);
    handle("/api/firmware/update", HTTP_POST, postFirmwareUpdate);
    handle("/api/scan", HTTP_POST, postScan);
    handle("/api/scan", HTTP_DELETE, delScan);
    handle("/api/screen", HTTP_GET, getScreen);
    xTaskCreate(sseTask, "sse", 2048, nullptr, 2, nullptr);
}

}  // namespace web
