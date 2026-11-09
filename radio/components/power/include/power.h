// Battery reading every 10 s; three readings in a row below the empty level mean the battery is empty.
#pragma once

namespace power {

constexpr int kIntervalMs = 10000;
constexpr int kEmptyReadings = 3;

void start();
int raw();
int percent();  // 0..100 over the usable range
bool empty();

// Percent from a raw reading.
int percentFor(int raw);

}  // namespace power
