#include "lamp_core.h"

namespace lamp {

namespace {
// Alternating on and off runs, in units, starting with the first dot's on.
constexpr uint8_t kRuns[] = {1, 1, 1, 1, 1, 3, 3, 1, 3, 1, 3, 3, 1, 1, 1, 1, 1, 7};
}  // namespace

bool sosLit(uint32_t t, uint32_t* next) {
    const uint32_t cycle = t / kSosCycleMs * kSosCycleMs;
    uint32_t end = cycle;
    for (unsigned i = 0;; i++) {
        end += kRuns[i] * kSosUnitMs;
        if (t < end) {
            *next = end;
            return i % 2 == 0;
        }
    }
}

int output(radio::Lamp m, int level, uint32_t t) {
    uint32_t next;
    if (m == radio::Lamp::On || (m == radio::Lamp::Sos && sosLit(t, &next))) return level;
    return 0;
}

}  // namespace lamp
