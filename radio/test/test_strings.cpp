// Every UI string in every language has glyphs and fits each slot it is drawn in; the test fails rather
// than letting the display clip. Placeholder values are typical: band "160m", network "Telenor-5G".
#include <cstdarg>
#include <cstring>
#include <regex>

#include "i18n_strings.h"
#include "rds.h"
#include "util.h"

using i18n::Str;

namespace {

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

int w(const std::string& s) { return gfx::textW(s.c_str()); }

std::string widest(std::initializer_list<std::string> v) {
    std::string best;
    for (const auto& s : v)
        if (w(s) > w(best)) best = s;
    return best;
}

// Eight of the widest character the radio can show in a PS.
std::string widestPs() {
    std::string best;
    for (int code = 0; code < 256; code++) {
        const char* ch = rds::charText(static_cast<uint8_t>(code), true);
        if (w(ch) > w(best)) best = ch;
    }
    std::string ps;
    for (int i = 0; i < rds::kPsLen; i++) ps += best;
    return ps;
}

struct Checker {
    int lang;
    const char* code;
    const char* tr(Str k) const { return i18n::tr(lang, k); }
    void fit(const std::string& what, int width, int limit) const {
        if (width > limit)
            t::fail(__FILE__, __LINE__,
                    std::string(code) + ": " + what + " is " + std::to_string(width) + " px, slot " + std::to_string(limit));
    }
    // A list row: label from x = 11, value right-aligned at x = 124, at least 4 px apart.
    void row(const std::string& label, const std::string& value) const {
        fit("row '" + label + "' + '" + value + "'", w(label) + (value.empty() ? 0 : 4 + w(value)), 113);
    }
};

}  // namespace

TEST(strings_have_glyphs) {
    for (int l = 0; l < i18n::kLangCount; l++) {
        for (int k = 0; k < i18n::kStrCount; k++) {
            const char* p = i18n::kTable[l][k];
            while (uint32_t cp = gfx::nextCp(p))
                if (!gfx::pipGlyph(cp))
                    t::fail(__FILE__, __LINE__,
                            std::string(i18n::kLangCode[l]) + "." + i18n::kKey[k] + ": no glyph U+" + std::to_string(cp));
        }
        const char* p = i18n::kLangName[l];
        while (uint32_t cp = gfx::nextCp(p)) CHECK(gfx::pipGlyph(cp) != nullptr);
    }
}

TEST(strings_placeholders_match_english) {
    const std::regex ph("%[ds%]");
    for (int k = 0; k < i18n::kStrCount; k++) {
        const std::string en = i18n::kTable[0][k];
        std::string want;
        for (auto it = std::sregex_iterator(en.begin(), en.end(), ph); it != std::sregex_iterator(); ++it) want += it->str();
        for (int l = 1; l < i18n::kLangCount; l++) {
            const std::string s = i18n::kTable[l][k];
            std::string got;
            for (auto it = std::sregex_iterator(s.begin(), s.end(), ph); it != std::sregex_iterator(); ++it) got += it->str();
            if (got != want) t::fail(__FILE__, __LINE__, std::string(i18n::kLangCode[l]) + "." + i18n::kKey[k]);
        }
    }
}

TEST(strings_fit_slots) {
    for (int l = 0; l < i18n::kLangCount; l++) {
        const Checker c{l, i18n::kLangCode[l]};
        // Lists.
        c.row(c.tr(Str::band), "160m");
        c.row(c.tr(Str::mode), "LSB");
        c.row(c.tr(Str::step), "100Hz");
        c.row(c.tr(Str::bandwidth), "Auto");
        c.row(c.tr(Str::volume), widest({c.tr(Str::val_mute), "63"}));
        c.row(c.tr(Str::seek), "");
        c.row("FM", c.tr(Str::only_fm));
        c.row("LSB", c.tr(Str::lower_ssb));
        c.row("USB", c.tr(Str::upper_ssb));
        c.row(c.tr(Str::seek_up), "");
        c.row(c.tr(Str::seek_down), "");
        c.row(c.tr(Str::status), widest({c.tr(Str::not_joined), "radio.local"}));
        c.row(c.tr(Str::wifi), c.tr(Str::not_joined));
        c.row(c.tr(Str::agc), widest({c.tr(Str::on), c.tr(Str::off)}));
        c.row(c.tr(Str::contrast), "15");
        c.row(c.tr(Str::light), widest({c.tr(Str::off), "15"}));
        c.row(c.tr(Str::side_button), widest({c.tr(Str::val_mute), c.tr(Str::val_preset), c.tr(Str::val_back)}));
        c.row(c.tr(Str::language), i18n::kLangName[l]);
        c.row(c.tr(Str::about), "");
        c.row(c.tr(Str::side_mute), "");
        c.row(c.tr(Str::side_next), "");
        c.row(c.tr(Str::side_back), "");
        c.row(i18n::kLangName[l], "");
        // Home top row: band, mode and step end before the signal bars; the Mute flag at x = 80 grows with
        // its text and stays clear of the bars at x = 108.
        int x = w("160m") + 1;
        x += 5 + w("LSB") + 1;
        c.fit("home top row", x + 5 + w(std::string(c.tr(Str::home_step)) + " 100Hz"), 106);
        c.fit("Mute flag", w(c.tr(Str::home_mute)), 24);
        c.fit("seek row", w(std::string("160m  ") + c.tr(Str::seek) + " ▶"), 106);
        // Toasts, from x = 1.
        for (const std::string& s : {fmt(c.tr(Str::toast_muted)), fmt(c.tr(Str::toast_sound)),
                                     fmt(c.tr(Str::toast_saved), 20), fmt(c.tr(Str::toast_full)),
                                     fmt(c.tr(Str::toast_none)), fmt(c.tr(Str::toast_preset), 20, "160m"),
                                     fmt(c.tr(Str::toast_joining), "Telenor-5G"), fmt(c.tr(Str::toast_no_seek)),
                                     fmt(c.tr(Str::toast_cancelled))})
            c.fit("toast '" + s + "'", w(s), 126);
        // Info, adjust and confirm lines, from x = 10.
        for (const std::string& s : {fmt(c.tr(Str::joined_s), "Telenor-5G"), fmt(c.tr(Str::not_joined_cap)),
                                     fmt(c.tr(Str::access_point)), fmt(c.tr(Str::battery_d), 100),
                                     fmt(c.tr(Str::volume)), fmt(c.tr(Str::contrast)), fmt(c.tr(Str::light)),
                                     fmt(c.tr(Str::confirm_title)), fmt(c.tr(Str::confirm_press)),
                                     fmt(c.tr(Str::confirm_long))})
            c.fit("line '" + s + "'", w(s), 117);
        // Text entry title with the character group at the right.
        const std::string title = fmt(c.tr(Str::password_for), "Telenor-5G");
        c.fit("entry title", w(title) + 3 + w(widest({"abc", "ABC", "123", "#+=", c.tr(Str::group_edit)})), 128);
        // Main menu on FM, and the Station screen: `No RDS` from x = 10, and each programme type right-aligned in
        // the title row, 3 px clear of an 8-character PS of the widest glyphs drawn from x = 10.
        c.row(c.tr(Str::station), "");
        c.fit("line '" + std::string(c.tr(Str::no_rds)) + "'", w(c.tr(Str::no_rds)), 118);
        for (int p = 1; p < rds::kPtyCount; p++) {
            const char* name = c.tr(static_cast<Str>(static_cast<int>(Str::pty_1) + p - 1));
            c.fit(std::string("programme type '") + name + "'", 10 + w(widestPs()) + 3 + w(name), 128);
        }
        // Full-screen messages, centred.
        c.fit("message", w(c.tr(Str::installing)), 128);
        c.fit("message", w(c.tr(Str::battery_empty)), 128);
    }
}

TEST(language_list) {
    CHECK_STR(i18n::kLangCode[0], "en");
    CHECK_STR(i18n::kLangName[0], "English");
    for (int l = 0; l < i18n::kLangCount; l++)
        for (int m = l + 1; m < i18n::kLangCount; m++) CHECK(std::strcmp(i18n::kLangCode[l], i18n::kLangCode[m]));
}
