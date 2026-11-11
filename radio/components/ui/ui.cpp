#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clock_core.h"
#include "i18n_strings.h"

namespace ui {

using gfx::Frame;
using radio::Cmd;
using radio::Mode;
using radio::State;
using i18n::Str;

namespace {

constexpr double kPi = 3.14159265358979323846;

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int mod(int a, int n) { return ((a % n) + n) % n; }
int jsRoundI(double x) { return static_cast<int>(radio::jsRound(x)); }

void copy(char* dst, size_t n, const char* src) {
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// ---- text entry character ring ----
const char* const kGroupName[] = {"abc", "ABC", "123", "#+="};
const char* const kGroupChars[] = {"abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789",
                                   "!@#$%&*-_.+=?/:;,'\"()"};
const char* const kEditKeys[] = {"SPACE", "DEL", "OK"};
constexpr int kGroups = 5;
constexpr int kGroupStart[kGroups] = {0, 26, 52, 62, 83};
constexpr int kRingLen = 86;

int groupOf(int idx) {
    int g = 0;
    for (int i = 0; i < kGroups; i++)
        if (kGroupStart[i] <= idx) g = i;
    return g;
}

// The key at ring position idx: a one-character string, or SPACE / DEL / OK.
const char* ringKey(int idx, char* buf) {
    const int g = groupOf(idx), k = idx - kGroupStart[g];
    if (g == 4) return kEditKeys[k];
    buf[0] = kGroupChars[g][k];
    buf[1] = 0;
    return buf;
}

// The key as drawn in the ring: SPACE shows as an open box.
const char* ringLabel(int idx, char* buf) {
    const char* k = ringKey(mod(idx, kRingLen), buf);
    return std::strcmp(k, "SPACE") == 0 ? "␣" : k;
}

// Text cut with an ellipsis to at most w pixels, and to whole characters within the n-byte buffer.
void cutText(const char* s, int w, char* out, size_t n) {
    size_t k = std::strlen(s);
    const bool tooLong = k > n - 1;
    if (tooLong) k = n - 1;
    std::memcpy(out, s, k);
    out[k] = 0;
    if (!tooLong && gfx::textW(out) <= w) return;
    for (;;) {
        while (k && (static_cast<uint8_t>(out[k]) & 0xC0) == 0x80) k--;  // whole characters
        if (k + 4 <= n) {
            std::memcpy(out + k, "\u2026", 4);
            if (gfx::textW(out) <= w || !k) return;
        }
        k--;
    }
}

// Age of the last sync: whole minutes below an hour, else whole hours.
void ageText(int lang, int64_t ms, char* out, size_t n) {
    const long m = static_cast<long>(ms / 60000);
    if (m < 60)
        snprintf(out, n, i18n::tr(lang, Str::age_min), static_cast<int>(m));
    else
        snprintf(out, n, i18n::tr(lang, Str::age_h), static_cast<int>(m / 60));
}

// Guide text as the display draws it: a character without a Pip glyph is "?".
void pipText(const char* s, char* out, size_t n) {
    size_t k = 0;
    const char* p = s;
    for (;;) {
        const char* start = p;
        const uint32_t cp = gfx::nextCp(p);
        if (!cp) break;
        const bool known = gfx::pipGlyph(cp) != nullptr;
        const size_t len = known ? static_cast<size_t>(p - start) : 1;
        if (k + len >= n) break;
        if (known)
            std::memcpy(out + k, start, len);
        else
            out[k] = '?';
        k += len;
    }
    out[k] = 0;
}

// 0..1440 minutes as HHMM.
void hhmm4(int m, char* out) { snprintf(out, 5, "%02d%02d", m / 60 % 100, m % 60); }

// The entry's window, "1700-1800".
void windowText(const guide::Entry& e, char* out) {
    hhmm4(e.start, out);
    out[4] = '-';
    hhmm4(e.end, out + 5);
}

// The days: `irr`, `daily`, or EiBi's form.
const char* daysLabel(int lang, uint8_t days, char* buf) {
    if (days & guide::kIrregular) return i18n::tr(lang, Str::irr);
    if ((days & guide::kEveryDay) == guide::kEveryDay) return i18n::tr(lang, Str::daily);
    guide::daysText(days, buf);
    return buf;
}

bool guideLoaded(const Status& st) { return st.guide && st.guide->valid; }

void barsText(int n, char* out) {
    for (int i = 0; i < 3; i++) out[i] = i < n ? '|' : '.';
    out[3] = 0;
}

// ---- splash: the wheel ----
const double kTongues[5][2] = {{-0.6, 6}, {-0.15, 10}, {0.3, 12}, {0.75, 10}, {1.2, 7}};
constexpr int kWheelCy = 16;
constexpr int kRollFrom = -16, kRollTo = 17;  // cx as the wheel rolls in
// The box around the centre that can be on screen: the tread reaches 14 px to the right; the flames reach further
// left than the screen edge is from the centre at rest.
constexpr int kWheelLeft = -kRollTo, kWheelRight = 14;
constexpr double kHeatRolling = 1.15, kHeatIdle = 0.9;

// The wheel's pixels by offset from its centre. A pixel's distance and angle depend only on its offset (cx is a
// whole pixel, cy is fixed), so they are computed once, with the same double maths as before, and each frame only
// turns the wheel and flickers the flames.
struct WheelGeometry {
    struct Solid {
        int8_t dx, dy;  // rim, always on
    };
    struct Turning {
        int8_t dx, dy;
        bool window;  // rim window ring (5 sectors), else tread (16 notches)
        double th;
    };
    struct Flame {
        int8_t dx, dy;
        uint8_t tongue;
        double over;  // distance past the flame root (r - 14.5)
        double w;     // taper of the tongue at this angle, > 0
    };
    std::vector<Solid> solid;
    std::vector<Turning> turning;
    std::vector<Flame> flame;
};
WheelGeometry* s_wheel = nullptr;

void buildWheel(WheelGeometry& g) {
    for (int dy = -kWheelCy; dy < gfx::H - kWheelCy; dy++)
        for (int dx = kWheelLeft; dx <= kWheelRight; dx++) {
            const double r = std::hypot(dx, dy), th = std::atan2(dy, dx);
            const int8_t x = static_cast<int8_t>(dx), y = static_cast<int8_t>(dy);
            if (r <= 10.6) {
                if (r < 1.3) continue;  // hub hole
                if (r >= 3.4 && r <= 8.4)
                    g.turning.push_back({x, y, true, th});
                else
                    g.solid.push_back({x, y});
            } else if (r >= 12 && r <= 13.3) {
                g.turning.push_back({x, y, false, th});
            } else if (r >= 14.5) {
                double d = th - kPi;
                d = std::atan2(std::sin(d), std::cos(d));
                d -= 0.35 * (r - 14.5) / 10;
                for (int k = 0; k < 5; k++) {
                    const double w = 1 - std::fabs(d - kTongues[k][0]) / 0.3;
                    // A tongue is never longer than at the rolling heat with the flicker at its maximum of 1, so a
                    // pixel beyond that is never on.
                    if (w > 0 && r - 14.5 < kTongues[k][1] * kHeatRolling * w)
                        g.flame.push_back({x, y, static_cast<uint8_t>(k), r - 14.5, w});
                }
            }
        }
    g.solid.shrink_to_fit();
    g.turning.shrink_to_fit();
    g.flame.shrink_to_fit();
}

void releaseWheel() {
    delete s_wheel;
    s_wheel = nullptr;
}

// Fraction of the way through the pixel's sector of the turning wheel.
double sectorFrac(double th, double phi, int sectors) {
    return std::fmod(std::fmod((th - phi) / (2 * kPi) * sectors, 1.0) + 1, 1.0);
}

void wheelLogo(Frame& f, int cx, double phi, double t, double heat) {
    prepareSplash();
    double len[5];  // tongue length at the root for this heat and flicker
    for (int k = 0; k < 5; k++)
        len[k] = kTongues[k][1] * heat * (0.72 + 0.28 * std::sin(t * 0.013 * (1 + 0.37 * k) + k * 1.7));
    const auto visible = [&](int dx) { return cx + dx >= 0 && cx + dx < gfx::W; };
    for (const auto& p : s_wheel->solid)
        if (visible(p.dx)) f.px(cx + p.dx, kWheelCy + p.dy, 1);
    for (const auto& p : s_wheel->turning) {
        if (!visible(p.dx)) continue;
        const double fr = sectorFrac(p.th, phi, p.window ? 5 : 16);
        if (p.window ? !(fr > 0.18 && fr < 0.82) : fr >= 0.2) f.px(cx + p.dx, kWheelCy + p.dy, 1);
    }
    for (const auto& p : s_wheel->flame)
        if (visible(p.dx) && p.over < len[p.tongue] * p.w) f.px(cx + p.dx, kWheelCy + p.dy, 1);
}

}  // namespace

int wrapText(const char* s, int w, void (*row)(void* ctx, int i, const char* text), void* ctx) {
    constexpr size_t kMax = 160;  // a RadioText as the display draws it is at most 128 bytes
    char cur[kMax] = "", word[kMax], t[2 * kMax + 1];
    int n = 0;
    const char* p = s;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != ' ') e++;
        const size_t wl = static_cast<size_t>(e - p) < kMax - 1 ? static_cast<size_t>(e - p) : kMax - 1;
        std::memcpy(word, p, wl);
        word[wl] = 0;
        p = e;
        snprintf(t, sizeof t, cur[0] ? "%s %s" : "%s%s", cur, word);
        if (gfx::textW(t) <= w) {
            copy(cur, sizeof cur, t);
            continue;
        }
        if (cur[0]) row(ctx, n++, cur);
        while (gfx::textW(word) > w) {
            // The longest start of the word that fits, at least one character.
            const char* end = word;
            gfx::nextCp(end);
            for (;;) {
                const char* next = end;
                if (!gfx::nextCp(next)) break;
                const size_t k = static_cast<size_t>(next - word);
                std::memcpy(t, word, k);
                t[k] = 0;
                if (gfx::textW(t) > w) break;
                end = next;
            }
            const size_t k = static_cast<size_t>(end - word);
            std::memcpy(t, word, k);
            t[k] = 0;
            row(ctx, n++, t);
            std::memmove(word, end, std::strlen(end) + 1);
        }
        copy(cur, sizeof cur, word);
    }
    if (cur[0]) row(ctx, n++, cur);
    return n;
}

void prepareSplash() {
    if (s_wheel) return;
    s_wheel = new WheelGeometry;
    buildWheel(*s_wheel);
}

// ---- state transitions ----

void Ui::begin(uint32_t now) {
    view_ = View::Splash;
    splashStart_ = now;
    lastInput_ = now;
    depth_ = 0;
}

void Ui::endSplash(Output& out, uint32_t now) {
    releaseWheel();
    goHome();
    out.push(Cmd::SplashEnd);
    if (result_[0]) {
        toast(result_, now, kResultMs);
        result_[0] = 0;
    }
}

void Ui::goHome() {
    view_ = View::Home;
    depth_ = 0;
}

void Ui::popTo() {
    if (depth_ == 0) return goHome();
    list_ = stack_[--depth_];
    view_ = View::List;
}

void Ui::toast(const char* text, uint32_t now, uint32_t ms) {
    copy(toast_, sizeof toast_, text);
    toastUntil_ = now + ms;
}

void Ui::result(const char* text, uint32_t now) {
    if (view_ == View::Splash)
        copy(result_, sizeof result_, text);
    else
        toast(text, now, kResultMs);
}

void Ui::confirmInstall(uint32_t now) {
    view_ = View::Confirm;
    depth_ = 0;
    confirmStart_ = now;
}

void Ui::message(const char* text) {
    copy(message_, sizeof message_, text);
    view_ = View::Message;
    depth_ = 0;
}

void Ui::build(ListId id, const State& s, const Status& st) {
    count_ = 0;
    gutterCount_ = 0;
    cur_ = 0;
    auto add = [&](const char* t, const char* r, Act act, int arg = 0, const char* mid = "") {
        if (count_ >= kMaxItems) return;
        Item& it = items_[count_++];
        copy(it.t, sizeof it.t, t);
        copy(it.r, sizeof it.r, r);
        copy(it.mid, sizeof it.mid, mid);
        it.act = act;
        it.arg = arg;
        it.skip = false;
        it.mark = false;
    };
    char buf[24];
    auto tr = [&](Str k) { return i18n::tr(s.lang, k); };
    switch (id) {
        case ListId::Main: {
            snprintf(buf, sizeof buf, "%d", s.volume);
            add(tr(Str::volume), s.muted ? tr(Str::val_mute) : buf, Act::Adjust, static_cast<int>(AdjustId::Volume));
            add(tr(Str::band), radio::bandLabel(s.hz, s.mode), Act::Go, static_cast<int>(ListId::Bands));
            snprintf(buf, sizeof buf, "%d", s.presetCount);
            add(tr(Str::presets), buf, Act::Go, static_cast<int>(ListId::Presets));
            add(tr(Str::mode), radio::modeName(s.mode), Act::Go, static_cast<int>(ListId::Modes));
            add(tr(Str::step), radio::currentStep(s).label, Act::Go, static_cast<int>(ListId::Steps));
            add(tr(Str::bandwidth), radio::currentBandwidth(s), Act::Go, static_cast<int>(ListId::Bandwidth));
            add(tr(Str::seek), "", Act::Go, static_cast<int>(ListId::Seek));
            add(tr(Str::station), "", Act::Station);
            add(tr(Str::settings), "", Act::Go, static_cast<int>(ListId::Settings));
            gutter_[0] = {s.muted ? gfx::Icon::Muted : gfx::Icon::Volume, Act::ToggleMute, 0};
            gutterCount_ = 1;
            break;
        }
        case ListId::Bands: {
            const int b = radio::bandIndex(s.hz, s.mode);
            cur_ = b > 0 ? b : 0;
            for (int i = 0; i < radio::kBandCount; i++) {
                const radio::Band& band = radio::kBands[i];
                char range[16];
                radio::rangeText(band, range, sizeof range);
                const bool ssb = band.mode == Mode::LSB || band.mode == Mode::USB;
                add(band.name, ssb ? radio::modeName(band.mode) : "", Act::Band, i, range);
            }
            break;
        }
        case ListId::Modes:
            if (radio::isFm(s.mode)) {
                add("FM", tr(Str::only_fm), Act::None);
                break;
            }
            add("AM", "", Act::Mode, static_cast<int>(Mode::AM));
            if (st.ssb) {
                add("LSB", tr(Str::lower_ssb), Act::Mode, static_cast<int>(Mode::LSB));
                add("USB", tr(Str::upper_ssb), Act::Mode, static_cast<int>(Mode::USB));
            }
            for (int i = 0; i < count_; i++)
                if (items_[i].arg == static_cast<int>(s.mode)) cur_ = i;
            break;
        case ListId::Presets: {
            for (int i = 0; i < s.presetCount; i++) {
                const radio::Preset& p = s.presets[i];
                char t[24], r[16];
                snprintf(t, sizeof t, "%d %s", i + 1, p.name);
                radio::presetFreqText(p, r, sizeof r);
                add(t, r, Act::Preset, i);
            }
            for (int i = 0; i < s.presetCount; i++)
                if (s.presets[i].hz == s.hz && s.presets[i].mode == s.mode) {
                    cur_ = i;
                    break;
                }
            break;
        }
        case ListId::Steps: {
            int n;
            const radio::Step* stp = radio::steps(radio::family(s.mode), &n);
            for (int i = 0; i < n; i++) add(stp[i].label, "", Act::Step, i);
            cur_ = s.stepIdx[radio::family(s.mode)];
            break;
        }
        case ListId::Bandwidth: {
            int n;
            const char* const* bw = radio::bandwidths(radio::family(s.mode), &n);
            for (int i = 0; i < n; i++) add(bw[i], "", Act::Bandwidth, i);
            cur_ = s.bwIdx[radio::family(s.mode)];
            break;
        }
        case ListId::Seek:
            add(tr(Str::seek_up), "", Act::Seek, 1);
            add(tr(Str::seek_down), "", Act::Seek, -1);
            break;
        case ListId::Wifi:
            add(tr(Str::status), st.joined ? "radio.local" : tr(Str::not_joined), Act::Info, static_cast<int>(InfoId::Wifi));
            for (int i = 0; i < st.netCount; i++) {
                char bars[4], r[16];
                barsText(st.nets[i].bars, bars);
                if (st.joined && std::strcmp(st.nets[i].ssid, st.ssid) == 0)
                    snprintf(r, sizeof r, "✓ %s", bars);
                else
                    snprintf(r, sizeof r, "%s", bars);
                add(st.nets[i].ssid, r, Act::Join, i);
            }
            break;
        case ListId::Settings:
            add(tr(Str::wifi), st.joined ? st.ssid : tr(Str::not_joined), Act::Go, static_cast<int>(ListId::Wifi));
            add(tr(Str::agc), s.agc ? tr(Str::on) : tr(Str::off), Act::ToggleAgc);
            snprintf(buf, sizeof buf, "%d", s.contrast);
            add(tr(Str::contrast), buf, Act::Adjust, static_cast<int>(AdjustId::Contrast));
            snprintf(buf, sizeof buf, "%d", s.light);
            add(tr(Str::light), st.lamp == radio::Lamp::Off ? tr(Str::off) : st.lamp == radio::Lamp::Sos ? "SOS" : buf,
                Act::Lamp);
            if (st.utcMs >= 0)
                clk::hhmm(st.utcMs, buf);
            else
                copy(buf, sizeof buf, "--:--");
            add(tr(Str::time), buf, Act::Go, static_cast<int>(ListId::Time));
            add(tr(Str::guide), guideLoaded(st) ? st.guide->season : "", Act::Go, static_cast<int>(ListId::GuideSettings));
            add(tr(Str::language), i18n::kLangName[s.lang < i18n::kLangCount ? s.lang : 0], Act::Go,
                static_cast<int>(ListId::Language));
            add(tr(Str::about), "", Act::Info, static_cast<int>(InfoId::About));
            break;
        case ListId::Language:
            for (int i = 0; i < i18n::kLangCount; i++) add(i18n::kLangName[i], "", Act::Language, i);
            cur_ = s.lang < i18n::kLangCount ? s.lang : 0;
            break;
        // Station in AM, LSB and USB: the frequency's guide entries, the on-air ones marked, each with its time window
        // or irr; the name cut to leave 4 px before it.
        case ListId::Guide:
            for (int i = 0; st.hits && i < st.hits->n; i++) {
                const guide::Hit& h = st.hits->hit[i];
                char r[16], name[guide::kStrMax + 1], cut[sizeof items_[0].t];
                if (h.e.days & guide::kIrregular)
                    copy(r, sizeof r, tr(Str::irr));
                else
                    windowText(h.e, r);
                pipText(h.name, name, sizeof name);
                cutText(name, 124 - gfx::textW(r) - 4 - 17, cut, sizeof cut);
                add(cut, r, Act::GuideEntry, i);
                items_[count_ - 1].mark = h.onAir;
            }
            break;
        // Settings > Guide: the loaded table, not selectable, and Update guide, which needs a configured network.
        case ListId::GuideSettings: {
            char label[16] = "";
            if (guideLoaded(st)) {
                snprintf(label, sizeof label, "EiBi %s", st.guide->season);
                snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(st.guide->entries));
            } else {
                buf[0] = 0;
            }
            add(guideLoaded(st) ? label : tr(Str::no_guide), buf, Act::None);
            items_[count_ - 1].skip = true;
            add(tr(Str::update_guide), st.netSaved ? "" : tr(Str::no_network), Act::GuideUpdate);
            cur_ = 1;
            break;
        }
        // Time: the sync switch, the server (cut to fit its row) and the age of the last sync, not selectable.
        case ListId::Time: {
            add(tr(Str::time_sync), s.timeSync ? tr(Str::on) : tr(Str::off), Act::ToggleTimeSync);
            char server[sizeof items_[0].r];
            cutText(s.ntpServer, 113 - 4 - gfx::textW(tr(Str::server)), server, sizeof server);
            add(tr(Str::server), server, Act::Server);
            if (st.syncAgeMs >= 0)
                ageText(s.lang, st.syncAgeMs, buf, sizeof buf);
            else
                copy(buf, sizeof buf, tr(Str::never));
            add(tr(Str::last_sync), buf, Act::None);
            items_[count_ - 1].skip = true;
            break;
        }
    }
}

void Ui::openList(ListId id, bool push, const State& s, const Status& st) {
    if (push && depth_ < 4) stack_[depth_++] = list_;
    build(id, s, st);
    const int cur = count_ ? cur_ : -1;
    list_ = {id, cur, clampi(cur - 1 < count_ - 4 ? cur - 1 : count_ - 4, 0, 1 << 20)};
    view_ = View::List;
}

void Ui::scrollTo() {
    ListState& l = list_;
    if (l.cursor == -1) {
        l.top = 0;
        return;
    }
    if (l.cursor < -1) return;
    if (l.cursor < l.top) l.top = l.cursor;
    if (l.cursor > l.top + 3) l.top = l.cursor - 3;
    const int maxTop = count_ - 4 > 0 ? count_ - 4 : 0;
    l.top = clampi(l.top, 0, maxTop);
}

int Ui::adjustValue(const State& s) const { return adjust_ == AdjustId::Volume ? s.volume : s.contrast; }

void Ui::adjustDone() {
    if (adjust_ == AdjustId::Volume) return goHome();
    popTo();
}

void Ui::stopSeek(const Status& st, Output& out) {
    if (st.seeking) out.push(Cmd::SeekStop);
}

void Ui::openLamp(const Status& st, Output& out) {
    if (view_ == View::List && depth_ < 4) stack_[depth_++] = list_;
    if (st.lamp == radio::Lamp::Off) out.push(Cmd::Lamp, static_cast<int>(radio::Lamp::On));
    view_ = View::Lamp;
}

void Ui::startSeek(int dir, const State& s, Output& out, uint32_t now) {
    goHome();
    if (radio::family(s.mode) == radio::kFamSSB) return toast(i18n::tr(s.lang, Str::toast_no_seek), now);
    out.push(Cmd::Seek, dir);
}

void Ui::activate(const State& s, const Status& st, Output& out, uint32_t now) {
    Act act;
    int arg;
    if (list_.cursor < -1) {
        const Gutter& g = gutter_[-list_.cursor - 2];
        act = g.act;
        arg = g.arg;
    } else {
        const Item& it = items_[list_.cursor];
        act = it.act;
        arg = it.arg;
    }
    switch (act) {
        case Act::None: return;
        case Act::Go:
            if (static_cast<ListId>(arg) == ListId::Wifi) out.push(Cmd::Scan);
            return openList(static_cast<ListId>(arg), true, s, st);
        case Act::Adjust:
            if (depth_ < 4) stack_[depth_++] = list_;
            adjust_ = static_cast<AdjustId>(arg);
            view_ = View::Adjust;
            return;
        case Act::Info:
            if (depth_ < 4) stack_[depth_++] = list_;
            info_ = static_cast<InfoId>(arg);
            view_ = View::Info;
            return;
        case Act::ToggleAgc: return out.push(Cmd::Agc, !s.agc);
        case Act::ToggleMute: return out.push(Cmd::Mute, !s.muted);
        case Act::Band: out.push(Cmd::Band, arg); return goHome();
        case Act::Mode: out.push(Cmd::SetMode, arg); return goHome();
        case Act::Preset:
            out.push(Cmd::TunePreset, arg);
            return goHome();
        case Act::Step: out.push(Cmd::Step, arg); return goHome();
        case Act::Bandwidth: out.push(Cmd::Bandwidth, arg); return goHome();
        case Act::Seek: return startSeek(arg, s, out, now);
        case Act::Join: return startEntry(st.nets[arg].ssid, "", 0);
        case Act::Server: return startEntry(nullptr, s.ntpServer, kRingLen - 1);  // filled in, on OK
        case Act::ToggleTimeSync: return out.push(Cmd::TimeSync, !s.timeSync);
        case Act::GuideEntry: {
            if (!st.hits || arg >= st.hits->n) return;
            const guide::Hit& h = st.hits->hit[arg];
            guideEntry_.e = h.e;
            copy(guideEntry_.name, sizeof guideEntry_.name, h.name);
            copy(guideEntry_.lang, sizeof guideEntry_.lang, h.lang);
            copy(guideEntry_.target, sizeof guideEntry_.target, h.target);
            if (depth_ < 4) stack_[depth_++] = list_;
            info_ = InfoId::GuideEntry;
            view_ = View::Info;
            return;
        }
        case Act::GuideUpdate:
            if (st.netSaved) out.push(Cmd::GuideUpdate);
            return;
        case Act::Station:
            if (!radio::isFm(s.mode)) return openList(ListId::Guide, true, s, st);
            if (depth_ < 4) stack_[depth_++] = list_;
            view_ = View::Station;
            stationTop_ = 0;
            stationSeq_ = st.rtSeq;
            return;
        case Act::Lamp: return openLamp(st, out);
        case Act::Language: out.push(Cmd::Language, arg); return popTo();
    }
}

// Text entry for a WiFi password (`ssid`) or, with no ssid, the NTP server.
void Ui::startEntry(const char* ssid, const char* value, int idx) {
    if (depth_ < 4) stack_[depth_++] = list_;
    entryServer_ = !ssid;
    copy(entrySsid_, sizeof entrySsid_, ssid ? ssid : "");
    copy(entry_, sizeof entry_, value);
    entryIdx_ = idx;
    view_ = View::Entry;
}

void Ui::event(const Event& e, uint32_t now, const State& s, const Status& st, Output& out) {
    if (view_ == View::Message) return;
    // The side button's hold does nothing on the splash, a message or the confirm screen.
    if (e.type == EventType::SideHold && (view_ == View::Splash || view_ == View::Confirm)) return;
    lastInput_ = now;
    if (view_ == View::Splash) return endSplash(out, now);
    stopSeek(st, out);
    if (view_ == View::List) build(list_.id, s, st);
    const int d = e.n;

    switch (e.type) {
        case EventType::Turn:
            switch (view_) {
                case View::Home:
                    out.push(Cmd::Tune, radio::clampHz(s.hz + d * radio::currentStep(s).hz, s.mode));
                    break;
                case View::List: {
                    // Ring order: the selectable entries, then the gutter from the bottom slot up to Back.
                    int ring[kMaxItems + 4], len = 0;
                    for (int k = 0; k < count_; k++)
                        if (!items_[k].skip) ring[len++] = k;
                    for (int k = gutterCount_; k >= 0; k--) ring[len++] = -(k + 1);
                    int i = -1;
                    for (int k = 0; k < len; k++)
                        if (ring[k] == list_.cursor) i = k;
                    list_.cursor = ring[mod(i + d, len)];
                    scrollTo();
                    break;
                }
                case View::Adjust:
                    if (adjust_ == AdjustId::Volume)
                        out.push(Cmd::Volume, clampi(s.volume + d, 0, radio::kVolumeMax));
                    else
                        out.push(Cmd::Contrast, clampi(s.contrast + d, radio::kContrastMin, radio::kContrastMax));
                    break;
                case View::Lamp: out.push(Cmd::Light, clampi(s.light + d, radio::kLightMin, radio::kLightMax)); break;
                case View::Entry: entryIdx_ = mod(entryIdx_ + d, kRingLen); break;
                case View::Station: {
                    syncStation(st);
                    const int rows = wrapText(st.rt, kRtRowW, [](void*, int, const char*) {}, nullptr);
                    stationTop_ = clampi(stationTop_ + d, 0, rows - 3 > 0 ? rows - 3 : 0);
                    break;
                }
                default: break;
            }
            return;

        case EventType::Press:
            switch (view_) {
                case View::Home: return openList(ListId::Main, false, s, st);
                case View::Info:
                case View::Station: return popTo();
                case View::Adjust: return adjustDone();
                // The lamp screen's press: on, SOS, off; off closes the screen.
                case View::Lamp:
                    if (st.lamp != radio::Lamp::Sos) return out.push(Cmd::Lamp, static_cast<int>(radio::Lamp::Sos));
                    out.push(Cmd::Lamp, static_cast<int>(radio::Lamp::Off));
                    return popTo();
                case View::Confirm:
                    out.push(Cmd::Install);
                    return message(i18n::tr(s.lang, Str::installing));
                case View::Entry: {
                    char buf[2];
                    const char* k = ringKey(entryIdx_, buf);
                    const size_t len = std::strlen(entry_);
                    if (std::strcmp(k, "OK") == 0 && entryServer_) {
                        copy(server_, sizeof server_, entry_);
                        out.push(Cmd::NtpServer);
                        return popTo();
                    }
                    if (std::strcmp(k, "OK") == 0) {
                        copy(joinSsid_, sizeof joinSsid_, entrySsid_);
                        copy(joinPwd_, sizeof joinPwd_, entry_);
                        out.push(Cmd::Join);
                        char t[96];
                        snprintf(t, sizeof t, i18n::tr(s.lang, Str::toast_joining), entrySsid_);
                        toast(t, now);
                        return goHome();
                    }
                    if (std::strcmp(k, "DEL") == 0) {
                        if (len) entry_[len - 1] = 0;
                        return;
                    }
                    if (len < static_cast<size_t>(entryServer_ ? radio::kNtpServerMax : kPasswordMax)) {
                        entry_[len] = std::strcmp(k, "SPACE") == 0 ? ' ' : k[0];
                        entry_[len + 1] = 0;
                    }
                    return;
                }
                case View::List:
                    if (list_.cursor == -1) return popTo();
                    return activate(s, st, out, now);
                default: return;
            }

        case EventType::LongPress:
            if (view_ == View::Home) {
                if (s.presetCount >= radio::kMaxPresets) return toast(i18n::tr(s.lang, Str::toast_full), now);
                out.push(Cmd::SavePreset);
                char t[96];
                snprintf(t, sizeof t, i18n::tr(s.lang, Str::toast_saved), s.presetCount + 1);
                return toast(t, now);
            }
            if (view_ == View::Confirm) {
                out.push(Cmd::CancelInstall);
                toast(i18n::tr(s.lang, Str::toast_cancelled), now);
            }
            return goHome();

        case EventType::HoldTurn:
            switch (view_) {
                case View::Home:
                    out.push(Cmd::Volume, clampi(s.volume + d, 0, radio::kVolumeMax));
                    volUntil_ = now + kToastMs;
                    break;
                case View::List: {
                    int c = (list_.cursor > 0 ? list_.cursor : 0) + d * 4;
                    c = c > count_ - 1 ? count_ - 1 : c < 0 ? 0 : c;
                    while (c > 0 && items_[c].skip) c--;
                    list_.cursor = c;
                    scrollTo();
                    break;
                }
                case View::Entry: {
                    const int g = groupOf(entryIdx_);
                    entryIdx_ = kGroupStart[mod(g + d, kGroups)];
                    break;
                }
                default: break;
            }
            return;

        // A short press of the side button is Back.
        case EventType::Side:
            if (view_ == View::Confirm) {
                out.push(Cmd::CancelInstall);
                toast(i18n::tr(s.lang, Str::toast_cancelled), now);
                return goHome();
            }
            if (view_ == View::List || view_ == View::Info || view_ == View::Entry || view_ == View::Station ||
                view_ == View::Lamp)
                return popTo();
            if (view_ == View::Adjust) return adjustDone();
            return;

        // Its hold turns the lamp on with the lamp screen, or off, closing the lamp screen as Back.
        case EventType::SideHold:
            if (st.lamp == radio::Lamp::Off) return openLamp(st, out);
            out.push(Cmd::Lamp, static_cast<int>(radio::Lamp::Off));
            if (view_ == View::Lamp) popTo();
            return;
    }
}

void Ui::tick(uint32_t now, const State& s, const Status&, Output& out) {
    if (view_ == View::Splash && now - splashStart_ > kSplashMs) endSplash(out, now);
    if ((view_ == View::List || view_ == View::Adjust || view_ == View::Info || view_ == View::Lamp) &&
        now - lastInput_ > kMenuTimeoutMs)
        goHome();
    if (view_ == View::Confirm && now - confirmStart_ > kConfirmTimeoutMs) {
        out.push(Cmd::CancelInstall);
        toast(i18n::tr(s.lang, Str::toast_cancelled), now);
        goHome();
    }
}

// ---- drawing ----

void Ui::render(Frame& f, uint32_t now, const State& s, const Status& st) {
    f.clear();
    switch (view_) {
        case View::Splash: return drawSplash(f, now - splashStart_);
        case View::Home: return drawHome(f, now, s, st);
        case View::List: return drawList(f, s, st);
        case View::Adjust: return drawAdjust(f, s);
        case View::Lamp: return drawLamp(f, s, st);
        case View::Info: return drawInfo(f, s, st);
        case View::Entry: return drawEntry(f, s);
        case View::Confirm: return drawConfirm(f, s);
        case View::Message: return drawMessage(f);
        case View::Station: return drawStation(f, s, st);
    }
}

void Ui::drawSplash(Frame& f, uint32_t tms) {
    const double t = tms;
    const double k = t / 900 < 1 ? t / 900 : 1, ease = 1 - std::pow(1 - k, 3);
    const int cx = jsRoundI(kRollFrom + ease * (kRollTo - kRollFrom));
    const double phi = (cx - kRollFrom) / 13.0 + (t - 900 > 0 ? t - 900 : 0) * 0.004;
    wheelLogo(f, cx, phi, t, k < 1 ? kHeatRolling : kHeatIdle);
    if (t > 800) {
        const int n = clampi(static_cast<int>(std::floor((t - 800) / 45)), 0, 10);
        char word[11];
        std::memcpy(word, "HOVERBOARD", n);
        word[n] = 0;
        gfx::text(f, 40, 1, word);
    }
    if (t > 1250) {
        gfx::text2x(f, 40, 10, "HAVOC");
        const int reveal = 40 + clampi(static_cast<int>(std::floor((t - 1250) / 6)), 0, 52);
        f.fill(reveal, 10, 128 - reveal, 13, 0);
    }
    if (t > 1700) gfx::textR(f, 128, 24, "radio");
}

// The name on home: the on-air names of the lookup, each once, in its order, take turns every 3 s. False in FM,
// without the clock or with nothing on air.
static bool onAirName(uint32_t now, const State& s, const Status& st, char* out, size_t n) {
    if (radio::isFm(s.mode) || st.utcMs < 0 || !st.hits) return false;
    const char* names[guide::kMaxHits];
    int count = 0;
    for (int i = 0; i < st.hits->n; i++) {
        const guide::Hit& h = st.hits->hit[i];
        if (!h.onAir) continue;
        bool dup = false;
        for (int k = 0; k < count; k++) dup |= !std::strcmp(names[k], h.name);
        if (!dup) names[count++] = h.name;
    }
    if (!count) return false;
    pipText(names[now / kNameTurnMs % count], out, n);
    return true;
}

void Ui::drawHome(Frame& f, uint32_t now, const State& s, const Status& st) {
    char fs[16];
    const char* unit;
    radio::freqText(s, fs, sizeof fs, &unit);
    const bool vol = now < volUntil_, toasting = toast_[0] && now < toastUntil_;
    const char* band = radio::bandLabel(s.hz, s.mode);
    if (vol) {
        gfx::icon(f, 0, 0, gfx::Icon::Volume);
        const int w = jsRoundI(s.volume / 63.0 * 90);
        f.fill(10, 2, 90, 4, 0);
        for (int i = 10; i < 100; i += 2) f.px(i, 4, 1);
        f.fill(10, 1, w, 5, 1);
        char v[8];
        snprintf(v, sizeof v, "%d", s.volume);
        gfx::textR(f, 128, 0, v);
    } else if (toasting) {
        f.fill(0, 0, 128, 8, 1);
        gfx::text(f, 1, 0, toast_, 0);
    } else if (st.seeking) {
        char t[64];
        snprintf(t, sizeof t, "%s  %s %s", band, i18n::tr(s.lang, Str::seek), st.seekDir > 0 ? "\u25b6" : "\u25c0");
        gfx::text(f, 0, 0, t);
    } else {
        int x;
        char t[48], name[guide::kStrMax + 1];
        snprintf(t, sizeof t, "%s %s", i18n::tr(s.lang, Str::home_step), radio::currentStep(s).label);
        if (onAirName(now, s, st, name, sizeof name)) {
            // In AM, LSB and USB the on-air name from the guide replaces the band name and mode, cut before the Mute
            // flag or the signal bars; the step follows when it fits.
            const int lim = s.muted ? 78 : 104;
            char cut[sizeof name + 4];
            cutText(name, lim, cut, sizeof cut);
            x = gfx::text(f, 0, 0, cut);
            if (x + 5 + gfx::textW(t) <= lim) gfx::text(f, x + 5, 0, t);
        } else {
            // On FM the station's RDS name, when known, replaces the band name and mode.
            if (radio::isFm(s.mode) && st.ps[0]) {
                x = gfx::text(f, 0, 0, st.ps);
            } else {
                x = gfx::text(f, 0, 0, band);
                x = gfx::text(f, x + 5, 0, radio::modeName(s.mode));
            }
            // The step yields to the Mute flag when they would overlap.
            if (!s.muted || x + 5 + gfx::textW(t) < 80) gfx::text(f, x + 5, 0, t);
        }
        if (s.muted) {
            const char* flag = i18n::tr(s.lang, Str::home_mute);
            const int fw = gfx::textW(flag) + 3;
            f.fill(80, 0, fw > 22 ? fw : 22, 8, 1);
            gfx::text(f, 82, 0, flag, 0);
        }
    }
    if (!vol && !toasting) {
        for (int i = 0; i < 5; i++) {
            const int h = 2 + i + (i > 2 ? 1 : 0), x = 108 + i * 4;
            if (i < st.bars)
                f.fill(x, 7 - h, 3, h, 1);
            else
                f.px(x + 1, 6, 1);
        }
    }
    const int b = radio::bandIndex(s.hz, s.mode);
    if (b >= 0) {
        const radio::Band& bd = radio::kBands[b];
        for (int i = 0; i < 128; i += 4) f.px(i, 11, 1);
        const int p = jsRoundI((s.hz / 1000.0 - bd.lo) / (bd.hi - bd.lo) * 125);
        f.fill(p, 9, 3, 5, 1);
    }
    const int fx = 114 - gfx::bigW(fs);
    gfx::big(f, fx, 14, fs);
    gfx::textR(f, 128, 24, unit);
    // UTC at the bottom left once the clock is set, when the frequency leaves 2 px for it.
    if (st.utcMs >= 0) {
        char t[8];
        clk::hhmm(st.utcMs, t);
        std::strcat(t, "Z");
        if (gfx::textW(t) + 2 <= fx) gfx::text(f, 0, 24, t);
    }
}

void Ui::drawGutter(Frame& f, int cursor, bool withGutter, const State&) {
    if (cursor == -1) {
        f.fill(0, 0, 8, 8, 1);
        gfx::text(f, 2, 0, "◀", 0);
    } else {
        gfx::text(f, 2, 0, "◀", 1);
    }
    if (!withGutter) return;
    for (int k = 0; k < gutterCount_; k++) {
        const int y = (k + 1) * 8;
        const bool sel = cursor == -(k + 2);
        if (sel) f.fill(0, y, 8, 8, 1);
        gfx::icon(f, 0, y, gutter_[k].icon, sel ? 0 : 1);
    }
}

void Ui::drawList(Frame& f, const State& s, const Status& st) {
    build(list_.id, s, st);
    const int n = count_;
    drawGutter(f, list_.cursor, true, s);
    const bool guideRows = list_.id == ListId::Guide;
    if (guideRows && !n) {
        if (!guideLoaded(st)) {
            gfx::text(f, 10, 12, i18n::tr(s.lang, Str::no_guide));
        } else {
            char num[16], t[96];
            snprintf(num, sizeof num, "%ld", static_cast<long>((s.hz + 500) / 1000));  // whole kHz
            snprintf(t, sizeof t, i18n::tr(s.lang, Str::nothing_listed), num);
            gfx::text(f, 10, 12, t);
        }
    }
    for (int r = 0; r < 4; r++) {
        const int i = list_.top + r;
        if (i >= n) break;
        const bool sel = i == list_.cursor;
        const int y = r * 8, c = sel ? 0 : 1;
        const Item& it = items_[i];
        if (sel) f.fill(9, y, 117, 8, 1);
        if (guideRows) {
            if (it.mark) gfx::text(f, 11, y, "▶", c);
            gfx::text(f, 17, y, it.t, c);
            gfx::textR(f, 124, y, it.r, c);
            continue;
        }
        gfx::text(f, 11, y, it.t, c);
        if (it.mid[0]) gfx::text(f, 32, y, it.mid, c);
        if (it.r[0]) gfx::textR(f, 124, y, it.r, c);
    }
    if (n > 4) {
        for (int y = 0; y < 32; y += 2) f.px(127, y, 1);
        const int th = jsRoundI(32.0 * 4 / n) > 3 ? jsRoundI(32.0 * 4 / n) : 3;
        const int ty = jsRoundI(static_cast<double>(32 - th) * list_.top / (n - 4));
        f.fill(127, ty, 1, th, 1);
    }
}

void Ui::drawAdjust(Frame& f, const State& s) {
    const bool vol = adjust_ == AdjustId::Volume;
    const char* title = i18n::tr(s.lang, vol ? Str::volume : Str::contrast);
    const int lo = vol ? 0 : radio::kContrastMin, hi = vol ? radio::kVolumeMax : radio::kContrastMax;
    drawGutter(f, 0, false, s);
    gfx::text(f, 10, 0, title);
    const int v = adjustValue(s), w = jsRoundI(static_cast<double>(v - lo) / (hi - lo) * 106);
    for (int i = 10; i < 116; i += 2) f.px(i, 17, 1);
    f.fill(10, 13, w, 9, 1);
    char t[12];
    snprintf(t, sizeof t, "%d/%d", v, hi);
    gfx::textR(f, 128, 24, t);
}

// The lamp screen: as an adjust screen, with what the next press does at the bottom left.
void Ui::drawLamp(Frame& f, const State& s, const Status& st) {
    const bool sos = st.lamp == radio::Lamp::Sos;
    drawGutter(f, 0, false, s);
    gfx::text(f, 10, 0, i18n::tr(s.lang, sos ? Str::light_sos : Str::light));
    for (int i = 10; i < 116; i += 2) f.px(i, 17, 1);
    f.fill(10, 13, jsRoundI(static_cast<double>(s.light) / radio::kLightMax * 106), 9, 1);
    char t[12];
    snprintf(t, sizeof t, "%d/%d", s.light, radio::kLightMax);
    gfx::textR(f, 128, 24, t);
    gfx::text(f, 10, 24, i18n::tr(s.lang, sos ? Str::press_off : Str::press_sos));
}

void Ui::drawInfo(Frame& f, const State& s, const Status& st) {
    char lines[4][96];
    int n = 3;
    if (info_ == InfoId::Wifi) {
        if (st.joined) {
            snprintf(lines[0], sizeof lines[0], i18n::tr(s.lang, Str::joined_s), st.ssid);
            snprintf(lines[1], sizeof lines[1], "radio.local");
            snprintf(lines[2], sizeof lines[2], "%s", st.ip);
        } else {
            snprintf(lines[0], sizeof lines[0], "%s", i18n::tr(s.lang, Str::not_joined_cap));
            snprintf(lines[1], sizeof lines[1], "%s", i18n::tr(s.lang, Str::access_point));
            snprintf(lines[2], sizeof lines[2], "Havoc-Radio");
        }
    } else if (info_ == InfoId::GuideEntry) {
        // The entry's details: name, window with the days, language and target as EiBi gives them.
        const GuideEntry& g = guideEntry_;
        char text[guide::kStrMax + 1], w[16], days[24], line[160];
        pipText(g.name, text, sizeof text);
        cutText(text, 118, lines[0], sizeof lines[0]);
        windowText(g.e, w);
        snprintf(line, sizeof line, "%s UTC %s", w, daysLabel(s.lang, g.e.days, days));
        cutText(line, 118, lines[1], sizeof lines[1]);
        pipText(g.lang, text, sizeof text);
        snprintf(line, sizeof line, i18n::tr(s.lang, Str::language_s), text);
        cutText(line, 118, lines[2], sizeof lines[2]);
        pipText(g.target, text, sizeof text);
        snprintf(line, sizeof line, i18n::tr(s.lang, Str::target_s), text);
        cutText(line, 118, lines[3], sizeof lines[3]);
        n = 4;
    } else {
        snprintf(lines[0], sizeof lines[0], "Havoc Radio %s", st.version);
        snprintf(lines[1], sizeof lines[1], "IP %s", st.ip);
        snprintf(lines[2], sizeof lines[2], i18n::tr(s.lang, Str::battery_d), st.battery);
        if (st.utcMs >= 0) {
            char t[6];
            clk::hhmm(st.utcMs, t);
            snprintf(lines[3], sizeof lines[3], "UTC %s", t);
        } else {
            snprintf(lines[3], sizeof lines[3], "%s", i18n::tr(s.lang, Str::utc_not_set));
        }
        n = 4;
    }
    drawGutter(f, -1, false, s);
    // Three lines sit 4 px down; four fill the screen from the top.
    for (int i = 0; i < n; i++) gfx::text(f, 10, i * 8 + (n > 3 ? 0 : 4), lines[i]);
}

void Ui::syncStation(const Status& st) {
    if (stationSeq_ == st.rtSeq) return;
    stationSeq_ = st.rtSeq;
    stationTop_ = 0;
}

void Ui::drawStation(Frame& f, const State& s, const Status& st) {
    drawGutter(f, -1, false, s);
    if (!st.rdsSync) {
        gfx::text(f, 10, 0, i18n::tr(s.lang, Str::no_rds));
        return;
    }
    gfx::text(f, 10, 0, st.ps);
    if (st.pty >= 1 && st.pty <= 31)
        gfx::textR(f, 128, 0, i18n::tr(s.lang, static_cast<Str>(static_cast<int>(Str::pty_1) + st.pty - 1)));
    syncStation(st);
    struct Ctx {
        Frame& f;
        int top;
    } ctx{f, 0};
    const int rows = wrapText(st.rt, kRtRowW, [](void*, int, const char*) {}, nullptr);
    ctx.top = clampi(stationTop_, 0, rows - 3 > 0 ? rows - 3 : 0);
    wrapText(
        st.rt, kRtRowW,
        [](void* p, int i, const char* text) {
            Ctx& c = *static_cast<Ctx*>(p);
            if (i >= c.top && i < c.top + 3) gfx::text(c.f, 10, 8 + (i - c.top) * 8, text);
        },
        &ctx);
}

void Ui::drawEntry(Frame& f, const State& s) {
    char t[96], buf[2];
    if (entryServer_)
        copy(t, sizeof t, i18n::tr(s.lang, Str::ntp_server));
    else
        snprintf(t, sizeof t, i18n::tr(s.lang, Str::password_for), entrySsid_);
    gfx::text(f, 0, 0, t);
    const int g = groupOf(entryIdx_);
    gfx::textR(f, 128, 0, g == 4 ? i18n::tr(s.lang, Str::group_edit) : kGroupName[g]);
    const char* v = entry_;
    while (gfx::textW(v) > 118) v++;
    const int ex = gfx::text(f, 1, 8, v);
    f.fill(ex + (*v ? 1 : 0), 15, 4, 1, 1);
    for (int x = 0; x < 128; x += 2) f.px(x, 17, 1);
    auto cw = [](const char* l) { return gfx::textW(l) + 5; };
    const int mid = 64;
    const char* sel = ringLabel(entryIdx_, buf);
    const int sw = cw(sel);
    f.fill(mid - (sw >> 1), 21, sw, 10, 1);
    gfx::text(f, mid - (sw >> 1) + 3, 22, sel, 0);
    int x = mid + (sw >> 1) + 1;
    for (int i = 1; x < 128; i++) {
        const char* l = ringLabel(entryIdx_ + i, buf);
        gfx::text(f, x + 2, 22, l);
        x += cw(l);
    }
    x = mid - (sw >> 1) - 1;
    for (int i = 1; x > 0; i++) {
        const char* l = ringLabel(entryIdx_ - i, buf);
        x -= cw(l);
        gfx::text(f, x + 3, 22, l);
    }
}

void Ui::drawConfirm(Frame& f, const State& s) {
    const char* const lines[] = {i18n::tr(s.lang, Str::confirm_title), i18n::tr(s.lang, Str::confirm_press),
                                 i18n::tr(s.lang, Str::confirm_long)};
    for (int i = 0; i < 3; i++) gfx::text(f, 10, i * 8 + 4, lines[i]);
}

// Download mode: the title, the step, during the download a bar by bytes with KB at the right, and Back: cancel.
void drawDownload(Frame& f, int lang, const Download& d) {
    f.clear();
    gfx::text(f, 0, 0, i18n::tr(lang, Str::updating_guide));
    char t[96];
    if (d.step == Download::Step::Join)
        snprintf(t, sizeof t, i18n::tr(lang, Str::toast_joining), d.ssid);
    else
        copy(t, sizeof t, i18n::tr(lang, d.step == Download::Step::Check ? Str::checking : Str::downloading));
    gfx::text(f, 0, 8, t);
    if (d.step == Download::Step::Get && d.total) {
        for (int i = 0; i < 128; i += 2) f.px(i, 19, 1);
        const uint64_t done = d.done < d.total ? d.done : d.total;
        f.fill(0, 16, static_cast<int>((2 * done * 128 + d.total) / (2ull * d.total)), 7, 1);
        snprintf(t, sizeof t, "%u/%u KB", static_cast<unsigned>((d.done + 512) / 1024),
                 static_cast<unsigned>((d.total + 512) / 1024));
        gfx::textR(f, 128, 24, t);
    }
    gfx::text(f, 0, 24, i18n::tr(lang, Str::back_cancel));
}

void Ui::drawMessage(Frame& f) { gfx::text(f, (gfx::W - gfx::textW(message_)) >> 1, 12, message_); }

}  // namespace ui
