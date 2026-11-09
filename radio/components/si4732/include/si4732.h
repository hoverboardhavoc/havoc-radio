// Si4732-A10 receiver driver over I2C, following AN332 (Si47xx programming guide).
// Every command waits for CTS first; the undocumented opcodes 0xF0..0xFF are never sent.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace si4732 {

// Opcodes 0xF0..0xFF are undocumented; some latch a test mode that only a power cycle clears.
constexpr bool opcodeAllowed(uint8_t op) { return op < 0xF0; }

// The SSB patch is a sequence of 8-byte patch download commands (opcode 0x15 or 0x16).
inline bool patchValid(const uint8_t* p, size_t n) {
    if (!p || !n || n % 8) return false;
    for (size_t i = 0; i < n; i += 8)
        if (p[i] != 0x15 && p[i] != 0x16) return false;
    return true;
}

// AM_TUNE_FREQ (SSB_TUNE_FREQ in patch mode) and AM_SEEK_START share one layout: opcode, ARG1, frequency in kHz
// (0 for a seek), antenna tuning capacitor (0 = automatic).
inline std::array<uint8_t, 6> amCommand(uint8_t op, uint8_t arg1, uint16_t khz, uint16_t antcap) {
    return {op, arg1, static_cast<uint8_t>(khz >> 8), static_cast<uint8_t>(khz), static_cast<uint8_t>(antcap >> 8),
            static_cast<uint8_t>(antcap)};
}

enum class Func : uint8_t { FM, AM, SSB };

struct TuneStatus {
    uint16_t freq;  // FM: 10 kHz units; AM/SSB: kHz
    uint8_t rssi;   // dBuV
    uint8_t snr;    // dB
    bool valid;
    bool bandLimit;
};

// GPIO reset pulse, then POWER_UP in the given function. SSB powers up in patch mode and downloads the patch.
bool powerUp(Func f, const uint8_t* patch = nullptr, size_t n = 0);
bool powerDown();
// GET_REV part number (32 for the Si4732), 0 when the chip does not answer.
uint8_t partNumber();

// Sends one command after CTS and reads `rn` response bytes (status first) after the next CTS.
// Returns false for a rejected opcode, a bus error, a CTS timeout or ERR in the status.
bool command(const uint8_t* cmd, size_t n, uint8_t* resp = nullptr, size_t rn = 0);
bool setProperty(uint16_t prop, uint16_t value);

bool tuneFm(uint16_t f10k);
// `antcap` is the antenna tuning capacitor (ANTCAP), 0 = automatic.
bool tuneAm(uint16_t khz, uint16_t antcap);
bool tuneSsb(uint16_t khz, bool usb, uint16_t antcap);
bool seekFm(bool up);
bool seekAm(bool up, uint16_t antcap);
bool seekDone();  // STCINT set
bool tuneStatus(bool fm, bool cancel, bool ack, TuneStatus& out);
bool rsq(bool fm, uint8_t& rssi, uint8_t& snr);
bool agc(bool fm, bool on);

// AN332 property numbers used by the firmware.
namespace prop {
constexpr uint16_t kSsbBfo = 0x0100;
constexpr uint16_t kSsbMode = 0x0101;
constexpr uint16_t kFmDeemphasis = 0x1100;
constexpr uint16_t kFmChannelFilter = 0x1102;
constexpr uint16_t kFmSeekBottom = 0x1400;
constexpr uint16_t kFmSeekTop = 0x1401;
constexpr uint16_t kFmSeekSpacing = 0x1402;
constexpr uint16_t kAmChannelFilter = 0x3102;
constexpr uint16_t kAmAvcMaxGain = 0x3103;  // AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, 340 per dB
constexpr uint16_t kAmSoftMuteMaxAttenuation = 0x3302;
constexpr uint16_t kAmSeekBottom = 0x3400;
constexpr uint16_t kAmSeekTop = 0x3401;
constexpr uint16_t kAmSeekSpacing = 0x3402;
constexpr uint16_t kRxVolume = 0x4000;
constexpr uint16_t kRxHardMute = 0x4001;
}  // namespace prop

}  // namespace si4732
