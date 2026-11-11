#include "guide_fetch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace guide {

namespace {

bool ieq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// A header's value when `line` is `name: value`, else nullptr.
const char* header(const char* line, const char* name) {
    const size_t n = std::strlen(name);
    if (std::strlen(line) <= n || !ieq(line, name, n) || line[n] != ':') return nullptr;
    const char* v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A minimal reader for one flat JSON object.
struct Json {
    const char* p;
    const char* end;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    }
    bool eat(char c) {
        ws();
        if (p < end && *p == c) {
            p++;
            return true;
        }
        return false;
    }
    // A string into out (escapes other than \uXXXX kept as their character; \uXXXX as '?').
    bool str(char* out, size_t n) {
        if (!eat('"')) return false;
        size_t k = 0;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= end) return false;
                c = *p++;
                if (c == 'u') {
                    if (end - p < 4) return false;
                    p += 4;
                    c = '?';
                } else if (c == 'n') {
                    c = '\n';
                } else if (c == 't') {
                    c = '\t';
                }
            }
            if (k + 1 < n) out[k++] = c;
        }
        if (n) out[k] = 0;
        return eat('"');
    }
    // Any value; numbers into *num when given.
    bool value(char* s, size_t n, bool* isStr, double* num, bool* isNum) {
        ws();
        *isStr = *isNum = false;
        if (p >= end) return false;
        if (*p == '"') {
            *isStr = true;
            return str(s, n);
        }
        if (*p == '{' || *p == '[') {
            int depth = 0;
            bool inStr = false;
            for (; p < end; p++) {
                if (inStr) {
                    if (*p == '\\') p++;
                    else if (*p == '"') inStr = false;
                } else if (*p == '"') {
                    inStr = true;
                } else if (*p == '{' || *p == '[') {
                    depth++;
                } else if ((*p == '}' || *p == ']') && --depth == 0) {
                    p++;
                    return true;
                }
            }
            return false;
        }
        const char* start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
        if (p == start || p - start > 31) return false;
        char tok[32];
        std::memcpy(tok, start, p - start);
        tok[p - start] = 0;
        if (!std::strcmp(tok, "true") || !std::strcmp(tok, "false") || !std::strcmp(tok, "null")) return true;
        char* e;
        *num = std::strtod(tok, &e);
        *isNum = *e == 0;
        return *isNum;
    }
};

}  // namespace

const char* stepName(Step s) {
    switch (s) {
        case Step::Join: return "join";
        case Step::Manifest: return "manifest";
        case Step::Table: return "table";
        case Step::Check: return "check";
    }
    return "?";
}

int Response::line(char* out, int n) {
    int k = 0;
    for (;;) {
        if (pos_ == fill_) {
            fill_ = h_.read(buf_, sizeof buf_);
            pos_ = 0;
            if (fill_ <= 0) {
                fill_ = 0;
                return -1;
            }
        }
        const char c = static_cast<char>(buf_[pos_++]);
        if (c == '\n') break;
        if (c != '\r' && k + 1 < n) out[k++] = c;  // a longer line is cut; no header of interest is that long
    }
    out[k] = 0;
    return k;
}

bool Response::begin() {
    char l[128];
    if (line(l, sizeof l) < 0) return false;
    // "HTTP/1.1 200 OK"
    const char* sp = std::strchr(l, ' ');
    if (std::strncmp(l, "HTTP/1.", 7) || !sp) return false;
    status_ = std::atoi(sp + 1);
    bool chunked = false;
    for (int lines = 0;; lines++) {
        const int n = line(l, sizeof l);
        if (n < 0 || lines > 100) return false;
        if (n == 0) break;
        if (const char* v = header(l, "content-length")) {
            char* e;
            const long long len = std::strtoll(v, &e, 10);
            if (e == v || len < 0) return false;
            length_ = len;
        }
        if (const char* v = header(l, "transfer-encoding")) chunked = std::strstr(v, "chunked") != nullptr;
    }
    left_ = length_;
    return status_ == 200 && length_ >= 0 && !chunked;
}

int Response::body(uint8_t* out, int n) {
    if (left_ <= 0) return 0;
    if (n > left_) n = static_cast<int>(left_);
    int got;
    if (pos_ < fill_) {
        got = fill_ - pos_ < n ? fill_ - pos_ : n;
        std::memcpy(out, buf_ + pos_, got);
        pos_ += got;
    } else {
        got = h_.read(out, n);
        if (got <= 0) return -1;  // cut off before Content-Length
    }
    left_ -= got;
    return got;
}

bool parseManifest(const char* json, size_t n, Manifest& out) {
    out = Manifest();
    Json j{json, json + n};
    if (!j.eat('{')) return false;
    if (j.eat('}')) return true;
    for (;;) {
        char key[24], s[72];
        bool isStr, isNum;
        double num = 0;
        if (!j.str(key, sizeof key) || !j.eat(':') || !j.value(s, sizeof s, &isStr, &num, &isNum)) return false;
        if (!std::strcmp(key, "format") && isNum) out.format = static_cast<int>(num);
        if (!std::strcmp(key, "length") && isNum && num >= 0 && num < 4294967296.0) out.length = static_cast<int64_t>(num);
        if (!std::strcmp(key, "sha256") && isStr && std::strlen(s) == 64) {
            out.hasSha = true;
            for (int i = 0; i < 32; i++) {
                const int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
                if (hi < 0 || lo < 0) out.hasSha = false;
                out.sha256[i] = static_cast<uint8_t>((hi << 4) | (lo & 15));
            }
        }
        if (j.eat('}')) break;
        if (!j.eat(',')) return false;
    }
    j.ws();
    return j.p == j.end;
}

Report fetch(Http& http, Sink& sink, const Info& loaded, Progress& progress) {
    Report r;
    auto fail = [&](Step s, Err e, Result res = Result::Failed) {
        http.close();
        r.step = s;
        r.err = e;
        r.result = res;
        return r;
    };
    // The manifest: at most 2 KB, format 1.
    progress.step(Step::Manifest);
    if (!http.open(kManifestPath)) return fail(Step::Manifest, Err::Read);
    Manifest m;
    {
        Response resp(http);
        if (!resp.begin() || resp.length() > static_cast<int64_t>(kManifestMax)) return fail(Step::Manifest, Err::Read);
        char json[kManifestMax];
        int got = 0;
        for (;;) {
            const int n = resp.body(reinterpret_cast<uint8_t*>(json) + got, static_cast<int>(sizeof json) - got);
            if (n < 0) return fail(Step::Manifest, Err::Cut);
            if (n == 0) break;
            got += n;
        }
        http.close();
        if (!parseManifest(json, got, m)) return fail(Step::Manifest, Err::Magic);
        if (m.format != kFormat) return fail(Step::Manifest, Err::Format);
        if (!m.hasSha || m.length < 0) return fail(Step::Manifest, Err::Magic);
    }
    if (loaded.valid && !std::memcmp(loaded.sha256, m.sha256, sizeof m.sha256)) {
        r.result = Result::UpToDate;
        return r;
    }
    if (m.length > kSlotSize || m.length < kHeaderLen) return fail(Step::Manifest, Err::TooLong);
    // The table: the manifest's length, streamed into the free slot.
    const uint32_t total = static_cast<uint32_t>(m.length);
    progress.step(Step::Table);
    progress.bytes(0, total);
    if (!http.open(kTablePath)) return fail(Step::Table, Err::Read);
    Response resp(http);
    if (!resp.begin()) return fail(Step::Table, Err::Read);
    if (resp.length() != m.length) return fail(Step::Table, Err::Length);
    Err e = sink.begin(total);
    if (e != Err::Ok) return fail(Step::Table, e);
    uint8_t buf[512];
    uint32_t done = 0;
    for (;;) {
        const int n = resp.body(buf, sizeof buf);
        if (n < 0) return fail(Step::Table, Err::Cut);
        if (n == 0) break;
        e = sink.add(buf, n);
        if (e != Err::Ok) return fail(Step::Table, e);
        done += n;
        progress.bytes(done, total);
    }
    http.close();
    // The SHA-256 and every table check; then the slots switch.
    progress.step(Step::Check);
    e = sink.finish(m.sha256);
    if (e == Err::Hash || e == Err::Cut || e == Err::Write) return fail(Step::Check, e);
    if (e != Err::Ok) return fail(Step::Check, e, Result::CheckFailed);
    r.step = Step::Check;
    r.result = Result::Updated;
    return r;
}

int requestUpdate(bool netSaved, RequestFlag& f, char* body, size_t n) {
    if (!netSaved) {
        std::snprintf(body, n, "{\"error\":\"no network\"}");
        return 409;
    }
    f.set();
    std::snprintf(body, n, "{\"update\":\"requested\"}");
    return 202;
}

const char* resultName(Result r) {
    switch (r) {
        case Result::Updated: return "updated";
        case Result::UpToDate: return "up to date";
        case Result::NoNetwork: return "no network";
        case Result::Failed: return "download failed";
        case Result::CheckFailed: return "check failed";
        case Result::Cancelled: return "cancelled";
        case Result::None: break;
    }
    return nullptr;
}

void lastUpdateJson(const LastUpdate& u, char* out, size_t n) {
    const char* name = resultName(u.result);
    if (!name) {
        if (n) out[0] = 0;
        return;
    }
    std::snprintf(out, n, "\"last_update\":{\"result\":\"%s\",\"step\":\"%s\",\"lowest_heap\":%lu}", name,
                  stepName(u.step), static_cast<unsigned long>(u.lowestHeap));
}

bool takeRequest(RequestFlag& f) {
    if (!f.get()) return false;
    f.clear();
    return true;
}

}  // namespace guide
