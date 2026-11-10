// The radio's UTC clock and the rule for the SNTP client. Pure: the firmware feeds the clock from the SNTP sync
// callback and the monotonic timer (clock.h), the host tests feed it directly.
#pragma once
#include <cstddef>
#include <cstdint>

namespace clk {

// UTC from the syncs and the monotonic time (ms since power-up). The clock is set from the first sync until the
// radio powers off; between syncs it runs on the monotonic time.
class Core {
public:
    // A sync: the server's UTC in ms since 1970 at monotonic time `mono`.
    void synced(int64_t utcMs, int64_t mono);
    bool isSet() const { return set_; }
    // UTC in ms at monotonic time `mono`, -1 until the first sync.
    int64_t utcMs(int64_t mono) const { return set_ ? utc_ + (mono - mono_) : -1; }
    // UTC of the last sync, -1 before the first.
    int64_t lastSyncMs() const { return set_ ? utc_ : -1; }
    // Time since the last sync at monotonic time `mono`, -1 before the first.
    int64_t syncAgeMs(int64_t mono) const { return set_ ? mono - mono_ : -1; }

private:
    bool set_ = false;
    int64_t utc_ = 0;   // UTC at the last sync
    int64_t mono_ = 0;  // monotonic time of the last sync
};

// "13:05" for a UTC time in ms; `out` has room for 6 bytes.
void hhmm(int64_t utcMs, char* out);

// The SNTP client runs while time sync is on and the radio is joined in station mode with an address. update()
// takes the current conditions and the server and answers what to do with the client: start it, stop it, or
// restart it when the server changed while it runs.
enum class Sntp : uint8_t { None, Start, Stop, Restart };

class SntpRule {
public:
    static constexpr size_t kServerMax = 63;
    Sntp update(bool sync, bool joined, bool ap, bool hasIp, const char* server);
    bool running() const { return running_; }
    const char* server() const { return server_; }

private:
    bool running_ = false;
    char server_[kServerMax + 1] = {};
};

}  // namespace clk
