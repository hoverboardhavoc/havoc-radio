#include "si4732.h"

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

namespace si4732 {

namespace {

const char* TAG = "si4732";

// Commands (AN332).
constexpr uint8_t kPowerUp = 0x01;
constexpr uint8_t kGetRev = 0x10;
constexpr uint8_t kPowerDown = 0x11;
constexpr uint8_t kSetProperty = 0x12;
constexpr uint8_t kGetIntStatus = 0x14;
constexpr uint8_t kFmTuneFreq = 0x20;
constexpr uint8_t kFmSeekStart = 0x21;
constexpr uint8_t kFmTuneStatus = 0x22;
constexpr uint8_t kFmRsqStatus = 0x23;
constexpr uint8_t kFmAgcOverride = 0x28;
constexpr uint8_t kAmTuneFreq = 0x40;  // SSB_TUNE_FREQ in patch mode
constexpr uint8_t kAmSeekStart = 0x41;
constexpr uint8_t kAmTuneStatus = 0x42;
constexpr uint8_t kAmRsqStatus = 0x43;
constexpr uint8_t kAmAgcOverride = 0x48;

// Status bits.
constexpr uint8_t kCts = 0x80;
constexpr uint8_t kErr = 0x40;
constexpr uint8_t kStcInt = 0x01;

// POWER_UP ARG1 bits and ARG2.
constexpr uint8_t kPatch = 0x20;
constexpr uint8_t kXoscEn = 0x10;  // 32.768 kHz crystal on RCLK
constexpr uint8_t kFuncFm = 0x00;
constexpr uint8_t kFuncAm = 0x01;
constexpr uint8_t kAnalogOut = 0x05;

constexpr int kCtsTimeoutMs = 300;
constexpr int kStcTimeoutMs = 600;

bool waitCts(uint8_t* status = nullptr) {
    for (int i = 0; i < kCtsTimeoutMs; i++) {
        uint8_t s = 0;
        if (!i2c::read(board::kSi4732Addr, &s, 1)) return false;
        if (s & kCts) {
            if (status) *status = s;
            return true;
        }
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "CTS timeout");
    return false;
}

// Waits for the seek/tune complete interrupt bit.
bool waitStc() {
    for (int i = 0; i < kStcTimeoutMs / 5; i++) {
        if (seekDone()) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

uint8_t seekArg(bool up) { return static_cast<uint8_t>((up ? 0x08 : 0) | 0x04); }  // SEEKUP, WRAP

bool tuneAndAck(bool fm, const uint8_t* cmd, size_t n) {
    if (!command(cmd, n) || !waitStc()) return false;
    TuneStatus st;
    return tuneStatus(fm, false, true, st);
}

}  // namespace

bool command(const uint8_t* cmd, size_t n, uint8_t* resp, size_t rn) {
    if (!n || !opcodeAllowed(cmd[0])) {
        ESP_LOGE(TAG, "rejected opcode 0x%02x", n ? cmd[0] : 0);
        return false;
    }
    if (!waitCts() || !i2c::write(board::kSi4732Addr, cmd, n)) return false;
    uint8_t status = 0;
    if (!waitCts(&status)) return false;
    if (rn && !i2c::read(board::kSi4732Addr, resp, rn)) return false;
    return !(status & kErr);
}

bool setProperty(uint16_t p, uint16_t v) {
    const uint8_t c[] = {kSetProperty, 0, static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p),
                         static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return command(c, sizeof c);
}

bool powerUp(Func f, const uint8_t* patch, size_t n) {
    board::siReset();
    const bool usePatch = f == Func::SSB;
    if (usePatch && !patchValid(patch, n)) return false;
    const uint8_t arg1 = static_cast<uint8_t>(kXoscEn | (usePatch ? kPatch : 0) | (f == Func::FM ? kFuncFm : kFuncAm));
    const uint8_t c[] = {kPowerUp, arg1, kAnalogOut};
    if (!command(c, sizeof c)) return false;
    // Crystal start-up time before the receiver is used.
    vTaskDelay(pdMS_TO_TICKS(500));
    if (usePatch) {
        uint8_t rec[8];
        for (size_t i = 0; i < n; i += 8) {
            for (int k = 0; k < 8; k++) rec[k] = patch[i + k];
            if (!command(rec, 8)) {
                ESP_LOGE(TAG, "patch record %u failed", static_cast<unsigned>(i / 8));
                return false;
            }
        }
    }
    return true;
}

bool powerDown() {
    const uint8_t c = kPowerDown;
    return command(&c, 1);
}

uint8_t partNumber() {
    const uint8_t c = kGetRev;
    uint8_t r[9] = {};
    return command(&c, 1, r, sizeof r) ? r[1] : 0;
}

bool tuneFm(uint16_t f) {
    const uint8_t c[] = {kFmTuneFreq, 0, static_cast<uint8_t>(f >> 8), static_cast<uint8_t>(f), 0};
    return tuneAndAck(true, c, sizeof c);
}

bool tuneAm(uint16_t khz, uint16_t antcap) {
    const auto c = amCommand(kAmTuneFreq, 0, khz, antcap);
    return tuneAndAck(false, c.data(), c.size());
}

bool tuneSsb(uint16_t khz, bool usb, uint16_t antcap) {
    // ARG1 bits 7:6 select the sideband in patch mode: 1 = LSB, 2 = USB.
    const auto c = amCommand(kAmTuneFreq, static_cast<uint8_t>((usb ? 2 : 1) << 6), khz, antcap);
    return tuneAndAck(false, c.data(), c.size());
}

bool seekFm(bool up) {
    const uint8_t c[] = {kFmSeekStart, seekArg(up)};
    return command(c, sizeof c);
}

bool seekAm(bool up, uint16_t antcap) {
    const auto c = amCommand(kAmSeekStart, seekArg(up), 0, antcap);
    return command(c.data(), c.size());
}

bool seekDone() {
    const uint8_t c = kGetIntStatus;
    uint8_t s = 0;
    return command(&c, 1, &s, 1) && (s & kStcInt);
}

bool tuneStatus(bool fm, bool cancel, bool ack, TuneStatus& out) {
    const uint8_t c[] = {fm ? kFmTuneStatus : kAmTuneStatus,
                         static_cast<uint8_t>((cancel ? 0x02 : 0) | (ack ? 0x01 : 0))};
    uint8_t r[8] = {};
    if (!command(c, sizeof c, r, sizeof r)) return false;
    out.bandLimit = r[1] & 0x80;
    out.valid = r[1] & 0x01;
    out.freq = static_cast<uint16_t>(r[2] << 8 | r[3]);
    out.rssi = r[4];
    out.snr = r[5];
    return true;
}

bool rsq(bool fm, uint8_t& rssi, uint8_t& snr) {
    const uint8_t c[] = {fm ? kFmRsqStatus : kAmRsqStatus, 0};
    uint8_t r[8] = {};
    if (!command(c, sizeof c, r, sizeof r)) return false;
    rssi = r[4];
    snr = r[5];
    return true;
}

bool agc(bool fm, bool on) {
    const uint8_t c[] = {fm ? kFmAgcOverride : kAmAgcOverride, static_cast<uint8_t>(on ? 0 : 1), 0};
    return command(c, sizeof c);
}

}  // namespace si4732
