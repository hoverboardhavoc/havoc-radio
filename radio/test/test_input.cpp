// Encoder decoding against recorded quadrature sequences, the button state machine, and input scripts.
#include <string>
#include <vector>

#include "input_logic.h"
#include "sim.h"
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

TEST(side_button_short_press) {
    // Reported on the release, before 1 s.
    Rig r;
    r.run(50, false);
    r.run(900, false, true);
    CHECK(r.got.empty());
    r.run(100, false, false);
    CHECK_EQ(r.got.size(), 1u);
    CHECK(r.got[0].type == ui::EventType::Side);
}

TEST(side_button_hold) {
    // The hold acts at 1 s while still held (the press is debounced for 20 ms first); the release does nothing.
    Rig r;
    r.run(50, false);
    r.run(1015, false, true);
    CHECK(r.got.empty());
    r.run(10, false, true);
    CHECK_EQ(r.got.size(), 1u);
    CHECK(r.got[0].type == ui::EventType::SideHold);
    r.run(2000, false, true);
    r.run(100, false, false);
    CHECK_EQ(r.got.size(), 1u);
    // The next short press is reported again.
    r.run(100, false, true);
    r.run(100, false, false);
    CHECK_EQ(r.got.size(), 2u);
    CHECK(r.got[1].type == ui::EventType::Side);
}

TEST(side_button_bounce_filtered) {
    Rig r;
    r.run(50, false);
    for (int i = 0; i < 3; i++) r.run(5, false, true), r.run(5, false, false);
    r.run(100, false);
    CHECK(r.got.empty());
}

namespace {
// Parses `text`; returns the step count, or -1 with the bad line in *bad.
int parse(const std::string& text, input::Step* out, int* bad) {
    return input::parseScript(text.data(), text.size(), out, bad);
}
}  // namespace

TEST(script_ops) {
    input::Step st[input::kScriptLines];
    int bad = -1;
    const std::string text = "turn 3\nturn -2\r\npress\nlong\n  side  \nsidehold\nwait 250\nwait 0\nturn +1";
    CHECK_EQ(parse(text, st, &bad), 9);
    CHECK_EQ(bad, 0);
    CHECK(!st[0].wait && st[0].type == ui::EventType::Turn && st[0].n == 3);
    CHECK(st[1].type == ui::EventType::Turn && st[1].n == -2);
    CHECK(!st[2].wait && st[2].type == ui::EventType::Press);
    CHECK(st[3].type == ui::EventType::LongPress);
    CHECK(st[4].type == ui::EventType::Side);
    CHECK(st[5].type == ui::EventType::SideHold);
    CHECK(st[6].wait && st[6].n == 250);
    CHECK(st[7].wait && st[7].n == 0);
    CHECK(st[8].type == ui::EventType::Turn && st[8].n == 1);
}

TEST(script_comments_and_blank_lines) {
    input::Step st[input::kScriptLines];
    int bad = -1;
    CHECK_EQ(parse("# open the menu\n\npress   # the knob\n\t\n#turn 1\n", st, &bad), 1);
    CHECK(st[0].type == ui::EventType::Press);
    CHECK_EQ(parse("", st, &bad), 0);
    CHECK_EQ(parse("# nothing\n", st, &bad), 0);
}

TEST(script_bad_lines) {
    input::Step st[input::kScriptLines];
    const char* const bad[] = {"hold 1", "turn", "turn 0", "turn 2 3", "turn x", "turn 1001", "turn -", "press 1",
                               "wait", "wait -1", "wait 10001", "wait 1e3", "Press", "shot home"};
    for (const char* line : bad) {
        int n = -1;
        CHECK_EQ(parse(std::string("press\n") + line + "\nside", st, &n), -1);
        if (n != 2) t::fail(__FILE__, __LINE__, std::string("line 2 for ") + line);
    }
    int n = -1;
    CHECK_EQ(parse("turn 1000\nturn -1000\nwait 10000", st, &n), 3);
}

TEST(script_limits) {
    input::Step st[input::kScriptLines];
    int bad = -1;
    std::string text;
    for (int i = 0; i < input::kScriptLines; i++) text += "side\n";
    CHECK_EQ(parse(text, st, &bad), input::kScriptLines);
    CHECK_EQ(parse(text + "#", st, &bad), -1);
    CHECK_EQ(bad, input::kScriptLines + 1);
    // Over 1 KB: line 0, whatever the lines.
    std::string big(input::kScriptBytes, ' ');
    big[0] = 'p', big[1] = 'r', big[2] = 'e', big[3] = 's', big[4] = 's';
    CHECK_EQ(parse(big, st, &bad), 1);
    CHECK_EQ(parse(big + " ", st, &bad), -1);
    CHECK_EQ(bad, 0);
}

namespace {
// The UI loop's input: the hardware's events from the buttons, then one script step, every 5 ms.
struct Loop {
    input::Buttons b;
    input::Injector inj;
    uint32_t now = 0;
    int finished = 0;
    std::vector<ui::Event> got;
    void run(uint32_t ms, bool knob, int detents = 0, ui::View view = ui::View::Home) {
        for (uint32_t i = 0; i < ms; i += 5) {
            input::EventQueue q;
            b.step(now, knob, false, i == 0 ? detents : 0, q);
            if (inj.take(now, view, q)) finished++;
            for (int k = 0; k < q.n; k++) got.push_back(q.ev[k]);
            now += 5;
        }
    }
};

input::Step ev(ui::EventType t, int n = 0) { return {false, t, static_cast<int16_t>(n)}; }
input::Step pause(int ms) { return {true, ui::EventType::Turn, static_cast<int16_t>(ms)}; }
}  // namespace

TEST(inject_order_with_hardware) {
    Loop l;
    const input::Step script[] = {ev(ui::EventType::Turn, 2), ev(ui::EventType::Side), pause(20),
                                  ev(ui::EventType::Press)};
    CHECK(l.inj.load(script, 4));
    CHECK(l.inj.busy());
    // Same pass as a hardware detent: the hardware's event first, then one step per pass.
    l.run(5, false, -1);
    CHECK_EQ(l.got.size(), 2u);
    CHECK(l.got[0].type == ui::EventType::Turn && l.got[0].n == -1);
    CHECK(l.got[1].type == ui::EventType::Turn && l.got[1].n == 2);
    CHECK(!l.inj.load(script, 1));  // still taking the first
    l.run(5, false);
    CHECK_EQ(l.got.size(), 3u);
    CHECK(l.got[2].type == ui::EventType::Side);
    CHECK_EQ(l.inj.taken(), 2);
    // The pause, from the third pass, holds the press back for 20 ms and counts as taken when it is over.
    l.run(20, false);
    CHECK_EQ(l.got.size(), 3u);
    CHECK_EQ(l.inj.taken(), 2);
    l.run(5, false);
    CHECK_EQ(l.inj.taken(), 3);
    CHECK_EQ(l.finished, 0);
    l.run(5, false);
    CHECK_EQ(l.got.size(), 4u);
    CHECK(l.got[3].type == ui::EventType::Press);
    CHECK_EQ(l.finished, 1);
    CHECK(!l.inj.busy());
    CHECK_EQ(l.inj.taken(), 4);
    l.run(50, false);
    CHECK_EQ(l.finished, 1);
    CHECK(l.inj.load(script, 1));
}

TEST(inject_waits_for_room) {
    // A queue the hardware has filled holds the step until the next pass.
    input::Injector inj;
    const input::Step script[] = {ev(ui::EventType::Press)};
    CHECK(inj.load(script, 1));
    input::EventQueue q;
    for (int i = 0; i < input::EventQueue::kMax; i++) q.push(ui::EventType::Turn, 1);
    CHECK(!inj.take(0, ui::View::Home, q));
    CHECK_EQ(inj.taken(), 0);
    input::EventQueue q2;
    CHECK(inj.take(5, ui::View::Home, q2));
    CHECK_EQ(q2.n, 1);
}

TEST(inject_dropped_on_confirm) {
    // An uploaded image waits for the knob: injected events are taken and dropped, the real press installs.
    Sim sim;
    sim.tick(3000);
    sim.ui.confirmInstall(Sim::kBoot + 3000);
    input::Injector inj;
    const input::Step script[] = {ev(ui::EventType::Press), ev(ui::EventType::LongPress), ev(ui::EventType::Side),
                                  ev(ui::EventType::Turn, 1)};
    CHECK(inj.load(script, 4));
    uint32_t t = 3100;
    for (int pass = 0; pass < 4; pass++, t += 10) {
        input::EventQueue q;
        inj.take(t, sim.ui.view(), q);
        CHECK_EQ(q.n, 0);
        for (int k = 0; k < q.n; k++) sim.ev(t, q.ev[k].type, q.ev[k].n);
    }
    CHECK(!inj.busy());
    CHECK(sim.ui.view() == ui::View::Confirm);
    CHECK_EQ(sim.commands[static_cast<int>(radio::Cmd::Install)], 0);
    CHECK_EQ(sim.commands[static_cast<int>(radio::Cmd::CancelInstall)], 0);
    Loop l;
    l.run(50, false);
    l.run(100, true);
    l.run(50, false);
    CHECK_EQ(l.got.size(), 1u);
    for (const ui::Event& e : l.got) sim.ev(t, e.type, e.n);
    CHECK_EQ(sim.commands[static_cast<int>(radio::Cmd::Install)], 1);
    // Away from the confirm screen the same script reaches the UI.
    CHECK(inj.load(script, 1));
    input::EventQueue q;
    inj.take(t, ui::View::Home, q);
    CHECK_EQ(q.n, 1);
}
