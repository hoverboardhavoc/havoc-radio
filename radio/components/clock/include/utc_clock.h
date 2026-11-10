// The radio's UTC clock on the target: set by the SNTP client's sync callback, read with the monotonic timer.
// The net module starts and stops the client; this module only keeps the time.
#pragma once
#include <cstdint>

namespace clk {

// Registers the SNTP sync callback. Call before the network starts.
void init();

struct Now {
    int64_t utcMs = -1;       // UTC in ms since 1970, -1 until the first sync
    int64_t lastSyncMs = -1;  // UTC of the last sync, -1 before the first
    int64_t syncAgeMs = -1;   // time since the last sync, -1 before the first
};
Now now();

}  // namespace clk
