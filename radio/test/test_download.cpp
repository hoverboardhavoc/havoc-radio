// The download mode's core with a fake transport. The guide: up to date, an update, a manifest of another format, a
// length that does not match or exceeds a slot, a hash mismatch, a table failing its checks, a cut-off body. The
// firmware, with a fake app slot: up to date, older, newer with the prompt confirmed and cancelled, no prompt from the
// API, a length that does not match or exceeds the slot, a hash mismatch, an image the SDK's check refuses, a cut-off
// body. The version comparison, the HTTP response head, the manifest parser, the request cleared before the download
// starts, and last_update.
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "guide_fakes.h"
#include "download_core.h"
#include "util.h"

using download::Err;
using download::Result;
using download::Step;

namespace {

std::string table(const char* name) { return readFile(t::root() + "/radio/test/guide/" + name); }

std::string hex(const uint8_t* p, int n) {
    std::string s;
    char b[3];
    for (int i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

std::string sha(const std::string& d) {
    guide::Sha256 h;
    h.update(d.data(), d.size());
    uint8_t out[32];
    h.finish(out);
    return hex(out, 32);
}

std::string manifest(const std::string& t, int format = 1, long length = -1, std::string digest = "") {
    return "{\n  \"format\": " + std::to_string(format) + ",\n  \"season\": \"B26\",\n  \"entries\": 8780,\n  \"length\": " +
           std::to_string(length < 0 ? static_cast<long>(t.size()) : length) + ",\n  \"sha256\": \"" +
           (digest.empty() ? sha(t) : digest) +
           "\",\n  \"source_modified\": \"2026-10-31T12:00:00Z\",\n  \"csv\": \"eibi/sked-b26.csv\"\n}\n";
}

std::string response(const std::string& body, long contentLength = -2, const char* status = "200 OK",
                     const char* extra = "") {
    std::string h = std::string("HTTP/1.1 ") + status + "\r\nConnection: close\r\nContent-Type: text/plain\r\n" +
                    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; sandbox" +
                    std::string(200, ' ') + "\r\n" + extra;
    if (contentLength != -1)
        h += "Content-Length: " + std::to_string(contentLength == -2 ? static_cast<long>(body.size()) : contentLength) + "\r\n";
    return h + "X-Fastly-Request-ID: 1234\r\n\r\n" + body;
}

// Serves canned responses by path, a few bytes at a time.
struct FakeHttp : download::Http {
    std::map<std::string, std::string> responses;
    std::vector<std::string> opened;
    std::string cur;
    size_t pos = 0;
    int chunk = 700;
    bool open(const char* path) override {
        opened.push_back(path);
        auto it = responses.find(path);
        if (it == responses.end()) return false;
        cur = it->second;
        pos = 0;
        return true;
    }
    int read(uint8_t* buf, int n) override {
        if (pos >= cur.size()) return 0;
        const size_t k = std::min<size_t>({static_cast<size_t>(n), cur.size() - pos, static_cast<size_t>(chunk)});
        std::memcpy(buf, cur.data() + pos, k);
        pos += k;
        return static_cast<int>(k);
    }
    void close() override {}
};

struct SlotSink : guide::Sink {
    guide::Slots& s;
    uint8_t sector[guide::kSector];
    guide::Slots::Writer w;
    explicit SlotSink(guide::Slots& slots) : s(slots), w(slots, sector) {}
    Err begin(uint32_t length) override { return w.begin(length); }
    Err add(const void* d, uint32_t n) override { return w.add(d, n); }
    Err finish(const uint8_t* sha256) override {
        const Err e = w.finish(sha256);
        if (e == Err::Ok) s.commit(w);
        return e;
    }
};

struct Steps : download::Progress {
    std::string log;
    uint32_t last = 0, total = 0;
    int updates = 0;
    void step(Step s) override { log += std::string(download::stepName(s)) + " "; }
    void bytes(uint32_t done, uint32_t t) override {
        if (done < last) log += "backwards ";
        last = done, total = t;
        updates++;
    }
};

struct Rig {
    FakeFlash flash;
    guide::Slots slots{flash};
    FakeHttp http;
    Steps steps;
    std::string old = table("rows.bin"), fresh = table("b26.bin");
    Rig() {
        flash.place(0, old);
        slots.boot();
        http.responses[download::kGuideManifestPath] = response(manifest(fresh));
        http.responses[download::kGuideTablePath] = response(fresh);
    }
    download::Report run() {
        SlotSink sink(slots);
        return download::fetchGuide(http, sink, slots.info(), steps);
    }
    bool oldInUse() { return slots.active() == 0 && slots.info().entries == 28; }
};

}  // namespace

TEST(download_update) {
    Rig r;
    const download::Report rep = r.run();
    CHECK(rep.result == Result::Updated);
    CHECK_EQ(r.slots.active(), 1);
    CHECK_STR(r.slots.info().season, "B26");
    CHECK_STR(r.steps.log, "manifest table check ");
    CHECK_EQ(r.steps.last, r.fresh.size());
    CHECK(r.steps.updates > 100);
    CHECK_EQ(r.http.opened.size(), 2u);
    // At boot the new table is the one read; the old slot's first sector is erased.
    guide::Slots again(r.flash);
    again.boot();
    CHECK_EQ(again.active(), 1);
    CHECK_EQ(r.flash.m[0], 0xFF);
}

TEST(download_up_to_date) {
    Rig r;
    CHECK(r.run().result == Result::Updated);
    r.steps.log.clear();
    const download::Report rep = r.run();
    CHECK(rep.result == Result::UpToDate);
    CHECK_STR(r.steps.log, "manifest ");
    CHECK_EQ(r.http.opened.size(), 3u);  // the table is not fetched again
    CHECK_EQ(r.slots.active(), 1);
}

TEST(download_failures_leave_the_old_table) {
    {
        Rig r;  // a manifest of another format
        r.http.responses[download::kGuideManifestPath] = response(manifest(r.fresh, 2));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Manifest && rep.err == Err::Format);
        CHECK_EQ(r.http.opened.size(), 1u);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // the table's length is not the manifest's
        r.http.responses[download::kGuideManifestPath] = response(manifest(r.fresh, 1, static_cast<long>(r.fresh.size()) + 1));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Table && rep.err == Err::Length);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a manifest length above a slot
        r.http.responses[download::kGuideManifestPath] = response(manifest(r.fresh, 1, guide::kSlotSize + 1));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.err == Err::TooLong);
        CHECK_EQ(r.http.opened.size(), 1u);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a hash mismatch
        r.http.responses[download::kGuideManifestPath] = response(manifest(r.fresh, 1, -1, std::string(64, '0')));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Check && rep.err == Err::Hash);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a table failing its checks, with a manifest that matches it
        std::string bad = r.fresh;
        bad[200] ^= 1;
        r.http.responses[download::kGuideManifestPath] = response(manifest(bad));
        r.http.responses[download::kGuideTablePath] = response(bad);
        const download::Report rep = r.run();
        CHECK(rep.result == Result::CheckFailed && rep.err == Err::Crc);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a body cut off before Content-Length
        r.http.responses[download::kGuideTablePath] = response(r.fresh.substr(0, 90000), static_cast<long>(r.fresh.size()));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Table && rep.err == Err::Cut);
        CHECK(r.oldInUse());
        guide::Slots again(r.flash);
        again.boot();
        CHECK_EQ(again.active(), 0);
    }
    {
        Rig r;  // a manifest over 2 KB, a 404, no connection, chunked encoding, no Content-Length
        r.http.responses[download::kGuideManifestPath] = response(manifest(r.fresh) + std::string(2048, ' '));
        CHECK(r.run().result == Result::Failed);
        r.http.responses[download::kGuideManifestPath] = response("404: Not Found", -2, "404 Not Found");
        CHECK(r.run().result == Result::Failed);
        r.http.responses.erase(download::kGuideManifestPath);
        CHECK(r.run().result == Result::Failed);
        r.http.responses[download::kGuideManifestPath] = response(manifest(r.fresh));
        r.http.responses[download::kGuideTablePath] = response(r.fresh, -1, "200 OK", "Transfer-Encoding: chunked\r\n");
        CHECK(r.run().result == Result::Failed);
        r.http.responses[download::kGuideTablePath] = response(r.fresh, -1);
        CHECK(r.run().result == Result::Failed);
        r.http.responses[download::kGuideManifestPath] = response("{\"format\": 1, \"length\": 5");
        CHECK(r.run().result == Result::Failed);
        CHECK(r.oldInUse());
    }
}

TEST(download_without_a_loaded_table) {
    FakeFlash flash;
    guide::Slots slots(flash);
    slots.boot();
    FakeHttp http;
    const std::string t = table("b26.bin");
    http.responses[download::kGuideManifestPath] = response(manifest(t));
    http.responses[download::kGuideTablePath] = response(t);
    http.chunk = 1;  // a byte at a time through the head and body
    SlotSink sink(slots);
    Steps steps;
    CHECK(download::fetchGuide(http, sink, slots.info(), steps).result == Result::Updated);
    CHECK_EQ(slots.active(), 0);
}

TEST(download_response_head) {
    FakeHttp http;
    http.responses["/a"] = "HTTP/1.1 200 OK\r\ncontent-length:  5\r\n\r\nhelloEXTRA";
    http.open("/a");
    download::Response a(http);
    CHECK(a.begin());
    CHECK_EQ(a.length(), 5);
    uint8_t b[16];
    CHECK_EQ(a.body(b, 16), 5);
    CHECK(!std::memcmp(b, "hello", 5));
    CHECK_EQ(a.body(b, 16), 0);
    for (const char* bad : {"HTTP/1.1 301 Moved\r\nContent-Length: 0\r\n\r\n", "SSH-2.0\r\n\r\n", "HTTP/1.1 200 OK\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: -4\r\n\r\n", ""}) {
        http.responses["/b"] = bad;
        http.open("/b");
        download::Response r(http);
        CHECK(!r.begin());
    }
}

TEST(download_manifest_parser) {
    download::Manifest m;
    const std::string t = table("b26.bin");
    const std::string conv = table("b26.bin.json");  // as the converter writes it
    CHECK(download::parseManifest(conv.data(), conv.size(), m));
    CHECK_EQ(m.format, 1);
    CHECK_EQ(m.length, static_cast<int64_t>(t.size()));
    CHECK(m.hasSha);
    CHECK_STR(hex(m.sha256, 32), sha(t));
    const char* ok = "{\"x\": {\"a\": [1, \"}\"]}, \"format\": 1, \"note\": \"a \\\"q\\\" \\u00e9\", \"length\": 10, "
                     "\"sha256\": \"" "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF" "\"}";
    CHECK(download::parseManifest(ok, std::strlen(ok), m));
    CHECK(m.format == 1 && m.length == 10 && m.hasSha && m.sha256[31] == 0xFF);
    for (const char* bad : {"", "[]", "{", "{\"format\" 1}", "{\"format\": 1,}", "{\"format\": 1} x", "{\"a\": tru}"})
        CHECK(!download::parseManifest(bad, std::strlen(bad), m));
    const char* badSha = "{\"sha256\": \"zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}";
    CHECK(download::parseManifest(badSha, std::strlen(badSha), m) && !m.hasSha);
}

namespace {

struct Store : download::RequestStore {
    download::Request on = download::Request::None;
    std::string log;
    download::Request get() override {
        log += "get ";
        return on;
    }
    void set(download::Request r) override { on = r; }
    void clear() override {
        log += "clear ";
        on = download::Request::None;
    }
};

}  // namespace

TEST(download_request_cleared_first) {
    using download::Request;
    for (Request r : {Request::Guide, Request::Firmware, Request::FirmwareApi}) {
        Store s;
        s.on = r;
        CHECK(download::takeRequest(s) == r);
        CHECK_STR(s.log, "get clear ");
        CHECK(s.on == Request::None);
        CHECK(download::takeRequest(s) == Request::None);  // after a crash and restart, no second download
        CHECK_STR(s.log, "get clear get ");
    }
    Store s;
    s.on = static_cast<Request>(9);  // not a request: cleared and ignored
    CHECK(download::takeRequest(s) == Request::None);
    CHECK(s.on == Request::None);
    CHECK(download::kindOf(Request::Guide) == download::Kind::Guide);
    CHECK(download::kindOf(Request::Firmware) == download::Kind::Firmware);
    CHECK(download::kindOf(Request::FirmwareApi) == download::Kind::Firmware);
}

TEST(download_update_request_from_the_web) {
    using download::Request;
    for (Request r : {Request::Guide, Request::FirmwareApi}) {
        Store s;
        char body[64];
        CHECK_EQ(download::requestUpdate(r, false, s, body, sizeof body), 409);
        CHECK_STR(body, "{\"error\":\"no network\"}");
        CHECK(s.on == Request::None);  // nothing stored, nothing restarts
        CHECK_EQ(download::requestUpdate(r, true, s, body, sizeof body), 202);
        CHECK(s.on == r);
        CHECK(download::takeRequest(s) == r && s.on == Request::None);
    }
}

TEST(download_last_update_json) {
    char out[160];
    download::LastUpdate u;
    download::lastUpdateJson(u, out, sizeof out);
    CHECK_STR(out, "");  // before a run
    using download::Kind;
    const struct {
        Result r;
        Step step;
        Kind kind;
        const char* json;
    } cases[] = {
        {Result::Updated, Step::Check, Kind::Guide,
         "{\"kind\":\"guide\",\"result\":\"updated\",\"step\":\"check\",\"lowest_heap\":9900}"},
        {Result::UpToDate, Step::Manifest, Kind::Guide,
         "{\"kind\":\"guide\",\"result\":\"up to date\",\"step\":\"manifest\",\"lowest_heap\":9900}"},
        {Result::NoNetwork, Step::Join, Kind::Firmware,
         "{\"kind\":\"firmware\",\"result\":\"no network\",\"step\":\"join\",\"lowest_heap\":9900}"},
        {Result::Failed, Step::Table, Kind::Guide,
         "{\"kind\":\"guide\",\"result\":\"download failed\",\"step\":\"table\",\"lowest_heap\":9900}"},
        {Result::Failed, Step::Image, Kind::Firmware,
         "{\"kind\":\"firmware\",\"result\":\"download failed\",\"step\":\"image\",\"lowest_heap\":9900}"},
        {Result::CheckFailed, Step::Check, Kind::Firmware,
         "{\"kind\":\"firmware\",\"result\":\"check failed\",\"step\":\"check\",\"lowest_heap\":9900}"},
        {Result::Cancelled, Step::Prompt, Kind::Firmware,
         "{\"kind\":\"firmware\",\"result\":\"cancelled\",\"step\":\"prompt\",\"lowest_heap\":9900}"},
    };
    for (const auto& c : cases) {
        u = download::LastUpdate();
        u.result = c.r;
        u.step = c.step;
        u.kind = c.kind;
        u.lowestHeap = 9900;
        download::lastUpdateJson(u, out, sizeof out);
        CHECK_STR(out, std::string("\"last_update\":") + c.json);
    }
}

// The byte home's result is stored as: the guide's results keep their values, the firmware's carry the kind.
TEST(download_result_byte) {
    using download::Kind;
    CHECK_EQ(download::packResult(Kind::Guide, Result::Updated), 1);
    for (Kind k : {Kind::Guide, Kind::Firmware})
        for (Result r : {Result::Updated, Result::UpToDate, Result::NoNetwork, Result::Failed, Result::CheckFailed,
                         Result::Cancelled}) {
            const uint8_t v = download::packResult(k, r);
            CHECK(v != 0);
            CHECK(download::resultKind(v) == k && download::resultOf(v) == r);
        }
}

TEST(download_versions) {
    const struct {
        const char* a;
        const char* b;
        int want;
    } cases[] = {
        {"2026.46.0", "2026.46.0", 0},
        {"2026.47.0", "2026.46.3", 1},   // the week
        {"2026.46.3", "2026.47.0", -1},
        {"2027.1.0", "2026.52.9", 1},    // the year
        {"2025.52.9", "2026.1.0", -1},
        {"2026.46.1", "2026.46.0", 1},   // the patch
        {"2026.46.0", "2026.46.1", -1},
        {"2026.100.0", "2026.99.0", 1},  // a field with more digits
        {"2026.9.0", "2026.10.0", -1},
        {"2026.46.10", "2026.46.9", 1},
        {"2026.046.0", "2026.46.0", 0},  // leading zeros are the same number
        {"2026.46", "2026.46.0", 0},     // a missing field counts as 0
        {"2026.46.0.1", "2026.46.0", 1},
    };
    for (const auto& c : cases) CHECK_EQ(download::compareVersions(c.a, c.b), c.want);
    for (const char* bad : {"", "v2026.46.0", "2026..0", "2026.46.0.", ".2026", "2026.46.x", "2026.46.0.0.0",
                            "2026.1234567890.0", "2026.46.0 ", "-1.0"})
        CHECK_EQ(download::compareVersions(bad, "2026.46.0"), -2);
    CHECK_EQ(download::compareVersions(nullptr, "2026.46.0"), -2);
}

TEST(download_firmware_paths) {
    char m[download::kPathMax], i[download::kPathMax];
    download::firmwarePaths("firmware", m, i);
    CHECK_STR(m, "/hoverboardhavoc/havoc-radio/firmware/layout1/manifest.json");
    CHECK_STR(i, "/hoverboardhavoc/havoc-radio/firmware/layout1/havoc-radio.bin");
    download::firmwarePaths("bench-test", m, i);
    CHECK_STR(m, "/hoverboardhavoc/havoc-radio/bench-test/layout1/manifest.json");
}

namespace {

// The inactive app slot: the image as written, and what the SDK's image check answers.
struct FakeOta : download::Ota {
    uint32_t cap = 0x100000;
    std::string image, log;
    bool refuse = false, failWrite = false, failBegin = false;
    uint32_t begun = 0;
    uint32_t capacity() override { return cap; }
    Err begin(uint32_t length) override {
        log += "begin ";
        begun = length;
        return failBegin ? Err::Write : Err::Ok;
    }
    Err write(const void* d, uint32_t n) override {
        if (failWrite) return Err::Write;
        image.append(static_cast<const char*>(d), n);
        return Err::Ok;
    }
    Err end() override {
        log += "end ";
        return refuse ? Err::Image : Err::Ok;
    }
    Err activate() override {
        log += "activate ";
        return Err::Ok;
    }
    void abort() override { log += "abort "; }
};

struct FakePrompt : download::Prompt {
    bool answer = true;
    std::string offered;
    int asked = 0;
    bool confirm(const char* version) override {
        asked++;
        offered = version;
        return answer;
    }
};

// An image as the SDK starts one (magic 0xE9), with a recognisable body.
std::string appImage(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = static_cast<char>((i * 131 + 7) & 0xFF);
    s[0] = static_cast<char>(0xE9);
    return s;
}

std::string fwManifest(const char* version, const std::string& img, long length = -1, std::string digest = "") {
    return std::string("{\"version\": \"") + version + "\", \"length\": " +
           std::to_string(length < 0 ? static_cast<long>(img.size()) : length) + ", \"sha256\": \"" +
           (digest.empty() ? sha(img) : digest) + "\", \"released\": \"2026-11-16T09:12:00Z\"}\n";
}

struct FwRig {
    char manifestPath[download::kPathMax], imagePath[download::kPathMax];
    FakeHttp http;
    FakeOta ota;
    FakePrompt prompt;
    Steps steps;
    std::string img = appImage(988160);
    bool withPrompt = true;
    FwRig(const char* version = "2026.47.0") {
        download::firmwarePaths("firmware", manifestPath, imagePath);
        http.responses[manifestPath] = response(fwManifest(version, img));
        http.responses[imagePath] = response(img);
        http.chunk = 1460;
    }
    download::Report run(const char* running = "2026.46.0") {
        return download::fetchFirmware(http, ota, running, manifestPath, imagePath, withPrompt ? &prompt : nullptr, steps);
    }
};

}  // namespace

TEST(firmware_newer_installed_after_the_prompt) {
    FwRig r;
    const download::Report rep = r.run();
    CHECK(rep.result == Result::Updated && rep.step == Step::Check && rep.err == Err::Ok);
    CHECK_EQ(r.prompt.asked, 1);
    CHECK_STR(r.prompt.offered, "2026.47.0");
    CHECK_STR(r.steps.log, "manifest prompt image check ");
    CHECK_STR(r.ota.log, "begin end activate ");
    CHECK_EQ(r.ota.begun, r.img.size());
    CHECK(r.ota.image == r.img);
    CHECK_EQ(r.steps.last, r.img.size());
    CHECK_EQ(r.steps.total, r.img.size());
    CHECK_EQ(r.http.opened.size(), 2u);
    CHECK_STR(r.http.opened[0], "/hoverboardhavoc/havoc-radio/firmware/layout1/manifest.json");
    CHECK_STR(r.http.opened[1], "/hoverboardhavoc/havoc-radio/firmware/layout1/havoc-radio.bin");
}

TEST(firmware_prompt_cancelled) {
    FwRig r;
    r.prompt.answer = false;
    const download::Report rep = r.run();
    CHECK(rep.result == Result::Cancelled && rep.step == Step::Prompt);
    CHECK_EQ(r.http.opened.size(), 1u);  // the image is not fetched
    CHECK_STR(r.ota.log, "");            // nor the slot touched
}

TEST(firmware_no_prompt_from_the_api) {
    FwRig r;
    r.withPrompt = false;
    const download::Report rep = r.run();
    CHECK(rep.result == Result::Updated);
    CHECK_EQ(r.prompt.asked, 0);
    CHECK_STR(r.steps.log, "manifest image check ");
    CHECK_STR(r.ota.log, "begin end activate ");
}

TEST(firmware_up_to_date) {
    for (const char* running : {"2026.47.0", "2026.47.1", "2026.48.0", "2027.1.0"}) {
        FwRig r;  // the manifest offers 2026.47.0: the same or older than the running one
        const download::Report rep = r.run(running);
        CHECK(rep.result == Result::UpToDate && rep.step == Step::Manifest);
        CHECK_EQ(r.prompt.asked, 0);
        CHECK_EQ(r.http.opened.size(), 1u);
        CHECK_STR(r.ota.log, "");
    }
}

TEST(firmware_failures_keep_the_running_firmware) {
    {
        FwRig r;  // the image's length is not the manifest's
        r.http.responses[r.manifestPath] = response(fwManifest("2026.47.0", r.img, static_cast<long>(r.img.size()) - 1));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Image && rep.err == Err::Length);
        CHECK_STR(r.ota.log, "begin abort ");
    }
    {
        FwRig r;  // longer than the inactive slot: refused before the prompt
        r.ota.cap = static_cast<uint32_t>(r.img.size()) - 1;
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Manifest && rep.err == Err::TooLong);
        CHECK_EQ(r.prompt.asked, 0);
        CHECK_STR(r.ota.log, "");
    }
    {
        FwRig r;  // a hash mismatch: the image is not handed to the SDK's check, nor selected
        r.http.responses[r.manifestPath] = response(fwManifest("2026.47.0", r.img, -1, std::string(64, 'a')));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Check && rep.err == Err::Hash);
        CHECK_STR(r.ota.log, "begin abort ");
    }
    {
        FwRig r;  // an image the SDK refuses, with a manifest that matches it
        r.ota.refuse = true;
        const download::Report rep = r.run();
        CHECK(rep.result == Result::CheckFailed && rep.step == Step::Check && rep.err == Err::Image);
        CHECK_STR(r.ota.log, "begin end ");
    }
    {
        FwRig r;  // a body cut off before Content-Length
        r.http.responses[r.imagePath] = response(r.img.substr(0, 400000), static_cast<long>(r.img.size()));
        const download::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == Step::Image && rep.err == Err::Cut);
        CHECK_STR(r.ota.log, "begin abort ");
    }
    {
        FwRig r;  // the slot cannot be erased or written
        r.ota.failBegin = true;
        CHECK(r.run().err == Err::Write);
        CHECK_STR(r.ota.log, "begin ");
        FwRig w;
        w.ota.failWrite = true;
        CHECK(w.run().err == Err::Write);
        CHECK_STR(w.ota.log, "begin abort ");
    }
    {
        FwRig r;  // no image on the server, a manifest without a version or with a broken one, a 404, a zero length
        r.http.responses.erase(r.imagePath);
        CHECK(r.run().result == Result::Failed);
        CHECK_STR(r.ota.log, "begin abort ");
        r.http.responses[r.manifestPath] = response("{\"length\": 5, \"sha256\": \"" + std::string(64, '0') + "\"}");
        CHECK(r.run().err == Err::Format);
        r.http.responses[r.manifestPath] = response(fwManifest("2026.47.x", r.img));
        CHECK(r.run().err == Err::Format);
        r.http.responses[r.manifestPath] = response("404: Not Found", -2, "404 Not Found");
        CHECK(r.run().err == Err::Read);
        r.http.responses[r.manifestPath] = response(fwManifest("2026.47.0", "", 0));
        CHECK(r.run().err == Err::TooLong);
        r.http.responses[r.manifestPath] = response(fwManifest("2026.47.0", r.img) + std::string(2048, ' '));
        CHECK(r.run().result == Result::Failed);
        CHECK_EQ(r.prompt.asked, 1);  // only the first run reached the prompt
        CHECK(r.ota.log.find("activate") == std::string::npos);
    }
}

TEST(firmware_manifest_version) {
    download::Manifest m;
    const std::string img = appImage(100);
    const std::string j = fwManifest("2026.47.0", img);
    CHECK(download::parseManifest(j.data(), j.size(), m));
    CHECK_STR(m.version, "2026.47.0");
    CHECK_EQ(m.length, 100);
    CHECK(m.hasSha);
    const std::string longer = "{\"version\": \"" + std::string(40, '1') + "\"}";
    CHECK(download::parseManifest(longer.data(), longer.size(), m));
    CHECK_STR(m.version, "");  // too long to be a version: none
}
