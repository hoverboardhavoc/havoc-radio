// The lamp's output and the SOS pattern.
#include <vector>

#include "lamp_core.h"
#include "test.h"

using radio::Lamp;

TEST(sos_pattern_two_cycles) {
    // Expected changes in ms from the start, lit from 0: S (three dots), 3 units, O (three dashes), 3 units,
    // S, 7 units; twice.
    const uint32_t u = lamp::kSosUnitMs;
    const uint32_t cycle[] = {1, 2, 3, 4, 5, 8, 11, 12, 15, 16, 19, 22, 23, 24, 25, 26, 27, 34};
    std::vector<uint32_t> want;
    for (int c = 0; c < 2; c++)
        for (uint32_t e : cycle) want.push_back((c * 34 + e) * u);
    CHECK_EQ(lamp::kSosCycleMs, 34 * u);
    // Walk the pattern edge by edge, as the timer does.
    std::vector<uint32_t> got;
    uint32_t t = 0;
    bool lit = true;
    while (got.size() < want.size()) {
        uint32_t next;
        CHECK(lamp::sosLit(t, &next) == lit);
        CHECK(lamp::sosLit(next - 1, &next) == lit);  // unchanged up to the edge
        got.push_back(next);
        t = next;
        lit = !lit;
    }
    CHECK(got == want);
    // The state at a few times in between.
    uint32_t n;
    CHECK(lamp::sosLit(150, &n) && n == 300);
    CHECK(!lamp::sosLit(1600, &n) && n == 2400);  // the gap after the first S
    CHECK(lamp::sosLit(2500, &n) && n == 3300);   // the first dash
    CHECK(!lamp::sosLit(10000, &n) && n == 10200);
    CHECK(lamp::sosLit(10200, &n) && n == 10500);  // the second cycle starts with a dot
}

TEST(lamp_output) {
    CHECK_EQ(lamp::output(Lamp::Off, 15, 0), 0);
    CHECK_EQ(lamp::output(Lamp::On, 9, 0), 9);
    CHECK_EQ(lamp::output(Lamp::On, 9, 1600), 9);
    CHECK_EQ(lamp::output(Lamp::Sos, 9, 0), 9);
    CHECK_EQ(lamp::output(Lamp::Sos, 9, 300), 0);
    CHECK_EQ(lamp::output(Lamp::Sos, 4, 2500), 4);
}
