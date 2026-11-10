#include "clock_core.h"

#include <cstring>

namespace clk {

void Core::synced(int64_t utcMs, int64_t mono) {
    set_ = true;
    utc_ = utcMs;
    mono_ = mono;
}

void hhmm(int64_t utcMs, char* out) {
    constexpr int64_t kDayMin = 24 * 60;
    const int m = static_cast<int>(((utcMs / 60000) % kDayMin + kDayMin) % kDayMin);
    out[0] = static_cast<char>('0' + m / 600);
    out[1] = static_cast<char>('0' + m / 60 % 10);
    out[2] = ':';
    out[3] = static_cast<char>('0' + m % 60 / 10);
    out[4] = static_cast<char>('0' + m % 10);
    out[5] = 0;
}

Sntp SntpRule::update(bool sync, bool joined, bool ap, bool hasIp, const char* server) {
    const bool want = sync && joined && !ap && hasIp;
    const bool changed = std::strncmp(server_, server, kServerMax) != 0;
    if (changed) {
        std::strncpy(server_, server, kServerMax);
        server_[kServerMax] = 0;
    }
    if (want == running_) return want && changed ? Sntp::Restart : Sntp::None;
    running_ = want;
    return want ? Sntp::Start : Sntp::Stop;
}

}  // namespace clk
