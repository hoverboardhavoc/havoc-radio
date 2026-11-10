// RDS decoder: takes the groups the Si4732 receives, each with its four block error levels, and holds the decoded
// PI, PS, RadioText, PTY, TP, TA and AF list. Pure, with no I2C and no heap: compiled into the firmware and into
// the host tests.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rds {

// One group: blocks A to D and the block error levels BLEA..BLED packed as the receiver reports them, BLEA in
// bits 7:6. Level 0 is no errors, 1 one or two bits corrected, 2 three to five corrected, 3 uncorrectable.
struct Group {
    uint16_t blk[4];
    uint8_t ble;
    int level(int block) const { return (ble >> (6 - 2 * block)) & 3; }
};

constexpr int kPsLen = 8;
constexpr int kRtLen = 64;
constexpr int kMaxAf = 25;
constexpr int kPtyCount = 32;  // 0 is none; 1 to 31 have names
// Room for the longest text as UTF-8 (three bytes at most per character) and its terminator.
constexpr size_t kPsBuf = kPsLen * 3 + 1;
constexpr size_t kRtBuf = kRtLen * 3 + 1;

// A character of the RDS character table (IEC 62106, annex E) as UTF-8: the full character for the web page, or
// for the radio's display "?" where the Pip font has no glyph. Control codes give "".
const char* charText(uint8_t code, bool display);

class Decoder {
public:
    Decoder() { reset(); }
    // Back to nothing received: a tune, a seek, a band change or leaving FM.
    void reset();
    // The receiver's RDS synchronisation flag.
    void setSync(bool on) { sync_ = on; }
    void group(const Group& g);

    bool sync() const { return sync_; }
    bool hasPi() const { return pi_.held; }
    uint16_t pi() const { return pi_.value; }
    uint8_t pty() const { return pty_.held ? pty_.value : 0; }
    bool tp() const { return tp_.held && tp_.value; }
    bool ta() const { return ta_.held && ta_.value; }
    int afCount() const { return afCount_; }
    int32_t afHz(int i) const;  // ascending
    // Changes whenever the RadioText is cleared: an A/B flag change or a reset.
    uint8_t rtSeq() const { return rtSeq_; }
    // The text up to the first position not yet accepted (and before a carriage return in RT), without trailing
    // spaces, as UTF-8 in `out`. With `display` the characters without a Pip glyph are "?". Returns the length.
    size_t ps(char* out, size_t n, bool display) const;
    size_t rt(char* out, size_t n, bool display) const;

private:
    // A value taken when the same one arrives `need` times in a row.
    struct Hold {
        uint16_t value;
        uint16_t cand;
        uint8_t count;
        bool held;
        // True when this arrival made a value held.
        bool see(uint16_t v, uint8_t need);
    };
    void text(uint8_t* chars, uint8_t* cand, uint64_t& acc, uint64_t& seen, int pos, uint8_t code);
    void af(uint8_t code);
    void clearRt();

    bool sync_;
    Hold pi_, pty_, tp_, ta_;
    uint8_t ps_[kPsLen], psCand_[kPsLen];
    uint64_t psAcc_, psSeen_;
    uint8_t rt_[kRtLen], rtCand_[kRtLen];
    uint64_t rtAcc_, rtSeen_;
    int8_t rtAb_;  // -1 before the first RadioText group
    uint8_t rtSeq_ = 0;
    bool afLf_;    // the next AF code is LW or MW
    uint8_t afCode_[kMaxAf];
    uint32_t afLfMask_;  // bit i: afCode_[i] is LW or MW
    uint8_t afCount_;
};

// A group as received, with its arrival time in ms since boot.
struct Raw {
    uint32_t t;
    Group g;
};

// The last kLen groups received, oldest first.
class Ring {
public:
    static constexpr int kLen = 64;
    void push(uint32_t t, const Group& g);
    int count() const { return count_; }
    const Raw& at(int i) const { return r_[(head_ + kLen - count_ + i) % kLen]; }
    // "<ms> AAAA BBBB CCCC DDDD EE": blocks and error levels in hex, as the capture API sends a group.
    static void format(const Raw& r, char* out, size_t n);

private:
    Raw r_[kLen];
    uint8_t head_ = 0, count_ = 0;
};

}  // namespace rds
