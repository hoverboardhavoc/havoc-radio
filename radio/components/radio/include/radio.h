// The radio: owns the model state (under a mutex) and a task that keeps the Si4732 in step with it.
#pragma once
#include "radio_model.h"
#include "rds.h"

namespace radio {

struct Live {
    uint8_t rssi = 0;  // dBuV
    uint8_t snr = 0;   // dB
    uint8_t bars = 0;  // 0..5
    bool seeking = false;
    int8_t seekDir = 1;
    bool ssb = false;   // SSB patch available
    bool ready = false; // receiver powered up
};

void start(const State& initial);
// Applies a command to the model and wakes the radio task. Returns true when the stored state changed.
bool submit(const Command& c);
// Changes the model directly (web API preset editing); `fn` runs under the lock.
bool edit(bool (*fn)(State&, void*), void* arg);
void snapshot(State& out);
Live live();
// The RDS decoder's state; empty outside FM.
void rdsState(rds::Decoder& out);
// The last groups received, oldest first, as many as fit in `max`. Returns the count.
int rdsGroups(rds::Raw* out, int max);
// Battery empty: amplifier muted and the receiver powered down; the task stops.
void shutdown();

// Signal bars from RSSI, by mode family.
uint8_t barsFor(uint8_t rssi, bool fm);

}  // namespace radio
