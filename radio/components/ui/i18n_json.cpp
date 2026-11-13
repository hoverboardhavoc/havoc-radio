#include "i18n_json.h"

#include "i18n_strings.h"

namespace i18n {

namespace {

struct Out {
    bool (*send)(void*, const char*, size_t);
    void* ctx;
    char buf[kJsonChunk];
    size_t n;
    bool ok;

    void flush() {
        if (ok && n) ok = send(ctx, buf, n);
        n = 0;
    }
    void put(const char* s, size_t len) {
        for (size_t i = 0; i < len; i++) {
            if (n == sizeof buf) flush();
            buf[n++] = s[i];
        }
    }
    void put(char c) { put(&c, 1); }
    void str(const char* s) {
        put('"');
        char e[6];
        for (; *s; s++) put(e, jsonEscape(static_cast<unsigned char>(*s), e));
        put('"');
    }
};

}  // namespace

size_t jsonEscape(unsigned char b, char* out) {
    const char* hex = "0123456789abcdef";
    const char* const shortForm = b == '"' ? "\\\"" : b == '\\' ? "\\\\" : b == '\b' ? "\\b" : b == '\f' ? "\\f"
                                : b == '\n' ? "\\n" : b == '\r' ? "\\r" : b == '\t' ? "\\t" : nullptr;
    if (shortForm) {
        out[0] = shortForm[0];
        out[1] = shortForm[1];
        return 2;
    }
    if (b < 0x20) {
        const char u[6] = {'\\', 'u', '0', '0', hex[b >> 4], hex[b & 15]};
        for (int i = 0; i < 6; i++) out[i] = u[i];
        return 6;
    }
    out[0] = static_cast<char>(b);
    return 1;
}

bool writeJson(int lang, bool (*send)(void* ctx, const char* data, size_t len), void* ctx) {
    Out o{send, ctx, {}, 0, true};
    o.put('{');
    for (int k = 0; k < kStrCount; k++) {
        if (k) o.put(',');
        o.str(kKey[k]);
        o.put(':');
        o.str(tr(lang, static_cast<Str>(k)));
    }
    o.put('}');
    o.flush();
    return o.ok;
}

}  // namespace i18n
