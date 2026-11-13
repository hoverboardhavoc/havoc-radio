#include "scan_core.h"

namespace scan {

namespace {

// The entry's frequency rounded to whole kHz.
int32_t roundedKhz(const guide::Entry& e) { return (e.khz * 10 + e.tenths + 5) / 10; }

}  // namespace

void Results::add(const Result& x) {
    if (!x.rssi) return;
    int at = n;
    while (at > 0 && (r[at - 1].rssi < x.rssi || (r[at - 1].rssi == x.rssi && r[at - 1].snr < x.snr))) at--;
    if (at >= kMax) return;
    for (int k = n < kMax ? n : kMax - 1; k > at; k--) r[k] = r[k - 1];
    r[at] = x;
    if (n < kMax) n++;
}

void start(State& s, Scope scope, int band, int32_t loKhz, int32_t hiKhz, int32_t returnHz, int64_t utcMs) {
    s.phase = Phase::Running;
    s.scope = scope;
    s.band = static_cast<int8_t>(band);
    s.id = static_cast<uint16_t>(s.id + 1);
    s.loKhz = loKhz;
    s.hiKhz = hiKhz;
    s.utcMs = utcMs;
    s.returnHz = returnHz;
    s.generation = 0;
    s.total = s.done = s.khz = 0;
    s.results.n = 0;
}

void measured(State& s, const Result& r) {
    if (s.phase != Phase::Running) return;
    s.khz = r.khz;
    s.done++;
    s.results.add(r);
}

void stop(State& s) {
    if (s.phase == Phase::Running) s.phase = Phase::Done;
}

void cancel(State& s) {
    s.phase = Phase::None;
    s.results.n = 0;
}

void first(const guide::Source& src, const guide::Info& info, Cursor& c, int32_t loKhz, int32_t hiKhz, int64_t utcMs) {
    c.loKhz = loKhz;
    c.hiKhz = hiKhz;
    c.utcMs = utcMs;
    c.i = info.valid ? info.entries : 0;
    if (!info.valid) return;
    // The first entry that rounds to lo or above, by binary search on the frequency.
    uint32_t lo = 0, hi = info.entries;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        guide::Entry e;
        if (!guide::readEntry(src, mid, e)) return;
        if (roundedKhz(e) < loKhz)
            lo = mid + 1;
        else
            hi = mid;
    }
    c.i = lo;
}

bool next(const guide::Source& src, const guide::Info& info, Cursor& c, uint16_t& khz, uint16_t& name) {
    if (!info.valid) return false;
    while (c.i < info.entries) {
        guide::Entry e;
        if (!guide::readEntry(src, c.i, e)) break;
        const int32_t k = roundedKhz(e);
        if (k > c.hiKhz) break;
        // The entries that round to k follow one another; the first that qualifies names the frequency.
        bool found = false;
        uint16_t nm = 0;
        for (;;) {
            if (!found && (c.utcMs < 0 || guide::onAir(e, c.utcMs))) {
                found = true;
                nm = e.name;
            }
            c.i++;
            if (c.i >= info.entries || !guide::readEntry(src, c.i, e) || roundedKhz(e) != k) break;
        }
        if (found) {
            khz = static_cast<uint16_t>(k);
            name = nm;
            return true;
        }
    }
    c.i = info.entries;
    return false;
}

}  // namespace scan
