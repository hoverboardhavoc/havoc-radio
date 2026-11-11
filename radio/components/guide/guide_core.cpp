#include "guide_core.h"

#include <cstring>

namespace guide {

namespace {

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }

// Valid UTF-8 (shortest forms, no surrogates, at most U+10FFFF) without control characters.
bool textOk(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = s[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) {
            i++;
            continue;
        }
        int len;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F, min = 0x80;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F, min = 0x800;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07, min = 0x10000;
        else return false;
        if (i + len > n) return false;
        for (int k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// Reads the string at `off` (inside the table of `length` bytes) into out; its length, or -1 when it is not
// terminated within kStrMax bytes or not valid text.
int stringAt(const Source& src, uint32_t length, uint32_t off, char out[kStrMax + 1]) {
    const uint32_t n = length - off < static_cast<uint32_t>(kStrMax + 1) ? length - off : kStrMax + 1;
    if (!src.read(off, out, n)) return -1;
    const void* z = std::memchr(out, 0, n);
    if (!z) return -1;
    const int len = static_cast<int>(static_cast<const char*>(z) - out);
    return textOk(reinterpret_cast<const uint8_t*>(out), len) ? len : -1;
}

void decode(const uint8_t* p, Entry& e) {
    e.khz = u16(p);
    e.tenths = p[2];
    e.days = p[3];
    e.start = u16(p + 4);
    e.end = u16(p + 6);
    e.name = u16(p + 8);
    e.lang = u16(p + 10);
    e.target = u16(p + 12);
}

constexpr int64_t kDayMs = 86400000;

}  // namespace

const char* errText(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::Read: return "read failed";
        case Err::Short: return "shorter than the header";
        case Err::Magic: return "not a guide table";
        case Err::Format: return "another format";
        case Err::HeaderLen: return "header length";
        case Err::Length: return "length";
        case Err::Crc: return "CRC does not match";
        case Err::Layout: return "entries or strings out of place";
        case Err::StringTable: return "string table out of range";
        case Err::String: return "string not valid";
        case Err::Entry: return "entry out of range";
        case Err::Index: return "string index out of range";
        case Err::Order: return "entries not sorted";
        case Err::TooLong: return "longer than a slot";
        case Err::Cut: return "cut off";
        case Err::Hash: return "SHA-256 does not match";
        case Err::Write: return "flash write failed";
        case Err::Image: return "not a valid app image";
    }
    return "?";
}

bool MemSource::read(uint32_t off, void* buf, uint32_t len) const {
    if (off > size_ || len > size_ - off) return false;
    std::memcpy(buf, p_ + off, len);
    return true;
}

// ---- SHA-256 (FIPS 180-4) and CRC-32 ----

Sha256::Sha256() {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, h0, sizeof h_);
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
               static_cast<uint32_t>(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    while (n) {
        const size_t k = 64 - fill_ < n ? 64 - fill_ : n;
        std::memcpy(buf_ + fill_, p, k);
        fill_ += k, p += k, n -= k;
        if (fill_ == 64) {
            block(buf_);
            fill_ = 0;
        }
    }
}

void Sha256::finish(uint8_t out[32]) {
    const uint64_t bits = len_ * 8;
    const uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (fill_ != 56) update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(lenBytes, 8);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = static_cast<uint8_t>(h_[i] >> (24 - 8 * k));
}

uint32_t crc32(uint32_t crc, const void* data, size_t n) {
    static const uint32_t t[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                   0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                   0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = crc >> 4 ^ t[crc & 15];
        crc = crc >> 4 ^ t[crc & 15];
    }
    return ~crc;
}

// ---- checks ----

Err check(const Source& src, uint32_t size, Info& out) {
    out = Info();
    uint8_t buf[256];
    if (size < kHeaderLen) return Err::Short;
    if (!src.read(0, buf, kHeaderLen)) return Err::Read;
    if (std::memcmp(buf, "HVGD", 4)) return Err::Magic;
    if (u16(buf + 4) != kFormat) return Err::Format;
    if (u16(buf + 6) != kHeaderLen) return Err::HeaderLen;
    const uint32_t count = u32(buf + 8), entOff = u32(buf + 12), strOff = u32(buf + 16), length = u32(buf + 20);
    const uint32_t modified = u32(buf + 28), crc = u32(buf + 32);
    if (length < kHeaderLen || length > size || length > kSlotSize) return Err::Length;
    char season[5] = {};
    std::memcpy(season, buf + 24, 4);
    // The CRC over everything after the header, and the SHA-256 of the whole table, in one pass.
    Sha256 sha;
    sha.update(buf, kHeaderLen);
    uint32_t c = 0;
    for (uint32_t off = kHeaderLen; off < length;) {
        const uint32_t n = length - off < sizeof buf ? length - off : sizeof buf;
        if (!src.read(off, buf, n)) return Err::Read;
        c = crc32(c, buf, n);
        sha.update(buf, n);
        off += n;
    }
    if (c != crc) return Err::Crc;
    if (entOff != kHeaderLen || count > (length - kHeaderLen) / kEntryLen || strOff != kHeaderLen + count * kEntryLen)
        return Err::Layout;
    if (length - strOff < 4) return Err::StringTable;
    if (!src.read(strOff, buf, 4)) return Err::Read;
    const uint32_t nstr = u32(buf);
    if (nstr > (length - strOff - 4) / 4) return Err::StringTable;
    const uint32_t first = strOff + 4 + 4 * nstr;
    // Every string: inside the table after the offsets, terminated within kStrMax bytes, valid text.
    char s[kStrMax + 1];
    for (uint32_t i = 0; i < nstr; i++) {
        if (!src.read(strOff + 4 + 4 * i, buf, 4)) return Err::Read;
        const uint32_t off = u32(buf);
        if (off < first || off >= length) return Err::StringTable;
        if (stringAt(src, length, off, s) < 0) return Err::String;
    }
    // Every entry: in range, string indexes in range, sorted by frequency and start time.
    uint32_t prevF = 0, prevS = 0;
    constexpr uint32_t kBatch = sizeof buf / kEntryLen;
    for (uint32_t i = 0; i < count; i += kBatch) {
        const uint32_t k = count - i < kBatch ? count - i : kBatch;
        if (!src.read(kHeaderLen + i * kEntryLen, buf, k * kEntryLen)) return Err::Read;
        for (uint32_t j = 0; j < k; j++) {
            Entry e;
            decode(buf + j * kEntryLen, e);
            if (e.tenths > 9 || e.start > 1439 || e.end < 1 || e.end > 1440) return Err::Entry;
            if (e.name >= nstr || e.lang >= nstr || e.target >= nstr) return Err::Index;
            const uint32_t f = e.khz * 10u + e.tenths;
            if (i + j && (f < prevF || (f == prevF && e.start < prevS))) return Err::Order;
            prevF = f, prevS = e.start;
        }
    }
    out.valid = true;
    std::memcpy(out.season, season, sizeof season);
    for (char& ch : out.season)
        if (ch && (ch < 0x20 || ch > 0x7E)) ch = '?';
    out.entries = count;
    out.length = length;
    out.modified = modified;
    out.strings = nstr;
    out.stringsOff = strOff;
    sha.finish(out.sha256);
    return Err::Ok;
}

bool readEntry(const Source& src, uint32_t i, Entry& out) {
    uint8_t b[kEntryLen];
    if (!src.read(kHeaderLen + i * kEntryLen, b, kEntryLen)) return false;
    decode(b, out);
    return true;
}

bool readString(const Source& src, const Info& info, uint16_t idx, char out[kStrMax + 1]) {
    out[0] = 0;
    uint8_t b[4];
    if (!info.valid || idx >= info.strings || !src.read(info.stringsOff + 4 + 4u * idx, b, 4)) return false;
    const uint32_t off = u32(b);
    if (off >= info.length || stringAt(src, info.length, off, out) < 0) {
        out[0] = 0;
        return false;
    }
    return true;
}

// ---- days and the on-air rule ----

bool onAir(const Entry& e, int64_t utcMs) {
    if (utcMs < 0 || (e.days & kIrregular)) return false;
    const int64_t day = utcMs / kDayMs;
    const int minute = static_cast<int>(utcMs % kDayMs / 60000);
    int dow = static_cast<int>((day + 3) % 7);  // 1970-01-01 was a Thursday; 0 is Monday
    bool in;
    if (e.start < e.end) {
        in = e.start <= minute && minute < e.end;
    } else if (e.start > e.end) {
        in = minute >= e.start || minute < e.end;
        if (minute < e.end) dow = (dow + 6) % 7;  // the window started the day before
    } else {
        in = false;
    }
    return in && (e.days >> dow & 1);
}

void daysText(uint8_t days, char out[24]) {
    static const char* const kNames[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    out[0] = 0;
    days &= kEveryDay;
    if (!days) return;
    // From Monday, unless a run of three or more days crosses from Sunday into Monday (`Fr-Mo`): then from its first
    // day, so it stays whole.
    int s = 0;
    if ((days & 0x41) == 0x41 && days != kEveryDay) {
        int back = 0, fwd = 0;
        while (days >> (6 - back) & 1) back++;
        while (days >> fwd & 1) fwd++;
        if (back + fwd >= 3) s = 7 - back;
    }
    size_t n = 0;
    for (int i = 0; i < 7;) {
        const int d = (s + i) % 7;
        if (!(days >> d & 1)) {
            i++;
            continue;
        }
        int len = 1;
        while (i + len < 7 && days >> ((s + i + len) % 7) & 1) len++;
        const int last = (d + len - 1) % 7;
        if (n) out[n++] = ',';
        std::memcpy(out + n, kNames[d], 2), n += 2;
        if (len >= 3) out[n++] = '-';
        if (len == 2) out[n++] = ',';
        if (len >= 2) std::memcpy(out + n, kNames[last], 2), n += 2;
        i += len;
    }
    out[n] = 0;
}

// ---- lookup ----

void lookup(const Source& src, const Info& info, int32_t hz, int64_t utcMs, Hits& out) {
    out.n = 0;
    out.used = 0;
    if (!info.valid || !info.entries) return;
    // The first entry within 1 kHz below, by binary search on the frequency.
    uint32_t lo = 0, hi = info.entries;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        Entry e;
        if (!readEntry(src, mid, e)) return;
        if (e.hz() < hz - kNearHz)
            lo = mid + 1;
        else
            hi = mid;
    }
    const int nowMin = utcMs >= 0 ? static_cast<int>(utcMs % kDayMs / 60000) : 0;
    // Candidates ordered by: on air first (table order), then start time from now, then table order.
    struct Cand {
        Entry e;
        bool on;
        uint32_t key, idx;
    };
    Cand best[kMaxHits];
    int n = 0;
    for (uint32_t i = lo; i < info.entries; i++) {
        Entry e;
        if (!readEntry(src, i, e)) break;
        if (e.hz() > hz + kNearHz) break;
        const bool on = onAir(e, utcMs);
        const uint32_t key = on ? 0 : 1 + static_cast<uint32_t>((e.start - nowMin + 1440) % 1440);
        int at = n;
        while (at > 0 && (best[at - 1].key > key || (best[at - 1].key == key && best[at - 1].idx > i))) at--;
        if (at >= kMaxHits) continue;
        const int last = n < kMaxHits ? n : kMaxHits - 1;
        for (int k = last; k > at; k--) best[k] = best[k - 1];
        best[at] = {e, on, key, i};
        if (n < kMaxHits) n++;
    }
    // The strings, each read once into the arena; an entry whose strings do not fit ends the list.
    auto text = [&](uint16_t idx) -> const char* {
        for (int k = 0; k < out.n; k++) {
            const Hit& h = out.hit[k];
            if (h.e.name == idx) return h.name;
            if (h.e.lang == idx) return h.lang;
            if (h.e.target == idx) return h.target;
        }
        char s[kStrMax + 1];
        readString(src, info, idx, s);
        const size_t len = std::strlen(s) + 1;
        if (out.used + len > Hits::kArena) return nullptr;
        char* p = out.arena + out.used;
        std::memcpy(p, s, len);
        out.used += len;
        return p;
    };
    for (int k = 0; k < n; k++) {
        Hit& h = out.hit[out.n];
        h.e = best[k].e;
        h.onAir = best[k].on;
        h.name = h.lang = h.target = nullptr;
        // Strings already read for this entry's own fields are matched against earlier hits only.
        const char* name = text(h.e.name);
        if (!name) break;
        h.name = name;
        const char* lang = h.e.lang == h.e.name ? name : text(h.e.lang);
        if (!lang) break;
        h.lang = lang;
        const char* target = h.e.target == h.e.name ? name : h.e.target == h.e.lang ? lang : text(h.e.target);
        if (!target) break;
        h.target = target;
        out.n++;
    }
}

}  // namespace guide
