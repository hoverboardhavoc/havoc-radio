// Encoder and buttons: quadrature decoding in a GPIO interrupt, buttons polled by the UI loop; input scripts from
// the web server go into the same queue.
#pragma once
#include <cstdint>

#include "input_logic.h"

namespace input {

void start();
// Call every few milliseconds: collects detents from the interrupt, debounces the buttons and emits events.
void poll(uint32_t now, EventQueue& out);

// Input scripts (POST /api/input). inject runs in the web server's task: it queues a script's steps for the UI loop
// and returns false while an earlier script is still being taken. waitInjected waits up to `ms` for the UI loop to
// take the last step and draw a frame after it; *taken is the number of steps taken.
bool inject(const Step* steps, int n);
bool waitInjected(uint32_t ms, int* taken);
// In the UI loop: after poll, the next script step for the screen in `view`; after a frame is drawn, drawn().
void takeInjected(uint32_t now, ui::View view, EventQueue& out);
void drawn();

}  // namespace input
