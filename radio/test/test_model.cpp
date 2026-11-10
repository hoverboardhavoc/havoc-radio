// Band, frequency, step and preset math of the radio model.
#include <cstring>
#include <regex>

#include "radio_model.h"
#include "util.h"

using radio::Cmd;
using radio::Mode;

namespace {
std::string freq(const radio::State& s) {
    char n[16];
    const char* u;
    radio::freqText(s, n, sizeof n, &u);
    return std::string(n) + " " + u;
}
std::string presetText(int32_t hz, Mode m) {
    radio::Preset p{};
    p.hz = hz;
    p.mode = m;
    char out[16];
    radio::presetFreqText(p, out, sizeof out);
    return out;
}
std::string range(int band) {
    char out[16];
    radio::rangeText(radio::kBands[band], out, sizeof out);
    return out;
}
int bandNamed(const char* n) {
    for (int i = 0; i < radio::kBandCount; i++)
        if (!std::strcmp(radio::kBands[i].name, n)) return i;
    return -1;
}
}  // namespace

TEST(bands_match_prototype) {
    const std::string body = jsBlock(prototype(), "const BANDS =", '[');
    const std::regex re(R"(\{ n: '([^']+)',\s*lo: (\d+),\s*hi: (\d+),\s*m: '([A-Z]+)' \})");
    int i = 0;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it, i++) {
        if (i >= radio::kBandCount) break;
        const radio::Band& b = radio::kBands[i];
        CHECK_STR(b.name, (*it)[1].str());
        CHECK_EQ(b.lo, std::stoi((*it)[2]));
        CHECK_EQ(b.hi, std::stoi((*it)[3]));
        CHECK_STR(radio::modeName(b.mode), (*it)[4].str());
    }
    CHECK_EQ(i, radio::kBandCount);
}

TEST(band_defaults) {
    CHECK_EQ(radio::bandDefaultHz(bandNamed("FM")), 100700000);
    CHECK_EQ(radio::bandDefaultHz(bandNamed("LW")), 216000);
    CHECK_EQ(radio::bandDefaultHz(bandNamed("MW")), 1071000);  // 118.5 rounds up, as Math.round
    CHECK_EQ(radio::bandDefaultHz(bandNamed("160m")), 1905000);
    CHECK_EQ(radio::bandDefaultHz(bandNamed("31m")), 9650000);
    CHECK_EQ(radio::bandDefaultHz(bandNamed("49m")), 6070000);
    CHECK_EQ(radio::bandDefaultHz(bandNamed("60m")), 4996000);
    CHECK_EQ(radio::bandDefaultHz(bandNamed("CB")), 27185000);
}

TEST(band_lookup_and_labels) {
    CHECK_EQ(radio::bandIndex(7200000, Mode::LSB), bandNamed("40m"));  // first match wins
    CHECK_EQ(radio::bandIndex(7201000, Mode::AM), bandNamed("41m"));
    CHECK_EQ(radio::bandIndex(21450000, Mode::AM), bandNamed("15m"));
    CHECK_EQ(radio::bandIndex(100000000, Mode::AM), -1);  // FM range only matches in FM
    CHECK_EQ(radio::bandIndex(100000000, Mode::FM), bandNamed("FM"));
    CHECK_STR(radio::bandLabel(290000, Mode::AM), "LW");
    CHECK_STR(radio::bandLabel(1700000, Mode::AM), "MW");
    CHECK_STR(radio::bandLabel(6300000, Mode::AM), "SW");
    CHECK_STR(radio::bandLabel(70000000, Mode::FM), "FM");
}

TEST(am_antenna_cap) {
    // Automatic on LW and MW, 1 from the start of SW.
    CHECK_EQ(radio::amAntennaCap(225), 0);
    CHECK_EQ(radio::amAntennaCap(999), 0);
    CHECK_EQ(radio::amAntennaCap(1799), 0);
    CHECK_EQ(radio::amAntennaCap(1800), 1);
    CHECK_EQ(radio::amAntennaCap(5930), 1);
    CHECK_EQ(radio::amAntennaCap(17490), 1);
}

TEST(range_text) {
    CHECK_STR(range(bandNamed("FM")), "87.5-108");
    CHECK_STR(range(bandNamed("LW")), "153-279k");
    CHECK_STR(range(bandNamed("160m")), "1.81-2.00");
    CHECK_STR(range(bandNamed("120m")), "2.30-2.50");   // 2.495 is stored above the tie
    CHECK_STR(range(bandNamed("CB")), "26.96-27.41");   // 26.965 below, 27.405 above
    CHECK_STR(range(bandNamed("13m")), "21.45-21.85");
}

TEST(freq_text) {
    radio::State s;
    radio::defaults(s);
    CHECK_STR(freq(s), "6070 kHz");
    s.hz = 6070500;
    CHECK_STR(freq(s), "6071 kHz");
    s.mode = Mode::FM;
    s.hz = 100700000;
    CHECK_STR(freq(s), "100.70 MHz");
    s.mode = Mode::LSB;
    s.hz = 7100070;
    s.stepIdx[radio::kFamSSB] = 0;
    CHECK_STR(freq(s), "7100.07 kHz");
    s.stepIdx[radio::kFamSSB] = 2;  // 1 kHz step shows whole kHz
    CHECK_STR(freq(s), "7100 kHz");
}

TEST(preset_text) {
    CHECK_STR(presetText(100700000, Mode::FM), "100.7M");
    CHECK_STR(presetText(100750000, Mode::FM), "100.8M");  // exact tie rounds up
    CHECK_STR(presetText(100050000, Mode::FM), "100.0M");  // 100.05 is stored below the tie
    CHECK_STR(presetText(7100000, Mode::AM), "7100k");
    CHECK_STR(presetText(7100370, Mode::LSB), "7100.37k");
    CHECK_STR(presetText(7100300, Mode::LSB), "7100.3k");
}

TEST(tune_and_clamp) {
    radio::State s;
    radio::defaults(s);
    CHECK(radio::apply(s, {Cmd::Tune, 50000}, true));
    CHECK_EQ(s.hz, 150000);
    radio::apply(s, {Cmd::Tune, 40000000}, true);
    CHECK_EQ(s.hz, 30000000);
    radio::apply(s, {Cmd::Tune, 7100400}, true);  // AM tunes whole kHz
    CHECK_EQ(s.hz, 7100000);
    radio::apply(s, {Cmd::Band, bandNamed("FM")}, true);
    radio::apply(s, {Cmd::Tune, 120000000}, true);
    CHECK_EQ(s.hz, 108000000);
    radio::apply(s, {Cmd::Tune, 100123000}, true);  // FM tunes 10 kHz
    CHECK_EQ(s.hz, 100120000);
}

TEST(band_switch_remembers_last) {
    radio::State s;
    radio::defaults(s);
    radio::apply(s, {Cmd::Tune, 6100000}, true);
    radio::apply(s, {Cmd::Band, bandNamed("FM")}, true);
    CHECK(s.mode == Mode::FM);
    CHECK_EQ(s.hz, 100700000);
    radio::apply(s, {Cmd::Band, bandNamed("49m")}, true);
    CHECK(s.mode == Mode::AM);
    CHECK_EQ(s.hz, 6100000);
    // Outside a named band nothing is stored.
    radio::apply(s, {Cmd::Tune, 6300000}, true);
    radio::apply(s, {Cmd::Band, bandNamed("31m")}, true);
    radio::apply(s, {Cmd::Band, bandNamed("49m")}, true);
    CHECK_EQ(s.hz, 6100000);
    // SSB bands fall back to AM without the patch.
    radio::apply(s, {Cmd::Band, bandNamed("40m")}, false);
    CHECK(s.mode == Mode::AM);
    radio::apply(s, {Cmd::Band, bandNamed("40m")}, true);
    CHECK(s.mode == Mode::LSB);
}

TEST(mode_rules) {
    radio::State s;
    radio::defaults(s);
    CHECK(!radio::apply(s, {Cmd::SetMode, static_cast<int>(Mode::FM)}, true));
    CHECK(!radio::apply(s, {Cmd::SetMode, static_cast<int>(Mode::USB)}, false));
    CHECK(radio::apply(s, {Cmd::SetMode, static_cast<int>(Mode::USB)}, true));
    radio::apply(s, {Cmd::Step, 0}, true);
    radio::apply(s, {Cmd::Tune, s.hz + 70}, true);
    CHECK_EQ(s.hz, 6070070);
    radio::apply(s, {Cmd::SetMode, static_cast<int>(Mode::AM)}, true);
    CHECK_EQ(s.hz, 6070000);
    // Steps and bandwidths are kept per mode family.
    CHECK_STR(radio::currentStep(s).label, "5k");
    CHECK(!radio::apply(s, {Cmd::Step, 4}, true));
    radio::apply(s, {Cmd::Bandwidth, 0}, true);
    CHECK_STR(radio::currentBandwidth(s), "1.0k");
    radio::apply(s, {Cmd::Band, bandNamed("FM")}, true);
    CHECK_STR(radio::currentBandwidth(s), "Auto");
    CHECK_STR(radio::currentStep(s).label, "100k");
    CHECK(!radio::apply(s, {Cmd::SetMode, static_cast<int>(Mode::AM)}, true));
}

TEST(volume_and_settings) {
    radio::State s;
    radio::defaults(s);
    s.muted = true;
    radio::apply(s, {Cmd::Volume, 70}, true);
    CHECK_EQ(s.volume, 63);
    CHECK(!s.muted);
    radio::apply(s, {Cmd::Contrast, 0}, true);
    CHECK_EQ(s.contrast, 1);
    radio::apply(s, {Cmd::Light, 99}, true);
    CHECK_EQ(s.light, 15);
    CHECK(!radio::apply(s, {Cmd::SideAction, 3}, true));
    CHECK(radio::apply(s, {Cmd::SideAction, 2}, true));
    CHECK(s.side == radio::Side::Back);
}

TEST(presets) {
    radio::State s;
    radio::defaults(s);
    CHECK(radio::apply(s, {Cmd::SavePreset, 0}, true));
    CHECK_EQ(s.presetCount, 1);
    CHECK_STR(s.presets[0].name, "49m");
    radio::presetAdd(s, "A very long name", 9650000, Mode::AM);
    CHECK_STR(s.presets[1].name, "A very lon");
    radio::presetAdd(s, "C", 100000000, Mode::FM);
    CHECK(radio::presetMove(s, 2, 0));
    CHECK_STR(s.presets[0].name, "C");
    CHECK_STR(s.presets[1].name, "49m");
    CHECK(radio::presetMove(s, 0, 2));
    CHECK_STR(s.presets[2].name, "C");
    CHECK(radio::presetRename(s, 0, "Home"));
    CHECK(radio::presetDelete(s, 1));
    CHECK_EQ(s.presetCount, 2);
    CHECK_STR(s.presets[1].name, "C");
    CHECK(!radio::presetDelete(s, 2));
    radio::apply(s, {Cmd::TunePreset, 1}, true);
    CHECK(s.mode == Mode::FM);
    CHECK_EQ(s.hz, 100000000);
    // Named after the station when one is shown.
    CHECK(radio::apply(s, {Cmd::SavePreset, 0}, true, "-METRO-"));
    CHECK_STR(s.presets[s.presetCount - 1].name, "-METRO-");
    CHECK(radio::apply(s, {Cmd::SavePreset, 0}, true, ""));
    CHECK_STR(s.presets[s.presetCount - 1].name, "FM");
    // Ten bytes at most, without cutting a UTF-8 character.
    radio::presetAdd(s, "ÅÄÖÆØÜ", 100000000, Mode::FM);
    CHECK_STR(s.presets[s.presetCount - 1].name, "ÅÄÖÆØ");
    radio::presetAdd(s, "aÅÄÖÆØ", 100000000, Mode::FM);
    CHECK_STR(s.presets[s.presetCount - 1].name, "aÅÄÖÆ");
    radio::presetAdd(s, "abcdefgh€", 100000000, Mode::FM);
    CHECK_STR(s.presets[s.presetCount - 1].name, "abcdefgh");
    while (s.presetCount < radio::kMaxPresets) radio::presetAdd(s, "x", 1000000, Mode::AM);
    CHECK(!radio::apply(s, {Cmd::SavePreset, 0}, true));
}

TEST(js_round) {
    CHECK_EQ(radio::jsRound(2.5), 3.0);
    CHECK_EQ(radio::jsRound(-2.5), -2.0);
    CHECK_EQ(radio::jsRound(0.49999999999999994), 0.0);
}
