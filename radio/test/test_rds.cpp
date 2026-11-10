// The RDS decoder: groups captured on the bench replayed through it, and the acceptance rules, the AF codes and the
// character table on made-up groups.
#include <cstdio>
#include <string>
#include <vector>

#include "rds.h"
#include "util.h"

namespace {

// Groups in the form GET /api/rds/groups sends them: {"groups":["<ms> AAAA BBBB CCCC DDDD EE", ...]}.
std::vector<rds::Raw> capture(const std::string& name) {
    const std::string text = readFile(t::root() + "/radio/test/rds/" + name);
    std::vector<rds::Raw> out;
    for (size_t i = text.find('['); i != std::string::npos && (i = text.find('"', i)) != std::string::npos;) {
        const size_t end = text.find('"', i + 1);
        unsigned long ms;
        unsigned a, b, c, d, ble;
        if (std::sscanf(text.substr(i + 1, end - i - 1).c_str(), "%lu %x %x %x %x %x", &ms, &a, &b, &c, &d, &ble) == 6)
            out.push_back({static_cast<uint32_t>(ms),
                           {{static_cast<uint16_t>(a), static_cast<uint16_t>(b), static_cast<uint16_t>(c),
                             static_cast<uint16_t>(d)},
                            static_cast<uint8_t>(ble)}});
        i = end + 1;
    }
    return out;
}

std::string ps(const rds::Decoder& d, bool display = false) {
    char buf[rds::kPsBuf];
    d.ps(buf, sizeof buf, display);
    return buf;
}

std::string rt(const rds::Decoder& d, bool display = false) {
    char buf[rds::kRtBuf];
    d.rt(buf, sizeof buf, display);
    return buf;
}

std::string afList(const rds::Decoder& d) {
    std::string s;
    char buf[16];
    for (int i = 0; i < d.afCount(); i++) {
        std::snprintf(buf, sizeof buf, "%s%ld", i ? " " : "", static_cast<long>(d.afHz(i)));
        s += buf;
    }
    return s;
}

constexpr uint16_t kPi = 0xF395;

uint8_t ble(int a, int b, int c, int d) { return static_cast<uint8_t>(a << 6 | b << 4 | c << 2 | d); }

// Group 0A: PS segment `seg` with two characters, TA, and two AF codes in block C.
rds::Group g0a(int seg, const char* two, uint8_t levels = 0, uint8_t af1 = 205, uint8_t af2 = 205, int ta = 0,
               int pty = 1, uint16_t pi = kPi) {
    const uint16_t b = static_cast<uint16_t>(0x0000 | 1 << 10 | pty << 5 | ta << 4 | seg);
    return {{pi, b, static_cast<uint16_t>(af1 << 8 | af2),
             static_cast<uint16_t>(static_cast<uint8_t>(two[0]) << 8 | static_cast<uint8_t>(two[1]))},
            levels};
}

// Group 2A: RadioText segment `seg` with four characters and the A/B flag.
rds::Group g2a(int seg, const char* four, int ab = 0, uint8_t levels = 0, int pty = 1) {
    const auto u = [&](int i) { return static_cast<uint8_t>(four[i]); };
    return {{kPi, static_cast<uint16_t>(0x2000 | 1 << 10 | pty << 5 | ab << 4 | seg),
             static_cast<uint16_t>(u(0) << 8 | u(1)), static_cast<uint16_t>(u(2) << 8 | u(3))},
            levels};
}

void feed(rds::Decoder& d, const rds::Group& g, int times = 1) {
    for (int i = 0; i < times; i++) d.group(g);
}

// PS sent twice round, as a station does.
void sendPs(rds::Decoder& d, const char* ps8) {
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < 4; s++) d.group(g0a(s, ps8 + 2 * s));
}

}  // namespace

// Radio Metro on 99.5 MHz, 70 s. Until about 40 s it carried the news (PS NYHETER, PTY 1), then music (PS -METRO-,
// PTY 3) with a new RadioText under the other A/B flag. Segment 0 of that text came seven times, and only once with
// block B at level 0, so the new text never shows.
TEST(rds_metro_capture) {
    const auto groups = capture("metro-99.5.json");
    CHECK_EQ(static_cast<int>(groups.size()), 784);
    rds::Decoder d;
    d.setSync(true);
    size_t i = 0;
    for (; i < groups.size() && groups[i].t < 40000; i++) d.group(groups[i].g);
    CHECK(d.hasPi());
    CHECK_EQ(d.pi(), 0xF395);
    CHECK_STR(ps(d), "NYHETER");
    CHECK_STR(rt(d), "METRO gir deg nasjonale nyheter");
    CHECK_EQ(d.pty(), 1);
    CHECK(d.tp());
    CHECK(!d.ta());
    const uint8_t seq = d.rtSeq();
    for (; i < groups.size(); i++) d.group(groups[i].g);
    CHECK(d.sync());
    CHECK_EQ(d.pi(), 0xF395);
    CHECK_STR(ps(d), "-METRO-");
    CHECK(d.rtSeq() != seq);
    CHECK_STR(rt(d), "");
    CHECK_EQ(d.pty(), 3);
    CHECK(d.tp());
    CHECK(!d.ta());
    CHECK_STR(afList(d), "88400000 90900000 99000000 99500000 102900000 104600000 105800000 106600000 107000000 "
                         "107900000");
}

TEST(rds_metro_block_b_errors_ignored) {
    // The groups whose block B has errors carried types 1A, 3A, 4A, 12A and others, with Metro's PS and RT in
    // their payload. Only their PI counts.
    rds::Decoder d;
    int n = 0;
    for (const auto& r : capture("metro-99.5.json"))
        if (r.g.level(1) > 0) {
            d.group(r.g);
            n++;
        }
    CHECK(n > 10);
    CHECK(d.hasPi());
    CHECK_STR(ps(d), "");
    CHECK_STR(rt(d), "");
    CHECK_EQ(d.pty(), 0);
    CHECK(!d.tp());
    CHECK_EQ(d.afCount(), 0);
}

TEST(rds_ring) {
    rds::Ring ring;
    CHECK_EQ(ring.count(), 0);
    const auto groups = capture("metro-99.5.json");
    for (const auto& r : groups) ring.push(r.t, r.g);
    CHECK_EQ(ring.count(), rds::Ring::kLen);
    char line[40];
    rds::Ring::format(ring.at(rds::Ring::kLen - 1), line, sizeof line);
    CHECK_STR(line, "69971 F395 0467 C3CD 2D20 30");
    rds::Ring::format(ring.at(0), line, sizeof line);
    const rds::Raw& first = groups[groups.size() - rds::Ring::kLen];
    char want[40];
    rds::Ring::format(first, want, sizeof want);
    CHECK_STR(line, want);
}

TEST(rds_block_b_level) {
    rds::Decoder d;
    // Block B at level 1: no type, no PS, no PTY.
    const char* name = "ABCDEFGH";
    for (int k = 0; k < 4; k++)
        for (int s = 0; s < 4; s++) d.group(g0a(s, name + 2 * s, ble(0, 1, 0, 0)));
    CHECK_STR(ps(d), "");
    CHECK_EQ(d.pty(), 0);
    sendPs(d, "ABCDEFGH");
    CHECK_STR(ps(d), "ABCDEFGH");
}

TEST(rds_text_levels) {
    rds::Decoder d;
    // Block D at level 2 never counts; at level 1 it does.
    feed(d, g0a(0, "AB", ble(0, 0, 0, 2)), 3);
    CHECK_STR(ps(d), "");
    feed(d, g0a(0, "AB", ble(0, 0, 0, 1)));
    CHECK_STR(ps(d), "");
    feed(d, g0a(0, "AB", ble(0, 0, 0, 1)));
    CHECK_STR(ps(d), "AB");
    // Shown up to the first position not accepted.
    feed(d, g0a(1, "CD"));
    CHECK_STR(ps(d), "AB");
    feed(d, g0a(1, "CD"));
    CHECK_STR(ps(d), "ABCD");
    // A change needs two arrivals in a row too; one stray character does not change it.
    feed(d, g0a(0, "XB"));
    CHECK_STR(ps(d), "ABCD");
    feed(d, g0a(0, "AB"));
    feed(d, g0a(0, "XB"));
    CHECK_STR(ps(d), "ABCD");
    feed(d, g0a(0, "XB"));
    CHECK_STR(ps(d), "XBCD");
    // RadioText block C at level 2 is skipped, block D still counts.
    rds::Decoder r;
    feed(r, g2a(0, "abcd", 0, ble(0, 0, 2, 0)), 2);
    CHECK_STR(rt(r), "");
    feed(r, g2a(0, "abcd", 0, ble(0, 0, 1, 0)), 2);
    CHECK_STR(rt(r), "abcd");
}

TEST(rds_text_ends) {
    rds::Decoder d;
    // Trailing spaces are dropped; a carriage return ends the RadioText.
    feed(d, g2a(0, "Hi  "), 2);
    CHECK_STR(rt(d), "Hi");
    feed(d, g2a(1, "yo\r "), 2);
    CHECK_STR(rt(d), "Hi  yo");
    // Control codes other than the carriage return are dropped.
    feed(d, g2a(1, "y\no\r"), 2);
    CHECK_STR(rt(d), "Hi  yo");
    // PS keeps inner spaces and drops trailing ones.
    rds::Decoder p;
    sendPs(p, "NRK P1  ");
    CHECK_STR(ps(p), "NRK P1");
}

TEST(rds_rt_ab_flag) {
    rds::Decoder d;
    const uint8_t seq = d.rtSeq();
    feed(d, g2a(0, "Song"), 2);
    feed(d, g2a(1, " one"), 2);
    CHECK_STR(rt(d), "Song one");
    CHECK_EQ(d.rtSeq(), seq);
    // The flag flips: the text clears, and the new one builds up from its own characters.
    feed(d, g2a(1, " two", 1));
    CHECK_STR(rt(d), "");
    CHECK(d.rtSeq() != seq);
    const uint8_t seq2 = d.rtSeq();
    feed(d, g2a(0, "Song", 1), 2);
    CHECK_STR(rt(d), "Song");
    feed(d, g2a(1, " two", 1));
    CHECK_STR(rt(d), "Song two");
    CHECK_EQ(d.rtSeq(), seq2);
    // 2B: two characters per segment in block D, PI in block C.
    rds::Decoder b;
    const rds::Group g{{kPi, 0x2800 | 1 << 10 | 1 << 5 | 1, kPi, 'H' << 8 | 'i'}, 0};
    feed(b, g, 2);
    CHECK_STR(rt(b), "");  // segment 1 without segment 0
    feed(b, rds::Group{{kPi, 0x2800 | 1 << 10 | 1 << 5, kPi, 'O' << 8 | 'h'}, 0}, 2);
    CHECK_STR(rt(b), "OhHi");
}

TEST(rds_pi) {
    rds::Decoder d;
    // Block A with errors is not used.
    feed(d, g0a(0, "AB", ble(1, 0, 0, 0)), 3);
    CHECK(!d.hasPi());
    rds::Decoder e;
    feed(e, g0a(0, "AB"));
    CHECK(!e.hasPi());
    feed(e, g0a(0, "AB"));
    CHECK(e.hasPi());
    CHECK_EQ(e.pi(), kPi);
    // A B-version group carries PI in block C: block A bad, block C good.
    rds::Decoder f;
    feed(f, rds::Group{{0, 0x2800 | 1 << 10, kPi, 0x4142}, ble(3, 0, 0, 0)}, 2);
    CHECK(f.hasPi());
    CHECK_EQ(f.pi(), kPi);
}

TEST(rds_pi_change_resets) {
    rds::Decoder d;
    d.setSync(true);
    sendPs(d, "-METRO- ");
    feed(d, g2a(0, "Text"), 2);
    feed(d, g0a(0, "-M", 0, 99, 120), 3);
    CHECK_STR(ps(d), "-METRO-");
    CHECK_EQ(d.pty(), 1);
    CHECK_EQ(d.afCount(), 2);
    const uint8_t seq = d.rtSeq();
    // One group of another PI: nothing changes yet.
    feed(d, g0a(1, "XY", 0, 205, 205, 0, 10, 0x1234));
    CHECK_EQ(d.pi(), kPi);
    CHECK_STR(ps(d), "-METRO-");
    // The second in a row: every field is cleared.
    feed(d, g0a(1, "XY", 0, 205, 205, 0, 10, 0x1234));
    CHECK_EQ(d.pi(), 0x1234);
    CHECK_STR(ps(d), "");
    CHECK_STR(rt(d), "");
    CHECK_EQ(d.pty(), 0);
    CHECK_EQ(d.afCount(), 0);
    CHECK(d.rtSeq() != seq);
    CHECK(d.sync());
    // A reset clears the PI as well.
    d.reset();
    CHECK(!d.hasPi());
    CHECK(!d.sync());
}

TEST(rds_hold) {
    rds::Decoder d;
    feed(d, g0a(0, "AB", 0, 205, 205, 1, 10), 2);
    CHECK_EQ(d.pty(), 0);
    CHECK(!d.tp());
    CHECK(!d.ta());
    feed(d, g0a(0, "AB", 0, 205, 205, 1, 10));
    CHECK_EQ(d.pty(), 10);
    CHECK(d.tp());
    CHECK(d.ta());
    // Two of another value in a row do not change it; a third does.
    feed(d, g0a(0, "AB", 0, 205, 205, 0, 4), 2);
    feed(d, g0a(0, "AB", 0, 205, 205, 1, 10));
    feed(d, g0a(0, "AB", 0, 205, 205, 0, 4), 2);
    CHECK_EQ(d.pty(), 10);
    CHECK(d.ta());
    feed(d, g0a(0, "AB", 0, 205, 205, 0, 4));
    CHECK_EQ(d.pty(), 4);
    CHECK(!d.ta());
    // TA comes from group 0 only.
    feed(d, g2a(0, "abcd", 0, 0, 4), 3);
    CHECK(!d.ta());
}

TEST(rds_af) {
    rds::Decoder d;
    // A count (9 follow), FM codes, a filler and a code outside the table; repeats are kept once.
    feed(d, g0a(0, "AB", 0, 224 + 9, 9));
    feed(d, g0a(1, "CD", 0, 120, 205));
    feed(d, g0a(2, "EF", 0, 9, 1));
    feed(d, g0a(3, "GH", 0, 204, 210));
    CHECK_STR(afList(d), "87600000 88400000 99500000 107900000");
    // Block C with errors is not used.
    feed(d, g0a(0, "AB", ble(0, 0, 1, 0), 50, 51));
    CHECK_EQ(d.afCount(), 4);
    // LW and MW after code 250: 1 is 153 kHz, 15 is 279 kHz, 16 is 531 kHz, 135 is 1602 kHz.
    rds::Decoder l;
    feed(l, g0a(0, "AB", 0, 250, 1));
    feed(l, g0a(0, "AB", 0, 250, 15));
    feed(l, g0a(0, "AB", 0, 250, 16));
    feed(l, g0a(0, "AB", 0, 250, 135));
    feed(l, g0a(0, "AB", 0, 250, 136));  // outside the LW/MW table
    feed(l, g0a(0, "AB", 0, 1, 250));    // FM 87.6, then 250 with nothing after it in this block
    feed(l, g0a(0, "AB", 0, 2, 205));    // so 2 is FM again
    CHECK_STR(afList(l), "153000 279000 531000 1602000 87600000 87700000");
    // Group 0B carries no AF list.
    rds::Decoder b;
    feed(b, rds::Group{{kPi, 0x0800 | 1 << 10, kPi, 0x4142}, 0}, 2);
    CHECK_EQ(b.afCount(), 0);
    // At most 25.
    rds::Decoder m;
    for (int c = 1; c <= 40; c += 2) feed(m, g0a(0, "AB", 0, static_cast<uint8_t>(c), static_cast<uint8_t>(c + 1)));
    CHECK_EQ(m.afCount(), rds::kMaxAf);
}

TEST(rds_characters) {
    // Every character the radio shows has a Pip glyph, or is "?".
    for (int c = 0; c < 256; c++) {
        const char* full = rds::charText(static_cast<uint8_t>(c), false);
        const char* shown = rds::charText(static_cast<uint8_t>(c), true);
        const bool control = c < 0x20 || c == 0x7F;
        CHECK_EQ(*full == 0, control);
        if (std::string(shown) == "?") {
            // Only where the font lacks the character.
            const char* p = full;
            const uint32_t cp = gfx::nextCp(p);
            if (cp != '?' && gfx::pipGlyph(cp))
                t::fail(__FILE__, __LINE__, "code " + std::to_string(c) + " has a glyph but shows as ?");
            continue;
        }
        CHECK_STR(shown, full);
        for (const char* p = shown; uint32_t cp = gfx::nextCp(p);)
            if (!gfx::pipGlyph(cp)) t::fail(__FILE__, __LINE__, "code " + std::to_string(c) + ": no glyph");
    }
    CHECK_STR(rds::charText(0x41, false), "A");
    CHECK_STR(rds::charText(0x24, false), "¤");
    CHECK_STR(rds::charText(0xAB, false), "$");
    CHECK_STR(rds::charText(0x91, false), "ä");
    CHECK_STR(rds::charText(0x8D, false), "ß");
    CHECK_STR(rds::charText(0xA9, false), "€");
    CHECK_STR(rds::charText(0xA9, true), "?");
    // Through the decoder: the web page gets the full text, the display the font's characters.
    rds::Decoder d;
    const char raw[] = {'\xA9', '5', ' ', '\x91'};
    feed(d, g2a(0, raw), 2);
    CHECK_STR(rt(d), "€5 ä");
    CHECK_STR(rt(d, true), "?5 ä");
}

TEST(rds_size) { CHECK(sizeof(rds::Decoder) < 300); }
