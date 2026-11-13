// The guide scan's core on the test tables scan.bin and b26.bin (radio/test/guide/): the frequency set (on air,
// without the clock, the scope's limits, duplicates and 0.1 kHz entries rounded), the 40 strongest with ties, stop
// and cancel, the frequency the scan started on; and in the UI, the commands for each way out of a scan.
#include <string>

#include "guide_core.h"
#include "scan_core.h"
#include "sim.h"
#include "util.h"

using ui::EventType;
using ui::ListId;
using ui::View;

namespace {

constexpr int64_t kTue = 1794268800000LL;  // 2026-11-10 00:00 UTC, a Tuesday
constexpr int64_t kMin = 60000, kHour = 3600000, kDay = 86400000;

struct Table {
    std::string bytes;
    guide::Info info;
    explicit Table(const char* name) : bytes(readFile(t::root() + "/radio/test/guide/" + name)) {
        CHECK(guide::check(guide::MemSource(bytes.data(), bytes.size()), bytes.size(), info) == guide::Err::Ok);
    }
    // "kHz name" for each frequency the scan measures, joined with "|".
    std::string freqs(int32_t lo, int32_t hi, int64_t utc) const {
        const guide::MemSource src(bytes.data(), bytes.size());
        scan::Cursor c;
        scan::first(src, info, c, lo, hi, utc);
        std::string s;
        uint16_t khz, name;
        char text[guide::kStrMax + 1];
        while (scan::next(src, info, c, khz, name)) {
            guide::readString(src, info, name, text);
            s += (s.empty() ? "" : "|") + std::to_string(khz) + " " + text;
        }
        return s;
    }
    int count(int32_t lo, int32_t hi, int64_t utc) const {
        const guide::MemSource src(bytes.data(), bytes.size());
        scan::Cursor c;
        scan::first(src, info, c, lo, hi, utc);
        int n = 0;
        uint16_t khz, name;
        while (scan::next(src, info, c, khz, name)) n++;
        return n;
    }
};

std::string results(const scan::Results& r) {
    std::string s;
    for (int i = 0; i < r.n; i++)
        s += (i ? "|" : "") + std::to_string(r.r[i].khz) + ":" + std::to_string(r.r[i].rssi) + "/" +
             std::to_string(r.r[i].snr);
    return s;
}

}  // namespace

TEST(scan_frequencies) {
    const Table s("scan.bin");
    const int64_t at1305 = kTue + 13 * kHour + 5 * kMin;
    // 49m: 5899.5 rounds into the band, 6200.5 out of it; 5940.0, .1 and .4 are one frequency, named by its first entry
    // on air; 5940.5 is 5941; the duplicate at 6000 counts once; irregular and off-air entries are left out.
    CHECK_STR(s.freqs(5900, 6200, at1305), "5900 Edge low|5940 Rounded|5941 Next up|6000 Second|6200 Top");
    // On air by the start day: Tuesday noon on a Tuesday, not on a Wednesday.
    CHECK_STR(s.freqs(5900, 6200, kTue + 12 * kHour + 30 * kMin),
              "5940 Rounded|5941 Next up|6000 Second|6150 Tuesday noon|6200 Top");
    CHECK_STR(s.freqs(5900, 6200, kTue + kDay + 12 * kHour + 30 * kMin), "5940 Rounded|5941 Next up|6000 Second|6200 Top");
    // Without the clock every listed frequency, irregular entries included, named by its first entry.
    CHECK_STR(s.freqs(5900, 6200, -1),
              "5900 Edge low|5940 Later|5941 Next up|6000 Second|6100 Irregular|6150 Tuesday noon|6200 Top");
    // Scan all.
    CHECK_STR(s.freqs(153, 30000, at1305),
              "5899 Below|5900 Edge low|5940 Rounded|5941 Next up|6000 Second|6200 Top|6201 Above|7000 Forty");
    // A range of one frequency, and ranges with nothing in them.
    CHECK_STR(s.freqs(5941, 5941, at1305), "5941 Next up");
    CHECK_STR(s.freqs(7001, 30000, at1305), "");
    CHECK_STR(s.freqs(153, 5898, -1), "");
    // No table: nothing.
    guide::Info none;
    scan::Cursor c;
    uint16_t khz, name;
    scan::first(guide::MemSource(s.bytes.data(), s.bytes.size()), none, c, 153, 30000, -1);
    CHECK(!scan::next(guide::MemSource(s.bytes.data(), s.bytes.size()), none, c, khz, name));
}

TEST(scan_frequencies_b26) {
    // About a thousand frequencies on air at any hour, a few dozen of them on LW and MW.
    const Table b("b26.bin");
    for (int64_t h : {0, 12}) {
        const int all = b.count(153, 30000, kTue + h * kHour), lwmw = b.count(153, 1602, kTue + h * kHour);
        CHECK(all > 900 && all < 1200);
        CHECK(lwmw > 30 && lwmw < 100);
    }
    CHECK(b.count(153, 30000, -1) > b.count(153, 30000, kTue + 12 * kHour));
}

TEST(scan_strongest) {
    scan::Results r;
    // Strongest first by RSSI, ties by SNR, equal ones in the order measured; RSSI 0 is not kept.
    for (const scan::Result& x : {scan::Result{6000, 30, 10, 0}, scan::Result{6010, 0, 0, 0}, scan::Result{6020, 40, 5, 0},
                                  scan::Result{6030, 30, 12, 0}, scan::Result{6040, 30, 10, 0}})
        r.add(x);
    CHECK_STR(results(r), "6020:40/5|6030:30/12|6000:30/10|6040:30/10");
    // 50 measurements: the 40 strongest stay.
    scan::Results m;
    for (int i = 0; i < 50; i++) m.add({static_cast<uint16_t>(5000 + i), static_cast<uint8_t>(1 + (i * 37) % 50), 0, 0});
    CHECK_EQ(m.n, scan::kMax);
    for (int i = 0; i < m.n; i++) CHECK_EQ(static_cast<int>(m.r[i].rssi), 50 - i);
    m.add({7000, 1, 0, 0});
    CHECK_EQ(static_cast<int>(m.r[scan::kMax - 1].rssi), 11);
    m.add({7000, 60, 0, 0});
    CHECK_EQ(static_cast<int>(m.r[0].khz), 7000);
    CHECK_EQ(static_cast<int>(m.r[scan::kMax - 1].rssi), 12);
}

TEST(scan_state) {
    scan::State s;
    scan::start(s, scan::Scope::Band, 9, 5900, 6200, 6000000, kTue);
    CHECK(s.phase == scan::Phase::Running && s.id == 1 && s.band == 9 && s.returnHz == 6000000);
    s.total = 3;
    scan::measured(s, {5900, 20, 3, 1});
    scan::measured(s, {5910, 30, 4, 2});
    CHECK(s.done == 2 && s.khz == 5910 && s.results.n == 2);
    // Stopped: the results so far; later measurements are not counted.
    scan::stop(s);
    CHECK(s.phase == scan::Phase::Done);
    scan::measured(s, {5920, 40, 5, 3});
    CHECK_STR(results(s.results), "5910:30/4|5900:20/3");
    // A new scan starts with no results; cancelled, it has none.
    scan::start(s, scan::Scope::All, -1, 153, 30000, 7100000, -1);
    CHECK(s.id == 2 && s.results.n == 0 && s.done == 0 && s.returnHz == 7100000);
    scan::measured(s, {5920, 40, 5, 3});
    scan::cancel(s);
    CHECK(s.phase == scan::Phase::None && s.results.n == 0);
}

// The UI: Back and a long press return to the frequency the scan started on, a press on a row and the timeout keep the
// selected one; the knob does not tune on home while a scan runs.
TEST(scan_ui_frequencies) {
    Sim m;
    m.signal = {{6070, {48, 21}}, {5900, {41, 17}}};
    m.ntp(2900, kTue + 13 * kHour + 5 * kMin);
    m.ev(3000, EventType::Turn, -14);
    CHECK_EQ(m.s.hz, 6000000);
    uint32_t t = 3000;
    const auto openSeek = [&] {
        m.ev(t += 100, EventType::Press);
        m.ev(t += 100, EventType::Turn, -5);
        m.ev(t += 100, EventType::Press);
        CHECK(m.ui.listId() == ListId::Seek);
    };
    openSeek();
    m.ev(t += 100, EventType::Turn, 3);  // Scan all
    m.ev(t += 100, EventType::Press);
    CHECK(m.ui.view() == View::Scanning && m.scanState.phase == scan::Phase::Running);
    // Turning does nothing; Back stops the scan, and the results tune to the first row.
    m.ev(t += 100, EventType::Turn, 2);
    CHECK_EQ(m.s.hz, 6000000);
    m.scanStep(3);
    m.ev(t += 100, EventType::Side);
    m.tick(t += 100);
    CHECK(m.ui.view() == View::List && m.ui.listId() == ListId::ScanResults && m.scanState.phase == scan::Phase::Done);
    CHECK_EQ(m.s.hz, 5900000);
    m.ev(t += 100, EventType::Side);
    CHECK(m.ui.view() == View::Home);
    CHECK_EQ(m.s.hz, 6000000);
    // Last scan, a turn to no row (Back) leaves the frequency, a long press returns.
    openSeek();
    m.ev(t += 100, EventType::Turn, 4);
    m.ev(t += 100, EventType::Press);
    CHECK(m.ui.listId() == ListId::ScanResults && m.ui.depth() == 2);
    CHECK_EQ(m.s.hz, 5900000);
    m.ev(t += 100, EventType::Turn, -1);
    CHECK_EQ(m.ui.cursor(), -1);
    CHECK_EQ(m.s.hz, 5900000);
    m.ev(t += 100, EventType::LongPress);
    CHECK(m.ui.view() == View::Home);
    CHECK_EQ(m.s.hz, 6000000);
    // The timeout keeps the selected row.
    openSeek();
    m.ev(t += 100, EventType::Turn, 4);
    m.ev(t += 100, EventType::Press);
    m.tick(t += ui::kMenuTimeoutMs + 1);
    CHECK(m.ui.view() == View::Home);
    CHECK_EQ(m.s.hz, 5900000);
    // A scan started elsewhere (the web page): the knob does not tune on home until it ends.
    m.startScan(scan::Scope::All);
    m.ev(t += 100, EventType::Turn, 1);
    CHECK_EQ(m.s.hz, 5900000);
    m.scanStep(10);
    m.ev(t += 100, EventType::Turn, 1);
    CHECK_EQ(m.s.hz, 5905000);
}
