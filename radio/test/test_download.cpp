// The download mode's core with a fake transport: up to date, an update, a manifest of another format, a length
// that does not match or exceeds a slot, a hash mismatch, a table failing its checks, a cut-off body, the HTTP
// response head, the manifest parser, and the request cleared before the download starts.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "guide_fakes.h"
#include "guide_fetch.h"
#include "util.h"

using guide::Err;
using guide::Result;

namespace {

std::string table(const char* name) { return readFile(t::root() + "/radio/build/host/guide/" + name); }

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
struct FakeHttp : guide::Http {
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

struct Steps : guide::Progress {
    std::string log;
    uint32_t last = 0, total = 0;
    int updates = 0;
    void step(guide::Step s) override { log += std::string(guide::stepName(s)) + " "; }
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
        http.responses[guide::kManifestPath] = response(manifest(fresh));
        http.responses[guide::kTablePath] = response(fresh);
    }
    guide::Report run() {
        SlotSink sink(slots);
        return guide::fetch(http, sink, slots.info(), steps);
    }
    bool oldInUse() { return slots.active() == 0 && slots.info().entries == 28; }
};

}  // namespace

TEST(download_update) {
    Rig r;
    const guide::Report rep = r.run();
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
    const guide::Report rep = r.run();
    CHECK(rep.result == Result::UpToDate);
    CHECK_STR(r.steps.log, "manifest ");
    CHECK_EQ(r.http.opened.size(), 3u);  // the table is not fetched again
    CHECK_EQ(r.slots.active(), 1);
}

TEST(download_failures_leave_the_old_table) {
    {
        Rig r;  // a manifest of another format
        r.http.responses[guide::kManifestPath] = response(manifest(r.fresh, 2));
        const guide::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == guide::Step::Manifest && rep.err == Err::Format);
        CHECK_EQ(r.http.opened.size(), 1u);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // the table's length is not the manifest's
        r.http.responses[guide::kManifestPath] = response(manifest(r.fresh, 1, static_cast<long>(r.fresh.size()) + 1));
        const guide::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == guide::Step::Table && rep.err == Err::Length);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a manifest length above a slot
        r.http.responses[guide::kManifestPath] = response(manifest(r.fresh, 1, guide::kSlotSize + 1));
        const guide::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.err == Err::TooLong);
        CHECK_EQ(r.http.opened.size(), 1u);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a hash mismatch
        r.http.responses[guide::kManifestPath] = response(manifest(r.fresh, 1, -1, std::string(64, '0')));
        const guide::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == guide::Step::Check && rep.err == Err::Hash);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a table failing its checks, with a manifest that matches it
        std::string bad = r.fresh;
        bad[200] ^= 1;
        r.http.responses[guide::kManifestPath] = response(manifest(bad));
        r.http.responses[guide::kTablePath] = response(bad);
        const guide::Report rep = r.run();
        CHECK(rep.result == Result::CheckFailed && rep.err == Err::Crc);
        CHECK(r.oldInUse());
    }
    {
        Rig r;  // a body cut off before Content-Length
        r.http.responses[guide::kTablePath] = response(r.fresh.substr(0, 90000), static_cast<long>(r.fresh.size()));
        const guide::Report rep = r.run();
        CHECK(rep.result == Result::Failed && rep.step == guide::Step::Table && rep.err == Err::Cut);
        CHECK(r.oldInUse());
        guide::Slots again(r.flash);
        again.boot();
        CHECK_EQ(again.active(), 0);
    }
    {
        Rig r;  // a manifest over 2 KB, a 404, no connection, chunked encoding, no Content-Length
        r.http.responses[guide::kManifestPath] = response(manifest(r.fresh) + std::string(2048, ' '));
        CHECK(r.run().result == Result::Failed);
        r.http.responses[guide::kManifestPath] = response("404: Not Found", -2, "404 Not Found");
        CHECK(r.run().result == Result::Failed);
        r.http.responses.erase(guide::kManifestPath);
        CHECK(r.run().result == Result::Failed);
        r.http.responses[guide::kManifestPath] = response(manifest(r.fresh));
        r.http.responses[guide::kTablePath] = response(r.fresh, -1, "200 OK", "Transfer-Encoding: chunked\r\n");
        CHECK(r.run().result == Result::Failed);
        r.http.responses[guide::kTablePath] = response(r.fresh, -1);
        CHECK(r.run().result == Result::Failed);
        r.http.responses[guide::kManifestPath] = response("{\"format\": 1, \"length\": 5");
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
    http.responses[guide::kManifestPath] = response(manifest(t));
    http.responses[guide::kTablePath] = response(t);
    http.chunk = 1;  // a byte at a time through the head and body
    SlotSink sink(slots);
    Steps steps;
    CHECK(guide::fetch(http, sink, slots.info(), steps).result == Result::Updated);
    CHECK_EQ(slots.active(), 0);
}

TEST(download_response_head) {
    FakeHttp http;
    http.responses["/a"] = "HTTP/1.1 200 OK\r\ncontent-length:  5\r\n\r\nhelloEXTRA";
    http.open("/a");
    guide::Response a(http);
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
        guide::Response r(http);
        CHECK(!r.begin());
    }
}

TEST(download_manifest_parser) {
    guide::Manifest m;
    const std::string t = table("b26.bin");
    const std::string conv = table("b26.bin.json");  // as the converter writes it
    CHECK(guide::parseManifest(conv.data(), conv.size(), m));
    CHECK_EQ(m.format, 1);
    CHECK_EQ(m.length, static_cast<int64_t>(t.size()));
    CHECK(m.hasSha);
    CHECK_STR(hex(m.sha256, 32), sha(t));
    const char* ok = "{\"x\": {\"a\": [1, \"}\"]}, \"format\": 1, \"note\": \"a \\\"q\\\" \\u00e9\", \"length\": 10, "
                     "\"sha256\": \"" "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF" "\"}";
    CHECK(guide::parseManifest(ok, std::strlen(ok), m));
    CHECK(m.format == 1 && m.length == 10 && m.hasSha && m.sha256[31] == 0xFF);
    for (const char* bad : {"", "[]", "{", "{\"format\" 1}", "{\"format\": 1,}", "{\"format\": 1} x", "{\"a\": tru}"})
        CHECK(!guide::parseManifest(bad, std::strlen(bad), m));
    const char* badSha = "{\"sha256\": \"zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}";
    CHECK(guide::parseManifest(badSha, std::strlen(badSha), m) && !m.hasSha);
}

TEST(download_request_cleared_first) {
    struct Flag : guide::RequestFlag {
        bool on = true;
        std::string log;
        bool get() override {
            log += "get ";
            return on;
        }
        void set() override { on = true; }
        void clear() override {
            log += "clear ";
            on = false;
        }
    } f;
    CHECK(guide::takeRequest(f));
    CHECK_STR(f.log, "get clear ");
    CHECK(!f.on);
    CHECK(!guide::takeRequest(f));  // after a crash and restart, no second download
    CHECK_STR(f.log, "get clear get ");
}

TEST(download_update_request_from_the_web) {
    struct Flag : guide::RequestFlag {
        bool on = false;
        bool get() override { return on; }
        void set() override { on = true; }
        void clear() override { on = false; }
    } f;
    char body[64];
    CHECK_EQ(guide::requestUpdate(false, f, body, sizeof body), 409);
    CHECK_STR(body, "{\"error\":\"no network\"}");
    CHECK(!f.on);  // nothing stored, nothing restarts
    CHECK_EQ(guide::requestUpdate(true, f, body, sizeof body), 202);
    CHECK(f.on);
    CHECK(guide::takeRequest(f) && !f.on);
}

TEST(download_last_update_json) {
    char out[128];
    guide::LastUpdate u;
    guide::lastUpdateJson(u, out, sizeof out);
    CHECK_STR(out, "");  // before a run
    const struct {
        Result r;
        guide::Step step;
        const char* json;
    } cases[] = {
        {Result::Updated, guide::Step::Check, "{\"result\":\"updated\",\"step\":\"check\",\"lowest_heap\":9900}"},
        {Result::UpToDate, guide::Step::Manifest, "{\"result\":\"up to date\",\"step\":\"manifest\",\"lowest_heap\":9900}"},
        {Result::NoNetwork, guide::Step::Join, "{\"result\":\"no network\",\"step\":\"join\",\"lowest_heap\":9900}"},
        {Result::Failed, guide::Step::Table, "{\"result\":\"download failed\",\"step\":\"table\",\"lowest_heap\":9900}"},
        {Result::CheckFailed, guide::Step::Check, "{\"result\":\"check failed\",\"step\":\"check\",\"lowest_heap\":9900}"},
        {Result::Cancelled, guide::Step::Table, "{\"result\":\"cancelled\",\"step\":\"table\",\"lowest_heap\":9900}"},
    };
    for (const auto& c : cases) {
        u = {c.r, c.step, 9900};
        guide::lastUpdateJson(u, out, sizeof out);
        CHECK_STR(out, std::string("\"last_update\":") + c.json);
    }
}

