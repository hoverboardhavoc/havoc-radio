// Encoder and buttons: quadrature decoding in a GPIO interrupt, buttons polled by the UI loop.
#pragma once
#include <cstdint>

#include "input_logic.h"

namespace input {

void start();
// Call every few milliseconds: collects detents from the interrupt, debounces the buttons and emits events.
void poll(uint32_t now, EventQueue& out);

}  // namespace input
