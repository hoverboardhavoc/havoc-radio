#include "radio_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace radio {

// Ranges and modes as in the prototype's BANDS table (checked by the host tests).
const Band kBands[kBandCount] = {
    {"FM", 87500, 108000, Mode::FM},  {"LW", 153, 279, Mode::AM},       {"MW", 531, 1602, Mode::AM},
    {"160m", 1810, 2000, Mode::LSB},  {"120m", 2300, 2495, Mode::AM},   {"90m", 3200, 3400, Mode::AM},
    {"80m", 3500, 3800, Mode::LSB},   {"75m", 3900, 4000, Mode::AM},    {"60m", 4750, 5060, Mode::AM},
    {"49m", 5900, 6200, Mode::AM},    {"40m", 7000, 7200, Mode::LSB},   {"41m", 7200, 7450, Mode::AM},
    {"31m", 9400, 9900, Mode::AM},    {"25m", 11600, 12100, Mode::AM},  {"22m", 13570, 13870, Mode::AM},
    {"20m", 14000, 14350, Mode::USB}, {"19m", 15100, 15800, Mode::AM},  {"16m", 17480, 17900, Mode::AM},
    {"17m", 18068, 18168, Mode::USB}, {"15m", 21000, 21450, Mode::USB}, {"13m", 21450, 21850, Mode::AM},
    {"12m", 24890, 24990, Mode::USB}, {"11m", 25670, 26100, Mode::AM},  {"CB", 26965, 27405, Mode::AM},
    {"10m", 28000, 29700, Mode::USB},
};

namespace {

const Step kStepsFM[] = {{50000, "50k"}, {100000, "100k"}, {200000, "200k"}};
const Step kStepsAM[] = {{1000, "1k"}, {5000, "5k"}, {9000, "9k"}, {10000, "10k"}};
const Step kStepsSSB[] = {{10, "10Hz"}, {100, "100Hz"}, {1000, "1k"}, {5000, "5k"}};
const char* const kBwFM[] = {"Auto", "110k", "84k", "60k", "40k"};
const char* const kBwAM[] = {"1.0k", "1.8k", "2.0k", "2.5k", "3.0k", "4.0k", "6.0k"};
const char* const kBwSSB[] = {"0.5k", "1.0k", "1.2k", "2.2k", "3.0k", "4.0k"};

template <typename T, size_t N>
constexpr int count(const T (&)[N]) {
    return static_cast<int>(N);
}

int32_t roundTo(int32_t hz, int32_t unit) { return (hz + unit / 2) / unit * unit; }

// Exact error of a * b in IEEE double arithmetic (Dekker's product), so the sign of a * b - p is known
// without relying on a fused multiply-add.
double productError(double a, double b, double p) {
    const double split = 134217729.0;  // 2^27 + 1
    const double ac = a * split, ah = ac - (ac - a), al = a - ah;
    const double bc = b * split, bh = bc - (bc - b), bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

// JavaScript (k / 1000).toFixed(digits) for digits 1 or 2, decided on the exact value of the double.
void fixedMilli(int32_t k, int digits, char* out, size_t n) {
    const int32_t scale = digits == 1 ? 100 : 10;
    int32_t q = k / scale;
    const int32_t r = k % scale;
    if (r * 2 > scale) {
        q++;
    } else if (r * 2 == scale) {
        const double d = k / 1000.0;
        const double p = d * 1000.0;
        const double err = (p - k) + productError(d, 1000.0, p);
        if (err >= 0) q++;  // above the tie, or an exact tie (toFixed picks the larger)
    }
    const int32_t div = digits == 1 ? 10 : 100;
    if (digits == 1)
        snprintf(out, n, "%ld.%01ld", static_cast<long>(q / div), static_cast<long>(q % div));
    else
        snprintf(out, n, "%ld.%02ld", static_cast<long>(q / div), static_cast<long>(q % div));
}

void copyName(char* dst, const char* src) {
    size_t i = 0;
    for (; src[i] && i < kPresetName - 1; i++) dst[i] = src[i];
    // A cut inside a UTF-8 character leaves that character out.
    if ((static_cast<uint8_t>(src[i]) & 0xC0) == 0x80)
        while (i && (static_cast<uint8_t>(dst[--i]) & 0xC0) == 0x80) {
        }
    dst[i] = 0;
}

}  // namespace

double jsRound(double x) {
    const double f = std::floor(x);
    return x - f >= 0.5 ? f + 1 : f;
}

int32_t bandDefaultHz(int i) {
    const Band& b = kBands[i];
    if (b.mode == Mode::FM) return 100700000;
    if (std::strcmp(b.name, "31m") == 0) return 9650000;
    if (std::strcmp(b.name, "49m") == 0) return 6070000;
    if (std::strcmp(b.name, "60m") == 0) return 4996000;
    const int q = b.lo < 1700 ? 9 : 5;
    return static_cast<int32_t>(jsRound((b.lo + b.hi) / 2.0 / q)) * q * 1000;
}

void defaults(State& s) {
    std::memset(&s, 0, sizeof s);
    s.hz = 6070000;
    s.mode = Mode::AM;
    s.stepIdx[kFamFM] = 1;
    s.stepIdx[kFamAM] = 1;
    s.stepIdx[kFamSSB] = 2;
    s.bwIdx[kFamAM] = 5;
    s.bwIdx[kFamSSB] = 4;
    s.bwIdx[kFamFM] = 0;
    s.volume = 45;
    s.agc = true;
    s.contrast = 15;
    s.light = kLightMax;
    for (int i = 0; i < kBandCount; i++) s.bandLast[i] = bandDefaultHz(i);
    s.timeSync = true;
    std::strcpy(s.ntpServer, kDefaultNtpServer);
}

bool validNtpServer(const char* server) {
    size_t n = 0;
    for (; server[n]; n++) {
        const char c = server[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok || n >= static_cast<size_t>(kNtpServerMax)) return false;
    }
    return true;
}

bool setNtpServer(State& s, const char* server) {
    if (!validNtpServer(server)) return false;
    const char* v = *server ? server : kDefaultNtpServer;
    if (!std::strcmp(s.ntpServer, v)) return false;
    std::strcpy(s.ntpServer, v);
    return true;
}

void repair(State& s) {
    State d;
    defaults(d);
    if (s.presetCount > kMaxPresets) s.presetCount = 0;
    if (s.volume > kVolumeMax) s.volume = d.volume;
    if (s.contrast < kContrastMin || s.contrast > kContrastMax) s.contrast = d.contrast;
    if (s.light < kLightMin || s.light > kLightMax) s.light = d.light;
    if (static_cast<int>(s.mode) > static_cast<int>(Mode::USB)) s = d;
    s.hz = clampHz(s.hz, s.mode);
    for (int i = 0; i < kFamilies; i++) {
        int n;
        steps(static_cast<Family>(i), &n);
        if (s.stepIdx[i] >= n) s.stepIdx[i] = d.stepIdx[i];
        bandwidths(static_cast<Family>(i), &n);
        if (s.bwIdx[i] >= n) s.bwIdx[i] = d.bwIdx[i];
    }
    for (int i = 0; i < s.presetCount; i++) s.presets[i].name[kPresetName - 1] = 0;
    s.ntpServer[kNtpServerMax] = 0;
    if (!s.ntpServer[0] || !validNtpServer(s.ntpServer)) std::strcpy(s.ntpServer, d.ntpServer);
}

const char* modeName(Mode m) {
    switch (m) {
        case Mode::FM: return "FM";
        case Mode::AM: return "AM";
        case Mode::LSB: return "LSB";
        case Mode::USB: return "USB";
    }
    return "?";
}

bool parseMode(const char* s, Mode* out) {
    for (Mode m : {Mode::FM, Mode::AM, Mode::LSB, Mode::USB})
        if (std::strcmp(s, modeName(m)) == 0) {
            *out = m;
            return true;
        }
    return false;
}

Family family(Mode m) { return m == Mode::FM ? kFamFM : m == Mode::AM ? kFamAM : kFamSSB; }
bool isFm(Mode m) { return m == Mode::FM; }

const Step* steps(Family f, int* n) {
    switch (f) {
        case kFamFM: *n = count(kStepsFM); return kStepsFM;
        case kFamAM: *n = count(kStepsAM); return kStepsAM;
        default: *n = count(kStepsSSB); return kStepsSSB;
    }
}

const char* const* bandwidths(Family f, int* n) {
    switch (f) {
        case kFamFM: *n = count(kBwFM); return kBwFM;
        case kFamAM: *n = count(kBwAM); return kBwAM;
        default: *n = count(kBwSSB); return kBwSSB;
    }
}

const Step& currentStep(const State& s) {
    int n;
    const Step* st = steps(family(s.mode), &n);
    const int i = s.stepIdx[family(s.mode)];
    return st[i < n ? i : n - 1];
}

const char* currentBandwidth(const State& s) {
    int n;
    const char* const* bw = bandwidths(family(s.mode), &n);
    const int i = s.bwIdx[family(s.mode)];
    return bw[i < n ? i : n - 1];
}

int32_t clampHz(int32_t hz, Mode m) {
    const int32_t lo = isFm(m) ? 64000000 : 150000, hi = isFm(m) ? 108000000 : 30000000;
    return hz < lo ? lo : hz > hi ? hi : hz;
}

int bandIndex(int32_t hz, Mode m) {
    for (int i = 0; i < kBandCount; i++) {
        const Band& b = kBands[i];
        if ((b.mode == Mode::FM) == isFm(m) && hz >= b.lo * 1000 && hz <= b.hi * 1000) return i;
    }
    return -1;
}

const char* bandLabel(int32_t hz, Mode m) {
    const int i = bandIndex(hz, m);
    if (i >= 0) return kBands[i].name;
    if (isFm(m)) return "FM";
    return hz < 300000 ? "LW" : hz < kSwLoKhz * 1000 ? "MW" : "SW";
}

uint16_t amAntennaCap(int32_t khz) { return khz < kSwLoKhz ? 0 : 1; }

void freqText(const State& s, char* num, size_t n, const char** unit) {
    if (isFm(s.mode)) {
        const int32_t c = (s.hz + 5000) / 10000;  // hundredths of a MHz
        snprintf(num, n, "%ld.%02ld", static_cast<long>(c / 100), static_cast<long>(c % 100));
        *unit = "MHz";
        return;
    }
    *unit = "kHz";
    if (s.mode != Mode::AM && currentStep(s).hz < 1000) {
        const int32_t c = (s.hz + 5) / 10;  // hundredths of a kHz
        snprintf(num, n, "%ld.%02ld", static_cast<long>(c / 100), static_cast<long>(c % 100));
        return;
    }
    snprintf(num, n, "%ld", static_cast<long>((s.hz + 500) / 1000));
}

void presetFreqText(const Preset& p, char* out, size_t n) {
    if (isFm(p.mode)) {
        char t[16];
        fixedMilli(p.hz / 1000, 1, t, sizeof t);
        snprintf(out, n, "%sM", t);
        return;
    }
    const int32_t khz = p.hz / 1000, frac = (p.hz % 1000) / 10;
    if (frac == 0)
        snprintf(out, n, "%ldk", static_cast<long>(khz));
    else if (frac % 10 == 0)
        snprintf(out, n, "%ld.%ldk", static_cast<long>(khz), static_cast<long>(frac / 10));
    else
        snprintf(out, n, "%ld.%02ldk", static_cast<long>(khz), static_cast<long>(frac));
}

void rangeText(const Band& b, char* out, size_t n) {
    if (b.mode == Mode::FM) {
        snprintf(out, n, "87.5-108");
        return;
    }
    if (b.hi < 2000) {
        snprintf(out, n, "%ld-%ldk", static_cast<long>(b.lo), static_cast<long>(b.hi));
        return;
    }
    char lo[12], hi[12];
    fixedMilli(b.lo, 2, lo, sizeof lo);
    fixedMilli(b.hi, 2, hi, sizeof hi);
    if (std::strlen(lo) + 1 + std::strlen(hi) > 11) {
        fixedMilli(b.lo, 1, lo, sizeof lo);
        fixedMilli(b.hi, 1, hi, sizeof hi);
    }
    snprintf(out, n, "%s-%s", lo, hi);
}

bool presetAdd(State& s, const char* name, int32_t hz, Mode m) {
    if (s.presetCount >= kMaxPresets) return false;
    Preset& p = s.presets[s.presetCount++];
    copyName(p.name, name);
    p.mode = m;
    p.hz = clampHz(hz, m);
    return true;
}

bool presetRename(State& s, int i, const char* name) {
    if (i < 0 || i >= s.presetCount) return false;
    copyName(s.presets[i].name, name);
    return true;
}

bool presetMove(State& s, int from, int to) {
    if (from < 0 || from >= s.presetCount || to < 0 || to >= s.presetCount) return false;
    const Preset p = s.presets[from];
    if (from < to)
        std::memmove(&s.presets[from], &s.presets[from + 1], sizeof(Preset) * (to - from));
    else
        std::memmove(&s.presets[to + 1], &s.presets[to], sizeof(Preset) * (from - to));
    s.presets[to] = p;
    return true;
}

bool presetDelete(State& s, int i) {
    if (i < 0 || i >= s.presetCount) return false;
    std::memmove(&s.presets[i], &s.presets[i + 1], sizeof(Preset) * (s.presetCount - i - 1));
    s.presetCount--;
    return true;
}

namespace {

int32_t alignHz(int32_t hz, Mode m) {
    hz = clampHz(hz, m);
    if (m == Mode::FM) return roundTo(hz, 10000);
    if (m == Mode::AM) return roundTo(hz, 1000);
    return roundTo(hz, 10);
}

Mode usable(Mode m, bool ssb) { return (m == Mode::LSB || m == Mode::USB) && !ssb ? Mode::AM : m; }

template <typename T>
bool set(T& field, T v) {
    if (field == v) return false;
    field = v;
    return true;
}

}  // namespace

bool apply(State& s, const Command& c, bool ssb, const char* station) {
    switch (c.cmd) {
        case Cmd::Tune: return set(s.hz, alignHz(c.v, s.mode));
        case Cmd::Band: {
            if (c.v < 0 || c.v >= kBandCount) return false;
            const int cur = bandIndex(s.hz, s.mode);
            if (cur >= 0) s.bandLast[cur] = s.hz;
            s.mode = usable(kBands[c.v].mode, ssb);
            s.hz = alignHz(s.bandLast[c.v], s.mode);
            return true;
        }
        case Cmd::SetMode: {
            const Mode m = static_cast<Mode>(c.v);
            if (m == Mode::FM || isFm(s.mode) || usable(m, ssb) != m) return false;
            s.mode = m;
            s.hz = alignHz(s.hz, m);
            return true;
        }
        case Cmd::Step: {
            int n;
            steps(family(s.mode), &n);
            if (c.v < 0 || c.v >= n) return false;
            return set(s.stepIdx[family(s.mode)], static_cast<uint8_t>(c.v));
        }
        case Cmd::Bandwidth: {
            int n;
            bandwidths(family(s.mode), &n);
            if (c.v < 0 || c.v >= n) return false;
            return set(s.bwIdx[family(s.mode)], static_cast<uint8_t>(c.v));
        }
        case Cmd::Volume: {
            const int v = c.v < 0 ? 0 : c.v > kVolumeMax ? kVolumeMax : c.v;
            const bool changed = s.volume != v || s.muted;
            s.volume = static_cast<uint8_t>(v);
            s.muted = false;
            return changed;
        }
        case Cmd::Mute: return set(s.muted, c.v != 0);
        case Cmd::Agc: return set(s.agc, c.v != 0);
        case Cmd::Contrast: {
            const int v = c.v < kContrastMin ? kContrastMin : c.v > kContrastMax ? kContrastMax : c.v;
            return set(s.contrast, static_cast<uint8_t>(v));
        }
        case Cmd::Light: {
            const int v = c.v < kLightMin ? kLightMin : c.v > kLightMax ? kLightMax : c.v;
            return set(s.light, static_cast<uint8_t>(v));
        }
        case Cmd::Language:
            if (c.v < 0 || c.v > 255) return false;
            return set(s.lang, static_cast<uint8_t>(c.v));
        case Cmd::TimeSync: return set(s.timeSync, c.v != 0);
        case Cmd::SavePreset:
            return presetAdd(s, station && *station ? station : bandLabel(s.hz, s.mode), s.hz, s.mode);
        case Cmd::TunePreset: {
            if (c.v < 0 || c.v >= s.presetCount) return false;
            const Preset& p = s.presets[c.v];
            s.mode = usable(p.mode, ssb);
            s.hz = alignHz(p.hz, s.mode);
            return true;
        }
        default: return false;
    }
}

}  // namespace radio
