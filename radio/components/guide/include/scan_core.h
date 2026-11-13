// The guide scan (pure): the frequencies a scan measures, read from the guide table one at a time, and the state of a
// scan with its 40 strongest results. The receiver work is the radio task's; the host tests run this core on tables
// made by the converter.
#pragma once
#include <cstdint>

#include "guide_core.h"

namespace scan {

// TODO: keep every result instead of the 40 strongest (a Scan all measures up to about 1,900 frequencies), and an
// optional filter that hides results with an SNR of 0 dB, on the radio's results list and the web page's Scan card.
constexpr int kMax = 40;            // results kept
constexpr uint32_t kSettleMs = 60;  // from the tune to the signal reading

enum class Scope : uint8_t { Band, All };
enum class Phase : uint8_t { None, Running, Done };

// A measured frequency: RSSI in dBuV, SNR in dB, and the string index of its first on-air name in the guide table.
struct Result {
    uint16_t khz;
    uint8_t rssi;
    uint8_t snr;
    uint16_t name;
};

// The strongest results by RSSI, ties by SNR, earlier first; a measurement of RSSI 0 is not kept.
struct Results {
    int n = 0;
    Result r[kMax];
    void add(const Result& x);
};

struct State {
    Phase phase = Phase::None;
    Scope scope = Scope::All;
    int8_t band = -1;          // the band scanned (radio::kBands), -1 for Scan all
    uint16_t id = 0;           // changes with every scan started
    int32_t loKhz = 0, hiKhz = 0;
    int64_t utcMs = -1;        // the time the on-air rule runs at; negative without the clock
    int32_t returnHz = 0;      // the frequency the scan started on
    uint32_t generation = 0;   // the guide table the names are read from
    uint16_t total = 0;        // frequencies to measure, once counted
    uint16_t done = 0;         // measured
    uint16_t khz = 0;          // the last frequency tuned, 0 before the first
    Results results;
};

// A new scan: running, with no results.
void start(State& s, Scope scope, int band, int32_t loKhz, int32_t hiKhz, int32_t returnHz, int64_t utcMs);
// A frequency measured while the scan runs.
void measured(State& s, const Result& r);
// Stopped with the results so far.
void stop(State& s);
// Cancelled: no results.
void cancel(State& s);

// The frequencies of a scan: every frequency of the table, rounded to 1 kHz and each once, within lo..hi kHz with an
// entry on air at utcMs, or with any entry (irregular ones included) when utcMs is negative; ascending.
struct Cursor {
    uint32_t i = 0;  // the next entry to read
    int32_t loKhz = 0, hiKhz = 0;
    int64_t utcMs = -1;
};
// Positions the cursor at the first entry of the range.
void first(const guide::Source& src, const guide::Info& info, Cursor& c, int32_t loKhz, int32_t hiKhz, int64_t utcMs);
// The next frequency and the name of its first qualifying entry in table order; false at the end.
bool next(const guide::Source& src, const guide::Info& info, Cursor& c, uint16_t& khz, uint16_t& name);

}  // namespace scan
