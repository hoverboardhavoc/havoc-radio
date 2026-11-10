// The lamp's output: off, the light level, or the level flashing SOS in Morse code. Pure: compiled into the
// firmware and the host tests; lamp.cpp drives the lamp from it on its own timer.
#pragma once
#include <cstdint>

#include "radio_model.h"

namespace lamp {

// SOS (... --- ...): a dot is 1 unit on and a dash 3; 1 unit off inside a letter, 3 between letters and 7 after the
// last letter, then it repeats. It starts with the first dot.
constexpr uint32_t kSosUnitMs = 300;
constexpr uint32_t kSosUnits = 34;
constexpr uint32_t kSosCycleMs = kSosUnits * kSosUnitMs;

// Whether SOS is lit `t` ms after it started; *next is the time of the next change, after t.
bool sosLit(uint32_t t, uint32_t* next);

// The output level, 0 to 15: 0 off, the level on, and in SOS the level during a flash. `t` is the time since SOS
// started.
int output(radio::Lamp m, int level, uint32_t t);

}  // namespace lamp
