// Radio model: bands, modes, steps, bandwidths, presets and the commands that change them.
// Pure C++: compiled into the firmware and into the host tests.
#pragma once
#include <cstddef>
#include <cstdint>

namespace radio {

enum class Mode : uint8_t { FM, AM, LSB, USB };
// Steps and bandwidths are kept per mode family.
enum Family : uint8_t { kFamFM, kFamAM, kFamSSB, kFamilies };

// The lamp on the back: off, on at the light level, or flashing SOS at it. Not stored: off at power-up.
enum class Lamp : uint8_t { Off, On, Sos };

struct Band {
    const char* name;
    int32_t lo;  // kHz
    int32_t hi;  // kHz
    Mode mode;
};
constexpr int kBandCount = 25;
extern const Band kBands[kBandCount];
// Outside the named bands AM is LW and MW below this frequency and SW from it up.
constexpr int32_t kSwLoKhz = 1800;

struct Step {
    int32_t hz;
    const char* label;
};

constexpr int kMaxPresets = 20;
constexpr int kPresetName = 11;  // up to 10 characters

struct Preset {
    char name[kPresetName];
    Mode mode;
    int32_t hz;
};

constexpr int kVolumeMax = 63;
constexpr int kContrastMin = 1;
constexpr int kContrastMax = 15;
constexpr int kLightMin = 1;  // the lamp level
constexpr int kLightMax = 15;

// NTP server: a host name or IPv4 address of letters, digits, '.' and '-'.
constexpr int kNtpServerMax = 63;
constexpr const char* kDefaultNtpServer = "pool.ntp.org";

struct State {
    int32_t hz;  // tuned frequency in Hz; SSB tunes in 10 Hz steps
    Mode mode;
    uint8_t stepIdx[kFamilies];
    uint8_t bwIdx[kFamilies];
    uint8_t volume;
    bool muted;
    bool agc;
    uint8_t contrast;
    uint8_t light;  // the lamp level, kLightMin..kLightMax
    uint8_t lang;  // index into the UI language list
    int32_t bandLast[kBandCount];  // Hz
    uint8_t presetCount;
    Preset presets[kMaxPresets];
    bool timeSync;  // set the clock from NTP while joined
    char ntpServer[kNtpServerMax + 1];
};

void defaults(State& s);
// Puts a loaded state back in range: a value out of range takes its default. A light level of 0 (off, before the
// lamp had its own on and off) loads as the default, 15.
void repair(State& s);

const char* modeName(Mode m);
bool parseMode(const char* s, Mode* out);
Family family(Mode m);
bool isFm(Mode m);

const Step* steps(Family f, int* count);
const char* const* bandwidths(Family f, int* count);
const Step& currentStep(const State& s);
const char* currentBandwidth(const State& s);

int32_t clampHz(int32_t hz, Mode m);
int bandIndex(int32_t hz, Mode m);  // -1 outside every named band
const char* bandLabel(int32_t hz, Mode m);
int32_t bandDefaultHz(int band);
// AM antenna tuning capacitor (ANTCAP) for a tuned frequency: 0 (automatic, tuned for a ferrite loop) on LW and
// MW, 1 on SW for a whip or wire, as the stock firmware sets it.
uint16_t amAntennaCap(int32_t khz);

// Text forms used by the display and the web page.
void freqText(const State& s, char* num, size_t n, const char** unit);  // "100.70" "MHz"
void presetFreqText(const Preset& p, char* out, size_t n);              // "100.7M", "7100k"
void rangeText(const Band& b, char* out, size_t n);                     // "87.5-108", "5.90-6.20"

enum class Cmd : uint8_t {
    Tune,         // v = Hz
    Band,         // v = band index
    SetMode,      // v = Mode
    Step,         // v = index for the current mode family
    Bandwidth,    // v = index for the current mode family
    Volume,       // v = 0..63, unmutes
    Mute,         // v = 0/1
    Agc,          // v = 0/1
    Contrast,     // v = 1..15
    Light,        // v = 1..15, the lamp level
    Lamp,         // v = Lamp: off, on or SOS (not part of the stored state)
    Language,     // v = language index
    SavePreset,   // current frequency, named after the station (its RDS PS) or else the band
    TunePreset,   // v = index
    Seek,         // v = +1 / -1
    SeekStop,
    Scan,         // WiFi scan
    Join,         // WiFi join with the text-entry credentials
    Install,      // confirm a pending firmware update
    CancelInstall,
    SplashEnd,    // the splash ended or was skipped: WiFi starts
    TimeSync,     // v = 0/1
    NtpServer,    // the server typed in text entry
    GuideUpdate,  // Settings > Guide > Update guide: restart into download mode
};

struct Command {
    Cmd cmd;
    int32_t v;
};

// Applies a state command. Commands that do not change the model (seek, WiFi, install) return false.
// `station` is the station name shown (the RDS PS), "" when there is none.
bool apply(State& s, const Command& c, bool ssb, const char* station = "");

// Preset editing for the web API.
bool presetAdd(State& s, const char* name, int32_t hz, Mode m);
bool presetRename(State& s, int i, const char* name);
bool presetMove(State& s, int from, int to);
bool presetDelete(State& s, int i);

// The NTP server check: 1 to 63 letters, digits, '.' and '-', or empty for the default.
bool validNtpServer(const char* server);
// Sets the NTP server; an empty one sets the default. Returns false, leaving the state as it was, for a server
// that fails the check or is the one already set.
bool setNtpServer(State& s, const char* server);

// JavaScript Math.round: nearest integer, ties toward +infinity.
double jsRound(double x);

}  // namespace radio
