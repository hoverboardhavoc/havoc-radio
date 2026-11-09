// Encoder decoding against recorded quadrature sequences, and the button state machine.
#include <vector>

#include "input_logic.h"
#include "test.h"

namespace {
int decode(const std::vector<uint8_t>& seq) {
    input::QuadDecoder q;
    int sum = 0;
    for (uint8_t s : seq) sum += q.feed(s);
    return sum;
}
// One detent clockwise (B leads) and anticlockwise (A leads), starting and ending at rest (A = B = 1).
// The state is A (GPIO14) in bit 1, B (GPIO13) in bit 0.
const std::vector<uint8_t> kCw = {2, 0, 1, 3};
const std::vector<uint8_t> kCcw = {1, 0, 2, 3};
std::vector<uint8_t> repeat(const std::vector<uint8_t>& s, int n) {
    std::vector<uint8_t> out;
    for (int i = 0; i < n; i++) out.insert(out.end(), s.begin(), s.end());
    return out;
}
}  // namespace

TEST(quad_detents) {
    CHECK_EQ(decode(kCw), 1);
    CHECK_EQ(decode(kCcw), -1);
    CHECK_EQ(decode(repeat(kCw, 5)), 5);
    CHECK_EQ(decode(repeat(kCcw, 3)), -3);
}

namespace {
// The stock firmware's decoder (AiEsp32RotaryEncoder readEncoder_ISR, all versions 1.0.0 to 1.7.0): state
// B in bit 1, A in bit 0, the same table, constructed as AiEsp32RotaryEncoder(14, 13, 12, -1, 4) on the
// same pins. It turns the right way, so its sign is the one to match.
int stockDecode(const std::vector<uint8_t>& seq) {
    static const int8_t kEncStates[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    uint8_t oldAB = 3;  // B = A = 1 at rest
    int pos = 0;
    for (uint8_t s : seq) {
        const uint8_t a = (s >> 1) & 1, b = s & 1;
        oldAB = static_cast<uint8_t>(oldAB << 2 | b << 1 | a);
        pos += kEncStates[oldAB & 0x0f];
    }
    return pos / 4;  // four steps per detent
}
}  // namespace

TEST(quad_direction_matches_stock) {
    CHECK_EQ(stockDecode(kCw), 1);
    CHECK_EQ(stockDecode(kCcw), -1);
    CHECK_EQ(decode(kCw), stockDecode(kCw));
    CHECK_EQ(decode(kCcw), stockDecode(kCcw));
}

TEST(quad_contact_bounce) {
    // Bounce on each edge (the state flips back and forth before settling) still counts one detent.
    CHECK_EQ(decode({2, 3, 2, 0, 2, 0, 1, 0, 1, 3, 1, 3}), 1);
    CHECK_EQ(decode({1, 3, 1, 0, 1, 0, 2, 3, 2, 3}), -1);
}

TEST(quad_partial_and_reversal) {
    // Half a detent and back is no count; a missed edge still counts.
    CHECK_EQ(decode({2, 0, 2, 3}), 0);
    CHECK_EQ(decode({2, 1, 3}), 1);  // 00 missed
    CHECK_EQ(decode({2, 0, 1, 3, 1, 0, 2, 3}), 0);
}

namespace {
struct Rig {
    input::Buttons b;
    uint32_t now = 0;
    std::vector<ui::Event> got;
    void run(uint32_t ms, bool knob, bool side = false, int detents = 0) {
        for (uint32_t i = 0; i < ms; i += 5) {
            input::EventQueue q;
            b.step(now, knob, side, i == 0 ? detents : 0, q);
            for (int k = 0; k < q.n; k++) got.push_back(q.ev[k]);
            now += 5;
        }
    }
};
}  // namespace

TEST(press_short) {
    Rig r;
    r.run(50, false);
    r.run(200, true);
    r.run(100, false);
    CHECK_EQ(r.got.size(), 1u);
    CHECK(r.got[0].type == ui::EventType::Press);
}

TEST(press_bounce_filtered) {
    Rig r;
    r.run(50, false);
    for (int i = 0; i < 3; i++) r.run(5, true), r.run(5, false);
    r.run(100, false);
    CHECK(r.got.empty());
}

TEST(long_press_fires_once_without_release) {
    Rig r;
    r.run(50, false);
    r.run(590, true);
    CHECK(r.got.empty());
    r.run(400, true);
    CHECK_EQ(r.got.size(), 1u);
    CHECK(r.got[0].type == ui::EventType::LongPress);
    r.run(100, false);
    CHECK_EQ(r.got.size(), 1u);  // no Press after a long press
}

TEST(hold_turn_suppresses_press_and_long) {
    Rig r;
    r.run(50, false);
    r.run(100, true);
    r.run(100, true, false, 2);
    r.run(1000, true);
    r.run(100, false);
    CHECK_EQ(r.got.size(), 1u);
    CHECK(r.got[0].type == ui::EventType::HoldTurn && r.got[0].n == 2);
    r.run(10, false, false, -1);
    CHECK(r.got.back().type == ui::EventType::Turn && r.got.back().n == -1);
}

TEST(side_button) {
    Rig r;
    r.run(50, false);
    r.run(100, false, true);
    r.run(100, false, false);
    CHECK_EQ(r.got.size(), 1u);
    CHECK(r.got[0].type == ui::EventType::Side);
}
