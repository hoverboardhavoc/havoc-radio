// The guide reader on tables made by the converter (radio/test/test_guide_table.py writes them to
// radio/build/host/guide/): the checks, damaged and hostile tables, lookup within 1 kHz, the on-air rule at window
// edges and across midnight with the start day, irregular entries, the ordering, and the two slots.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "guide_core.h"
#include "guide_slots.h"
#include "util.h"

using guide::Err;

namespace {

constexpr int64_t kTue = 1794268800000LL;  // 2026-11-10 00:00 UTC, a Tuesday
constexpr int64_t kMin = 60000, kHour = 3600000, kDay = 86400000;

std::string table(const char* name) { return readFile(t::root() + "/radio/build/host/guide/" + name); }

uint32_t u32(const std::string& t, size_t off) {
    uint32_t v;
    std::memcpy(&v, t.data() + off, 4);
    return v;
}
void put16(std::string& t, size_t off, uint16_t v) { std::memcpy(&t[off], &v, 2); }
void put32(std::string& t, size_t off, uint32_t v) { std::memcpy(&t[off], &v, 4); }
std::string recrc(std::string t) {
    put32(t, 32, guide::crc32(0, t.data() + 48, t.size() - 48));
    return t;
}

Err checkOf(const std::string& t, guide::Info* info = nullptr) {
    guide::Info i;
    const Err e = guide::check(guide::MemSource(t.data(), t.size()), t.size(), i);
    if (info) *info = i;
    return e;
}

std::string hex(const uint8_t* p, int n) {
    std::string s;
    char b[3];
    for (int i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

struct Look {
    std::string t;
    guide::Info info;
    guide::Hits hits;
    explicit Look(const char* name) : t(table(name)) { CHECK(checkOf(t, &info) == Err::Ok); }
    // "name*" for an entry on air, joined with "|".
    std::string at(int32_t khz10, int64_t utc) {
        guide::lookup(guide::MemSource(t.data(), t.size()), info, khz10 * 100, utc, hits);
        std::string s;
        for (int i = 0; i < hits.n; i++) s += (i ? "|" : "") + std::string(hits.hit[i].name) + (hits.hit[i].onAir ? "*" : "");
        return s;
    }
};

// NOR flash: erase sets a sector to 0xFF, a write can only clear bits.
struct FakeFlash : guide::Flash {
    std::vector<uint8_t> m = std::vector<uint8_t>(guide::kPartitionSize, 0xFF);
    int erases = 0, writes = 0;
    bool failWrites = false;
    bool read(uint32_t off, void* buf, uint32_t len) override {
        if (off > m.size() || len > m.size() - off) return false;
        std::memcpy(buf, m.data() + off, len);
        return true;
    }
    bool erase(uint32_t off) override {
        if (off % guide::kSector || off >= m.size()) return false;
        std::memset(m.data() + off, 0xFF, guide::kSector);
        erases++;
        return true;
    }
    bool write(uint32_t off, const void* buf, uint32_t len) override {
        if (failWrites || off > m.size() || len > m.size() - off) return false;
        for (uint32_t i = 0; i < len; i++) m[off + i] &= static_cast<const uint8_t*>(buf)[i];
        writes++;
        return true;
    }
    void place(int slot, const std::string& t) {
        std::memcpy(m.data() + slot * guide::kSlotSize, t.data(), t.size());
    }
};

Err upload(guide::Slots& s, const std::string& t, const uint8_t* sha = nullptr, uint32_t send = UINT32_MAX,
           bool commit = true) {
    static uint8_t sector[guide::kSector];
    guide::Slots::Writer w(s, sector);
    Err e = w.begin(t.size());
    if (e != Err::Ok) return e;
    const uint32_t n = send < t.size() ? send : t.size();
    for (uint32_t off = 0; off < n; off += 1000) {
        e = w.add(t.data() + off, n - off < 1000 ? n - off : 1000);
        if (e != Err::Ok) return e;
    }
    e = w.finish(sha);
    if (e == Err::Ok && commit) CHECK(s.commit(w));
    return e;
}

}  // namespace

TEST(guide_sha256_and_crc32) {
    guide::Sha256 h;
    h.update("abc", 3);
    uint8_t d[32];
    h.finish(d);
    CHECK_STR(hex(d, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    guide::Sha256 e;
    const std::string m(1000, 'a');
    for (int i = 0; i < 1000; i++) e.update(m.data(), m.size());
    e.finish(d);
    CHECK_STR(hex(d, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");  // a million 'a'
    CHECK_EQ(guide::crc32(0, "123456789", 9), 0xCBF43926u);
}

TEST(guide_checks_converter_tables) {
    for (const char* name : {"b26.bin", "a26.bin", "rows.bin"}) {
        const std::string t = table(name);
        guide::Info info;
        CHECK(t.size() > 48);
        CHECK(checkOf(t, &info) == Err::Ok);
        CHECK(info.valid && info.length == t.size());
    }
    guide::Info info;
    const std::string t = table("b26.bin");
    checkOf(t, &info);
    CHECK_STR(info.season, "B26");
    CHECK_EQ(info.modified, 1793448000u);
    // The SHA-256 the converter wrote into the manifest.
    const std::string manifest = table("b26.bin.json");
    const size_t at = manifest.find("\"sha256\": \"");
    CHECK(at != std::string::npos);
    CHECK_STR(hex(info.sha256, 32), manifest.substr(at + 11, 64));
    CHECK(manifest.find("\"entries\": " + std::to_string(info.entries) + ",") != std::string::npos);
}

TEST(guide_refuses_damaged_and_hostile_tables) {
    const std::string t = table("rows.bin");
    const uint32_t strOff = u32(t, 16), nstr = u32(t, strOff), first = u32(t, strOff + 4);
    auto with16 = [&](size_t off, uint16_t v) { std::string d = t; put16(d, off, v); return recrc(d); };
    auto with32 = [&](size_t off, uint32_t v) { std::string d = t; put32(d, off, v); return recrc(d); };
    CHECK(checkOf(t.substr(0, 47)) == Err::Short);
    CHECK(checkOf("HVGX" + t.substr(4)) == Err::Magic);
    CHECK(checkOf(with16(4, 2)) == Err::Format);
    CHECK(checkOf(with16(6, 52)) == Err::HeaderLen);
    CHECK(checkOf(t.substr(0, t.size() - 1)) == Err::Length);              // cut off: length past the end
    CHECK(checkOf(with32(20, 0x7FFFFFFF)) == Err::Length);                 // a huge length
    CHECK(checkOf(with32(20, 40)) == Err::Length);                         // shorter than the header
    {
        std::string d = t;
        d[60] ^= 1;
        CHECK(checkOf(d) == Err::Crc);
    }
    CHECK(checkOf(with32(8, 0xFFFFFFFF)) == Err::Layout);                  // a huge entry count
    CHECK(checkOf(with32(8, u32(t, 8) + 1)) == Err::Layout);
    CHECK(checkOf(with32(12, 0)) == Err::Layout);                          // entries over the header
    CHECK(checkOf(with32(16, static_cast<uint32_t>(t.size()))) == Err::Layout);
    CHECK(checkOf(with32(strOff, 0x40000000)) == Err::StringTable);        // a huge string count
    CHECK(checkOf(with32(strOff + 4, static_cast<uint32_t>(t.size()))) == Err::StringTable);  // offset at the end
    CHECK(checkOf(with32(strOff + 4, 0xFFFFFFF0)) == Err::StringTable);    // offset far past the end
    CHECK(checkOf(with32(strOff + 4, 0)) == Err::StringTable);             // offset into the header
    {
        std::string d = t;
        d.back() = 'x';  // the last string unterminated
        CHECK(checkOf(recrc(d)) == Err::String);
        d = t.substr(0, first) + std::string(65, 'x') + '\0';  // a 65-byte first string
        put32(d, 20, static_cast<uint32_t>(d.size()));
        CHECK(checkOf(recrc(d)) == Err::String);
        d = t.substr(0, first) + std::string(64, 'x') + '\0';  // 64 bytes is the most
        put32(d, 20, static_cast<uint32_t>(d.size()));
        CHECK(checkOf(recrc(d)) != Err::String);
    }
    for (const char* bad : {"\xff", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\t", "\x7f"}) {
        std::string d = t;
        std::memcpy(&d[first], bad, std::strlen(bad));
        CHECK(checkOf(recrc(d)) == Err::String);
    }
    {
        std::string d = t;
        std::memcpy(&d[first], "\xc3\xa1", 2);  // á is fine
        CHECK(checkOf(recrc(d)) == Err::Ok);
    }
    CHECK(checkOf(with16(48 + 14, 6000)) == Err::Order);                   // second entry below the first
    CHECK(checkOf(with16(48 + 14 * 2 + 4, 1300)) == Err::Order);           // 9000: start after the next one's
    CHECK(checkOf(with16(48 + 2, 0x7F0A)) == Err::Entry);                  // tenths 10
    CHECK(checkOf(with16(48 + 4, 1440)) == Err::Entry);
    CHECK(checkOf(with16(48 + 6, 0)) == Err::Entry);
    CHECK(checkOf(with16(48 + 6, 1441)) == Err::Entry);
    CHECK(checkOf(with16(48 + 8, static_cast<uint16_t>(nstr))) == Err::Index);
    CHECK(checkOf(with16(48 + 10, 0xFFFF)) == Err::Index);
    CHECK(checkOf(with16(48 + 12, static_cast<uint16_t>(nstr))) == Err::Index);
    // Fewer bytes available than the header.
    guide::Info info;
    CHECK(guide::check(guide::MemSource(t.data(), t.size()), 47, info) == Err::Short);
}

TEST(guide_days_text) {
    struct {
        uint8_t days;
        const char* text;
    } cases[] = {{0x1F, "Mo-Fr"}, {0x0A, "Tu,Th"}, {0x7D, "We-Mo"}, {0x60, "Sa,Su"}, {0x41, "Mo,Su"}, {0x61, "Sa-Mo"}, {0x43, "Su-Tu"},
                 {0x01, "Mo"},    {0x55, "Mo,We,Fr,Su"}, {0x1D, "Mo,We-Fr"}, {0x7B, "Th-Tu"}, {0x3F, "Mo-Sa"},
                 {0x7E, "Tu-Su"}, {0x00, ""}};
    for (const auto& c : cases) {
        char out[24];
        guide::daysText(c.days, out);
        CHECK_STR(out, c.text);
    }
}

TEST(guide_on_air_edges_and_midnight) {
    guide::Entry ten{9000, 0, 0x7F, 600, 660, 0, 0, 0};
    CHECK(!guide::onAir(ten, kTue + 9 * kHour + 59 * kMin));
    CHECK(guide::onAir(ten, kTue + 10 * kHour));
    CHECK(guide::onAir(ten, kTue + 10 * kHour + 59 * kMin + 59999));
    CHECK(!guide::onAir(ten, kTue + 11 * kHour));  // the end minute is not on air
    CHECK(!guide::onAir(ten, -1));                 // no clock
    guide::Entry night{9000, 0, 0x02, 1380, 60, 0, 0, 0};  // 2300-0100, Tuesday
    CHECK(guide::onAir(night, kTue + 23 * kHour + 30 * kMin));
    CHECK(guide::onAir(night, kTue + kDay + 30 * kMin));  // Wednesday 00:30: started on Tuesday
    CHECK(!guide::onAir(night, kTue + kDay + 60 * kMin));
    CHECK(!guide::onAir(night, kTue + 30 * kMin));         // Tuesday 00:30: started on Monday
    CHECK(!guide::onAir(night, kTue + kDay + 23 * kHour + 30 * kMin));
    guide::Entry allDay{9000, 0, 0x7F, 0, 1440, 0, 0, 0};
    CHECK(guide::onAir(allDay, kTue));
    CHECK(guide::onAir(allDay, kTue + kDay - 1));
    guide::Entry sunday{9000, 0, 0x40, 0, 1440, 0, 0, 0};
    CHECK(guide::onAir(sunday, kTue + 5 * kDay + 12 * kHour));
    CHECK(!guide::onAir(sunday, kTue + 6 * kDay + 12 * kHour));
    guide::Entry irr{9000, 0, 0x80, 0, 1440, 0, 0, 0};
    CHECK(!guide::onAir(irr, kTue + 12 * kHour));
    guide::Entry irrDays{9000, 0, 0xFF, 0, 1440, 0, 0, 0};
    CHECK(!guide::onAir(irrDays, kTue + 12 * kHour));
    guide::Entry empty{9000, 0, 0x7F, 761, 761, 0, 0, 0};  // 1241-1241
    CHECK(!guide::onAir(empty, kTue + 761 * kMin));
}

TEST(guide_lookup_within_1khz_and_order) {
    Look l("rows.bin");
    // Tuesday 10:30: Ten and All day on air in table order, then by start time from 10:30. 8998.9 and 9001.1 kHz are
    // more than 1 kHz away.
    CHECK_STR(l.at(90000, kTue + 10 * kHour + 30 * kMin), "Ten*|All day*|Weekday|Wednesday|Tuesday night|Irregular");
    CHECK_STR(l.at(90000, kTue + 11 * kHour), "Weekday*|All day*|Wednesday|Tuesday night|Irregular|Ten");
    CHECK_STR(l.at(90000, kTue + 23 * kHour + 30 * kMin), "Tuesday night*|All day*|Irregular|Ten|Weekday|Wednesday");
    CHECK_STR(l.at(90000, kTue + kDay + 12 * kHour + 15 * kMin),
              "Wednesday*|All day*|Tuesday night|Irregular|Ten|Weekday");
    // Without the clock: by start time from 0000, nothing on air.
    CHECK_STR(l.at(90000, -1), "All day|Irregular|Ten|Weekday|Wednesday|Tuesday night");
    // 0.1 kHz steps: 9000.5 sees 8999.9 to 9001.5.
    CHECK_STR(l.at(90005, -1), "All day|Too high|Irregular|Ten|Wednesday|Tuesday night");
    CHECK_STR(l.at(89989, -1), "Too low|Weekday");
    CHECK_STR(l.at(70000, -1), "");
    CHECK_STR(l.at(1500, -1), "");
    CHECK_STR(l.at(300000, -1), "");
    // Codes and the arena.
    l.at(90005, -1);
    CHECK_STR(l.hits.hit[4].lang, "F");
    CHECK_STR(l.hits.hit[4].target, "Af");
    CHECK(l.hits.hit[4].e.days == 0x04 && l.hits.hit[4].e.start == 720 && l.hits.hit[4].e.end == 780);
    // At most 16, the on-air one first.
    CHECK_STR(l.at(120000, kTue + 5 * kHour + 10 * kMin),
              "Hour 05*|Hour 06|Hour 07|Hour 08|Hour 09|Hour 10|Hour 11|Hour 12|Hour 13|Hour 14|Hour 15|Hour 16|"
              "Hour 17|Hour 18|Hour 19|Hour 00");
    CHECK_EQ(l.hits.n, guide::kMaxHits);
}

TEST(guide_lookup_b26) {
    Look l("b26.bin");
    CHECK_STR(l.at(60700, kTue + 13 * kHour + 5 * kMin),
              "CFRX Toronto, CFRB 1010*|Channel 292*|China Radio Int.|China Radio Int.|SM Radio Dessau|"
              "Voice of Korea|SM Radio Dessau");
    CHECK_STR(l.at(60700, kTue + 17 * kHour + 30 * kMin),
              "CFRX Toronto, CFRB 1010*|Channel 292*|China Radio Int.*|China Radio Int.|SM Radio Dessau|"
              "Voice of Korea|SM Radio Dessau");
    CHECK_STR(l.at(60700, -1), "CFRX Toronto, CFRB 1010|Channel 292|SM Radio Dessau|Voice of Korea|SM Radio Dessau|"
                                "China Radio Int.|China Radio Int.");
    CHECK_STR(l.at(60720, -1), "");
    l.at(60700, -1);
    CHECK(l.hits.hit[2].e.days == guide::kIrregular);
    CHECK_STR(l.hits.hit[1].lang, "D,E");
}

TEST(guide_lookup_arena_bounds_long_strings) {
    // 16 entries with 64-byte strings fill the arena: the list ends where the strings stop fitting.
    std::string t = table("rows.bin");
    const uint32_t strOff = u32(t, 16), nstr = u32(t, strOff);
    std::string head = t.substr(0, strOff + 4 + 4 * nstr);
    std::string strings;
    for (uint32_t i = 0; i < nstr; i++) {
        put32(head, strOff + 4 + 4 * i, static_cast<uint32_t>(head.size() + strings.size()));
        std::string s(64, static_cast<char>('A' + i % 26));
        strings += s + '\0';
    }
    t = head + strings;
    put32(t, 20, static_cast<uint32_t>(t.size()));
    t = recrc(t);
    guide::Info info;
    CHECK(checkOf(t, &info) == Err::Ok);
    guide::Hits hits;
    guide::lookup(guide::MemSource(t.data(), t.size()), info, 12000000, -1, hits);
    CHECK(hits.n > 0 && hits.n < guide::kMaxHits);
    CHECK(hits.used <= guide::Hits::kArena);
    for (int i = 0; i < hits.n; i++) CHECK_EQ(std::strlen(hits.hit[i].name), 64u);
}

TEST(guide_slots_update_and_boot) {
    const std::string a = table("rows.bin"), b = table("b26.bin");
    FakeFlash flash;
    {
        guide::Slots s(flash);
        s.boot();
        CHECK_EQ(s.active(), -1);
        CHECK(!s.info().valid);
        // The first table goes to slot 0.
        CHECK(upload(s, a) == Err::Ok);
        CHECK_EQ(s.active(), 0);
        CHECK_STR(s.info().season, "A26");
        // A new table to the free slot; the old slot's first sector is erased at the switch.
        guide::Sha256 h;
        h.update(b.data(), b.size());
        uint8_t sha[32];
        h.finish(sha);
        CHECK(upload(s, b, sha) == Err::Ok);
        CHECK_EQ(s.active(), 1);
        CHECK_EQ(s.info().entries, 8780u);
        CHECK(std::all_of(flash.m.begin(), flash.m.begin() + guide::kSector, [](uint8_t x) { return x == 0xFF; }));
    }
    {
        guide::Slots s(flash);
        s.boot();
        CHECK_EQ(s.active(), 1);
        CHECK_STR(s.info().season, "B26");
        // A failed table leaves the old one in use: a damaged byte, a cut-off body, a hash mismatch, too long.
        std::string bad = a;
        bad[100] ^= 1;
        CHECK(upload(s, bad) == Err::Crc);
        CHECK_EQ(s.active(), 1);
        CHECK(upload(s, a, nullptr, static_cast<uint32_t>(a.size() - 10)) == Err::Cut);
        uint8_t zero[32] = {};
        CHECK(upload(s, a, zero) == Err::Hash);
        CHECK(upload(s, std::string(guide::kSlotSize + 1, 'x')) == Err::TooLong);
        CHECK(upload(s, "HVGD") == Err::Short);
        flash.failWrites = true;
        CHECK(upload(s, a) == Err::Write);
        flash.failWrites = false;
        CHECK_EQ(s.active(), 1);
        guide::Info info;
        CHECK(guide::check(s.source(), guide::kSlotSize, info) == Err::Ok && info.entries == 8780);
        // Lookups read the old table until the switch: a checked table not yet committed changes nothing.
        CHECK(upload(s, a, nullptr, UINT32_MAX, false) == Err::Ok);
        CHECK_EQ(s.active(), 1);
        CHECK_STR(s.info().season, "B26");
    }
    {
        // The checked table left in slot 0 carries the same EiBi date as B26 in slot 1: both are valid at boot, and
        // a tie keeps slot 0.
        guide::Slots s(flash);
        s.boot();
        CHECK_EQ(s.active(), 0);
        CHECK_STR(s.info().season, "A26");
    }
}

TEST(guide_slots_both_valid_and_neither) {
    std::string older = table("rows.bin"), newer = table("rows.bin");
    put32(newer, 28, u32(older, 28) + 86400);  // a later EiBi date (the header is outside the CRC)
    for (int newSlot = 0; newSlot < 2; newSlot++) {
        FakeFlash flash;
        flash.place(newSlot, newer);
        flash.place(1 - newSlot, older);
        guide::Slots s(flash);
        s.boot();
        CHECK_EQ(s.active(), newSlot);
        CHECK_EQ(s.info().modified, u32(newer, 28));
        CHECK_EQ(flash.m[(1 - newSlot) * guide::kSlotSize], 0xFF);  // the other's first sector erased
        CHECK_EQ(flash.erases, 1);
    }
    FakeFlash flash;
    std::string bad = older;
    bad[0] = 'X';
    flash.place(0, bad);
    flash.place(1, older.substr(0, older.size() - 50));  // a cut-off write
    guide::Slots s(flash);
    s.boot();
    CHECK_EQ(s.active(), -1);
    CHECK(!s.info().valid);
    CHECK_EQ(flash.erases, 0);
    // A write cut off by power: the half-written slot fails at boot and the old one stays.
    const std::string big = table("b26.bin");
    FakeFlash f2;
    f2.place(0, older);
    guide::Slots s2(f2);
    s2.boot();
    static uint8_t sector[guide::kSector];
    guide::Slots::Writer w(s2, sector);
    CHECK(w.begin(big.size()) == Err::Ok);
    CHECK_EQ(w.slot(), 1);
    CHECK(w.add(big.data(), 50000) == Err::Ok);
    guide::Slots after(f2);
    after.boot();
    CHECK_EQ(after.active(), 0);
}
