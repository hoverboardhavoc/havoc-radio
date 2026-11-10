// The UTC clock, the rule that starts and stops the SNTP client, and the NTP server check.
#include <string>

#include "clock_core.h"
#include "radio_model.h"
#include "test.h"

using clk::Sntp;

namespace {
constexpr int64_t kNoon = 1794268800000LL + 12 * 3600000LL;  // 2026-11-10 12:00 UTC
std::string hm(int64_t ms) {
    char out[6];
    clk::hhmm(ms, out);
    return out;
}
}  // namespace

TEST(clock_not_set_before_first_sync) {
    clk::Core c;
    CHECK(!c.isSet());
    CHECK_EQ(c.utcMs(5000), -1);
    CHECK_EQ(c.lastSyncMs(), -1);
    CHECK_EQ(c.syncAgeMs(5000), -1);
}

TEST(clock_set_by_sync_and_runs_on) {
    clk::Core c;
    c.synced(kNoon, 8000);
    CHECK(c.isSet());
    CHECK_EQ(c.utcMs(8000), kNoon);
    CHECK_EQ(c.utcMs(8000 + 65432), kNoon + 65432);  // UTC advances with the monotonic time
    CHECK_EQ(c.lastSyncMs(), kNoon);
    CHECK_EQ(c.syncAgeMs(8000 + 12 * 60000), 12 * 60000);
    // The next sync moves both the time and the last sync.
    c.synced(kNoon + 3600000 + 250, 8000 + 3600000);
    CHECK_EQ(c.utcMs(8000 + 3600000), kNoon + 3600250);
    CHECK_EQ(c.lastSyncMs(), kNoon + 3600250);
    CHECK_EQ(c.syncAgeMs(8000 + 3600000), 0);
}

TEST(clock_hhmm) {
    CHECK_STR(hm(kNoon), "12:00");
    CHECK_STR(hm(kNoon + 65 * 60000 + 59999), "13:05");
    CHECK_STR(hm(kNoon - 12 * 3600000 - 1), "23:59");
    CHECK_STR(hm(kNoon + 12 * 3600000), "00:00");
}

TEST(sntp_runs_only_with_sync_station_and_address) {
    // Every combination: the client runs only with sync on, joined, no access point and an address.
    for (int m = 0; m < 16; m++) {
        const bool sync = m & 1, joined = m & 2, ap = m & 4, ip = m & 8;
        clk::SntpRule r;
        const Sntp a = r.update(sync, joined, ap, ip, "pool.ntp.org");
        const bool want = sync && joined && !ap && ip;
        CHECK(a == (want ? Sntp::Start : Sntp::None));
        CHECK_EQ(r.running(), want);
    }
}

TEST(sntp_stops_when_a_condition_ends) {
    const char* srv = "pool.ntp.org";
    // Time sync turned off.
    clk::SntpRule r;
    CHECK(r.update(true, true, false, true, srv) == Sntp::Start);
    CHECK(r.update(true, true, false, true, srv) == Sntp::None);
    CHECK(r.update(false, true, false, true, srv) == Sntp::Stop);
    CHECK(r.update(false, true, false, true, srv) == Sntp::None);
    CHECK(r.update(true, true, false, true, srv) == Sntp::Start);
    // The network lost: no longer joined, and the address gone.
    CHECK(r.update(true, false, false, false, srv) == Sntp::Stop);
    CHECK(r.update(true, true, false, false, srv) == Sntp::None);  // joined, still waiting for an address
    CHECK(r.update(true, true, false, true, srv) == Sntp::Start);
    // The access point started.
    CHECK(r.update(true, false, true, true, srv) == Sntp::Stop);
    CHECK(!r.running());
}

TEST(sntp_restarts_on_a_new_server) {
    clk::SntpRule r;
    CHECK(r.update(true, true, false, true, "pool.ntp.org") == Sntp::Start);
    CHECK(r.update(true, true, false, true, "ntp.example.net") == Sntp::Restart);
    CHECK_STR(r.server(), "ntp.example.net");
    // A server changed while the client is stopped is used on the next start.
    CHECK(r.update(false, true, false, true, "ntp.example.net") == Sntp::Stop);
    CHECK(r.update(false, true, false, true, "10.0.0.1") == Sntp::None);
    CHECK(r.update(true, true, false, true, "10.0.0.1") == Sntp::Start);
    CHECK_STR(r.server(), "10.0.0.1");
}

TEST(ntp_server_check) {
    CHECK(radio::validNtpServer("pool.ntp.org"));
    CHECK(radio::validNtpServer("192.168.0.1"));
    CHECK(radio::validNtpServer("NTP-1.Example.COM"));
    CHECK(radio::validNtpServer(""));  // the default
    CHECK(radio::validNtpServer(std::string(63, 'a').c_str()));
    CHECK(!radio::validNtpServer(std::string(64, 'a').c_str()));
    for (const char* bad : {"pool ntp", "ntp_1", "a/b", "ntp:123", "x@y", "\xc3\xa6.no", "a,b"})
        CHECK(!radio::validNtpServer(bad));
}

TEST(ntp_server_setting) {
    radio::State s;
    radio::defaults(s);
    CHECK(s.timeSync);
    CHECK_STR(s.ntpServer, "pool.ntp.org");
    CHECK(radio::setNtpServer(s, "ntp.example.net"));
    CHECK_STR(s.ntpServer, "ntp.example.net");
    CHECK(!radio::setNtpServer(s, "ntp.example.net"));  // unchanged
    CHECK(!radio::setNtpServer(s, "bad server"));
    CHECK_STR(s.ntpServer, "ntp.example.net");
    CHECK(radio::setNtpServer(s, ""));  // empty sets the default
    CHECK_STR(s.ntpServer, "pool.ntp.org");
    CHECK(radio::apply(s, {radio::Cmd::TimeSync, 0}, true));
    CHECK(!s.timeSync);
    CHECK(!radio::apply(s, {radio::Cmd::TimeSync, 0}, true));
}
