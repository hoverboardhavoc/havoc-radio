// Navigation rules: selection ring, back, timeouts, long press, hold + turn and text entry.
#include <cstring>
#include "sim.h"
#include "test.h"

using ui::EventType;
using ui::ListId;
using ui::View;

namespace {
// A Sim past the splash, with the main menu open at t = 3000.
struct Menu : Sim {
    Menu() { ev(3000, EventType::Press); }
};
}  // namespace

TEST(splash_ends_or_skips) {
    Sim a;
    a.tick(2600);
    CHECK(a.ui.view() == View::Splash);
    a.tick(2601);
    CHECK(a.ui.view() == View::Home);
    Sim b;
    b.ev(100, EventType::Press);  // the input only skips the splash
    CHECK(b.ui.view() == View::Home);
}

TEST(ring_order_main_menu) {
    Menu m;
    CHECK(m.ui.view() == View::List && m.ui.listId() == ListId::Main);
    CHECK_EQ(m.ui.cursor(), 0);
    // Clockwise: down the eight entries (Volume to Settings), then Mute, then Back, then the first entry.
    const int expect[] = {1, 2, 3, 4, 5, 6, 7, -2, -1, 0, 1};
    uint32_t t = 3100;
    for (int e : expect) {
        m.ev(t += 100, EventType::Turn, 1);
        CHECK_EQ(m.ui.cursor(), e);
    }
    m.ev(t += 100, EventType::Turn, -2);
    CHECK_EQ(m.ui.cursor(), -1);
    // Anticlockwise: Back, Mute, Settings and up the entries.
    const int back[] = {-2, 7, 6, 5, 4, 3, 2, 1, 0, -1};
    for (int e : back) {
        m.ev(t += 100, EventType::Turn, -1);
        CHECK_EQ(m.ui.cursor(), e);
    }
}

TEST(ring_order_main_menu_fm) {
    Sim f;
    f.s.mode = radio::Mode::FM;
    f.s.hz = 100700000;
    f.ev(3000, EventType::Press);
    CHECK_EQ(f.ui.cursor(), 0);
    // Station sits between Seek and Settings: nine entries.
    const int expect[] = {1, 2, 3, 4, 5, 6, 7, 8, -2, -1, 0};
    uint32_t t = 3000;
    for (int e : expect) {
        f.ev(t += 100, EventType::Turn, 1);
        CHECK_EQ(f.ui.cursor(), e);
    }
    const int back[] = {-1, -2, 8, 7};
    for (int e : back) {
        f.ev(t += 100, EventType::Turn, -1);
        CHECK_EQ(f.ui.cursor(), e);
    }
    f.ev(t += 100, EventType::Press);
    CHECK(f.ui.view() == View::Station);
}

TEST(main_menu_entries) {
    // Opens on Volume: a press opens the volume screen.
    Menu v;
    v.ev(3100, EventType::Press);
    CHECK(v.ui.view() == View::Adjust);
    v.ev(3200, EventType::Turn, 2);
    CHECK_EQ(v.s.volume, 47);
    // Presets opens the presets list on the current preset.
    Menu p;
    p.ev(3100, EventType::Turn, 2);
    p.ev(3200, EventType::Press);
    CHECK(p.ui.listId() == ListId::Presets);
    CHECK_EQ(p.ui.depth(), 1);
    // An empty presets list opens with Back selected.
    Menu e;
    e.s.presetCount = 0;
    e.ev(3100, EventType::Turn, 2);
    e.ev(3200, EventType::Press);
    CHECK(e.ui.listId() == ListId::Presets);
    CHECK_EQ(e.ui.cursor(), -1);
    // Settings, the last entry, opens Settings.
    Menu st;
    st.ev(3100, EventType::Turn, -3);
    st.ev(3200, EventType::Press);
    CHECK(st.ui.listId() == ListId::Settings);
    CHECK_EQ(st.ui.depth(), 1);
    // Mute toggles in place: the menu stays open on Mute.
    Menu m;
    m.ev(3100, EventType::Turn, -2);
    CHECK_EQ(m.ui.cursor(), -2);
    m.ev(3200, EventType::Press);
    CHECK(m.s.muted);
    CHECK(m.ui.view() == View::List && m.ui.listId() == ListId::Main);
    CHECK_EQ(m.ui.cursor(), -2);
    m.ev(3300, EventType::Press);
    CHECK(!m.s.muted);
}

TEST(back_and_depth) {
    Menu m;
    m.ev(3050, EventType::Turn, 1);
    m.ev(3100, EventType::Press);  // Band list
    CHECK(m.ui.listId() == ListId::Bands);
    CHECK_EQ(m.ui.depth(), 1);
    CHECK_EQ(m.ui.cursor(), 9);  // 49m preselected
    CHECK_EQ(m.ui.top(), 8);
    m.ev(3200, EventType::Turn, 16);  // past the last band to Back
    CHECK_EQ(m.ui.cursor(), -1);
    CHECK_EQ(m.ui.top(), 0);
    m.ev(3300, EventType::Press);
    CHECK(m.ui.listId() == ListId::Main);
    CHECK_EQ(m.ui.depth(), 0);
    m.ev(3400, EventType::Turn, -2);
    m.ev(3500, EventType::Press);  // Back from the main menu goes home
    CHECK(m.ui.view() == View::Home);
}

TEST(choosing_returns_home) {
    Menu m;
    m.ev(3100, EventType::Turn, 4);
    m.ev(3200, EventType::Press);  // Step list
    m.ev(3300, EventType::Turn, 1);
    m.ev(3400, EventType::Press);
    CHECK(m.ui.view() == View::Home);
    CHECK_STR(radio::currentStep(m.s).label, "9k");
}

TEST(menu_timeout) {
    Menu m;
    m.tick(11000);
    CHECK(m.ui.view() == View::List);
    m.tick(11001);
    CHECK(m.ui.view() == View::Home);
    // Text entry does not time out.
    Menu e;
    e.ev(3100, EventType::Turn, -3);
    e.ev(3200, EventType::Press);  // Settings
    e.ev(3300, EventType::Press);  // WiFi
    e.ev(3400, EventType::Turn, 1);
    e.ev(3500, EventType::Press);  // Lab
    CHECK(e.ui.view() == View::Entry);
    e.tick(60000);
    CHECK(e.ui.view() == View::Entry);
}

TEST(long_press) {
    Sim h;
    h.ev(3000, EventType::LongPress);
    CHECK_EQ(h.s.presetCount, 6);
    CHECK_STR(h.s.presets[5].name, "49m");
    Menu m;
    m.ev(3100, EventType::Press);
    m.ev(3200, EventType::LongPress);
    CHECK(m.ui.view() == View::Home);
    CHECK_EQ(m.s.presetCount, 5);
}

TEST(hold_turn) {
    Sim h;
    h.ev(3000, EventType::HoldTurn, -5);
    CHECK_EQ(h.s.volume, 40);
    h.s.muted = true;
    h.ev(3100, EventType::HoldTurn, 1);
    CHECK(!h.s.muted);
    Menu m;
    m.ev(3050, EventType::Turn, 1);
    m.ev(3100, EventType::Press);  // bands at 9
    m.ev(3200, EventType::HoldTurn, 1);
    CHECK_EQ(m.ui.cursor(), 13);
    m.ev(3300, EventType::HoldTurn, -10);
    CHECK_EQ(m.ui.cursor(), 0);
    m.ev(3400, EventType::Turn, -1);  // Back, then hold + turn lands on the entries
    m.ev(3500, EventType::HoldTurn, 1);
    CHECK_EQ(m.ui.cursor(), 4);
}

TEST(turn_tunes_by_step) {
    Sim h;
    h.ev(3000, EventType::Turn, 3);
    CHECK_EQ(h.s.hz, 6085000);
    h.ev(3100, EventType::Turn, -10000);
    CHECK_EQ(h.s.hz, 150000);
}

TEST(any_input_stops_seek) {
    for (EventType e : {EventType::Turn, EventType::Press, EventType::LongPress, EventType::HoldTurn, EventType::Side,
                        EventType::SideHold}) {
        Sim h;
        h.tick(3000);
        h.st.seeking = true;
        h.ev(3100, e, 1);
        CHECK(!h.st.seeking);
    }
}

TEST(text_entry) {
    Menu e;
    e.ev(3100, EventType::Turn, -3);  // Settings
    e.ev(3200, EventType::Press);
    e.ev(3300, EventType::Press);
    e.ev(3400, EventType::Turn, 1);
    e.ev(3500, EventType::Press);
    CHECK(e.ui.view() == View::Entry);
    e.ev(3600, EventType::Turn, -1);  // the ring wraps: OK
    CHECK_EQ(e.ui.entryIndex(), 85);
    e.ev(3700, EventType::HoldTurn, 1);  // next group wraps to abc
    CHECK_EQ(e.ui.entryIndex(), 0);
    e.ev(3800, EventType::HoldTurn, 2);  // 123
    CHECK_EQ(e.ui.entryIndex(), 52);
    e.ev(3900, EventType::Press);
    e.ev(4000, EventType::HoldTurn, 2);  // edit: SPACE
    e.ev(4100, EventType::Press);
    CHECK_STR(e.ui.entryText(), "0 ");
    e.ev(4200, EventType::Turn, 1);  // DEL
    e.ev(4300, EventType::Press);
    CHECK_STR(e.ui.entryText(), "0");
    e.ev(4350, EventType::HoldTurn, 1);  // abc
    for (int i = 0; i < 70; i++) e.ev(4400 + i, EventType::Press);
    CHECK_EQ(static_cast<int>(std::strlen(e.ui.entryText())), ui::kPasswordMax);
    e.ev(5000, EventType::Turn, -1);  // OK
    e.ev(5100, EventType::Press);
    CHECK(e.ui.view() == View::Home);
    CHECK_STR(e.ui.joinSsid(), "Lab");
    CHECK_EQ(static_cast<int>(std::strlen(e.ui.joinPassword())), ui::kPasswordMax);
}

TEST(side_press_is_back) {
    // Home: nothing.
    Sim h;
    h.ev(3000, EventType::Side);
    CHECK(h.ui.view() == View::Home);
    CHECK(!h.s.muted);
    // A list goes back.
    Menu m;
    m.ev(3050, EventType::Turn, 1);
    m.ev(3100, EventType::Press);
    CHECK_EQ(m.ui.depth(), 1);
    m.ev(3200, EventType::Side);
    CHECK(m.ui.listId() == ListId::Main && m.ui.depth() == 0);
    m.ev(3300, EventType::Side);
    CHECK(m.ui.view() == View::Home);
    // An adjust screen confirms: the volume screen goes home with the new volume.
    Menu v;
    v.ev(3100, EventType::Press);
    v.ev(3200, EventType::Turn, -5);
    v.ev(3300, EventType::Side);
    CHECK(v.ui.view() == View::Home);
    CHECK_EQ(v.s.volume, 40);
    // Info and the Station screen go back.
    Menu i;
    i.ev(3100, EventType::Turn, -3);
    i.ev(3200, EventType::Press);
    i.ev(3300, EventType::Turn, 6);
    i.ev(3400, EventType::Press);  // About
    CHECK(i.ui.view() == View::Info);
    i.ev(3500, EventType::Side);
    CHECK(i.ui.listId() == ListId::Settings);
    Sim f;
    f.s.mode = radio::Mode::FM;
    f.s.hz = 100700000;
    f.ev(3000, EventType::Press);
    f.ev(3100, EventType::Turn, -4);
    f.ev(3200, EventType::Press);
    CHECK(f.ui.view() == View::Station);
    f.ev(3300, EventType::Side);
    CHECK(f.ui.listId() == ListId::Main);
    // Text entry cancels: nothing joined.
    Menu e;
    e.ev(3100, EventType::Turn, -3);
    e.ev(3200, EventType::Press);
    e.ev(3300, EventType::Press);
    e.ev(3400, EventType::Turn, 1);
    e.ev(3500, EventType::Press);
    e.ev(3600, EventType::Press);
    CHECK(e.ui.view() == View::Entry);
    e.ev(3700, EventType::Side);
    CHECK(e.ui.listId() == ListId::Wifi);
    CHECK(e.ui.joinSsid()[0] == 0);
    // The splash skips; a message ignores it; the confirm screen cancels the install.
    Sim sp;
    sp.ev(500, EventType::Side);
    CHECK(sp.ui.view() == View::Home);
    Sim c;
    c.tick(3000);
    c.ui.confirmInstall(Sim::kBoot + 3000);
    ui::Output o;
    c.ui.event({EventType::Side, 0}, Sim::kBoot + 3100, c.s, c.st, o);
    CHECK(c.ui.view() == View::Home);
    CHECK(o.n == 1 && o.cmds[0].cmd == radio::Cmd::CancelInstall);
    Sim msg;
    msg.tick(3000);
    msg.ui.message("Battery empty");
    msg.ev(3100, EventType::Side);
    CHECK(msg.ui.view() == View::Message);
}

TEST(lamp_off_at_power_up) {
    Sim h;
    CHECK(h.st.lamp == radio::Lamp::Off);
    CHECK(ui::Status{}.lamp == radio::Lamp::Off);
}

TEST(side_hold_lamp) {
    // From off on home: the lamp on, the lamp screen; the hold again turns it off and closes the screen.
    Sim h;
    h.ev(3000, EventType::SideHold);
    CHECK(h.st.lamp == radio::Lamp::On);
    CHECK(h.ui.view() == View::Lamp);
    h.ev(3100, EventType::SideHold);
    CHECK(h.st.lamp == radio::Lamp::Off);
    CHECK(h.ui.view() == View::Home);
    // From SOS on the lamp screen: off and closed.
    h.ev(3200, EventType::SideHold);
    h.ev(3300, EventType::Press);
    CHECK(h.st.lamp == radio::Lamp::Sos);
    h.ev(3400, EventType::SideHold);
    CHECK(h.st.lamp == radio::Lamp::Off);
    CHECK(h.ui.view() == View::Home);
    // In a list: the lamp screen opens over it and Back returns to it.
    Menu m;
    m.ev(3050, EventType::Turn, 1);
    m.ev(3100, EventType::Press);  // Band list
    m.ev(3200, EventType::SideHold);
    CHECK(m.ui.view() == View::Lamp);
    m.ev(3300, EventType::Side);
    CHECK(m.ui.view() == View::List && m.ui.listId() == ListId::Bands);
    // On, away from the lamp screen: off, and the list stays.
    m.ev(3400, EventType::SideHold);
    CHECK(m.st.lamp == radio::Lamp::Off);
    CHECK(m.ui.view() == View::List && m.ui.listId() == ListId::Bands);
    // On the lamp screen with the lamp on: off and back to the list.
    m.ev(3500, EventType::SideHold);
    m.ev(3600, EventType::SideHold);
    CHECK(m.st.lamp == radio::Lamp::Off);
    CHECK(m.ui.view() == View::List && m.ui.listId() == ListId::Bands);
}

TEST(side_hold_ignored) {
    Sim sp;
    sp.ev(500, EventType::SideHold);
    CHECK(sp.ui.view() == View::Splash);
    CHECK(sp.st.lamp == radio::Lamp::Off);
    Sim c;
    c.tick(3000);
    c.ui.confirmInstall(Sim::kBoot + 3000);
    c.ev(3100, EventType::SideHold);
    CHECK(c.ui.view() == View::Confirm);
    CHECK(c.st.lamp == radio::Lamp::Off);
    Sim msg;
    msg.tick(3000);
    msg.ui.message("Battery empty");
    msg.ev(3100, EventType::SideHold);
    CHECK(msg.ui.view() == View::Message);
    CHECK(msg.st.lamp == radio::Lamp::Off);
}

TEST(lamp_screen) {
    Sim h;
    h.ev(3000, EventType::SideHold);
    // Turning sets the level, 1 to 15.
    h.ev(3100, EventType::Turn, 3);
    CHECK_EQ(h.s.light, 15);
    h.ev(3200, EventType::Turn, -20);
    CHECK_EQ(h.s.light, 1);
    h.ev(3300, EventType::Turn, 5);
    CHECK_EQ(h.s.light, 6);
    // The knob press: on, SOS, off; off closes the screen.
    h.ev(3400, EventType::Press);
    CHECK(h.st.lamp == radio::Lamp::Sos);
    CHECK(h.ui.view() == View::Lamp);
    h.ev(3500, EventType::Turn, 1);  // the level also changes in SOS
    CHECK_EQ(h.s.light, 7);
    h.ev(3600, EventType::Press);
    CHECK(h.st.lamp == radio::Lamp::Off);
    CHECK(h.ui.view() == View::Home);
    // A long press goes home, leaving the lamp on.
    h.ev(3700, EventType::SideHold);
    h.ev(3800, EventType::LongPress);
    CHECK(h.ui.view() == View::Home);
    CHECK(h.st.lamp == radio::Lamp::On);
    CHECK_EQ(h.s.presetCount, 5);
    // It closes after 8 s without input, leaving the lamp on.
    Sim t;
    t.ev(3000, EventType::SideHold);
    t.tick(11000);
    CHECK(t.ui.view() == View::Lamp);
    t.tick(11001);
    CHECK(t.ui.view() == View::Home);
    CHECK(t.st.lamp == radio::Lamp::On);
}

TEST(settings_light) {
    // Settings > Light with the lamp off: the lamp on and the lamp screen; Back returns to Settings.
    Menu m;
    m.ev(3100, EventType::Turn, -3);
    m.ev(3200, EventType::Press);
    m.ev(3300, EventType::Turn, 3);
    m.ev(3400, EventType::Press);
    CHECK(m.ui.view() == View::Lamp);
    CHECK(m.st.lamp == radio::Lamp::On);
    m.ev(3500, EventType::Press);  // SOS
    m.ev(3600, EventType::Side);
    CHECK(m.ui.listId() == ListId::Settings);
    CHECK_EQ(m.ui.cursor(), 3);
    // With the lamp in SOS it stays in SOS.
    m.ev(3700, EventType::Press);
    CHECK(m.ui.view() == View::Lamp);
    CHECK(m.st.lamp == radio::Lamp::Sos);
}

TEST(confirm_install) {
    Sim a;
    a.tick(3000);
    a.ui.confirmInstall(Sim::kBoot + 3000);
    a.tick(33000);
    CHECK(a.ui.view() == View::Confirm);
    ui::Output o;
    a.ui.tick(Sim::kBoot + 33001, a.s, a.st, o);
    CHECK(a.ui.view() == View::Home);
    CHECK(o.n == 1 && o.cmds[0].cmd == radio::Cmd::CancelInstall);
    Sim b;
    b.tick(3000);
    b.ui.confirmInstall(Sim::kBoot + 3000);
    ui::Output p;
    b.ui.event({EventType::Press, 0}, Sim::kBoot + 3100, b.s, b.st, p);
    CHECK(p.n == 1 && p.cmds[0].cmd == radio::Cmd::Install);
    CHECK(b.ui.view() == View::Message);
}

TEST(language_setting) {
    Menu m;
    m.ev(3100, EventType::Turn, -3);
    m.ev(3200, EventType::Press);  // Settings
    m.ev(3300, EventType::Turn, 5);
    m.ev(3400, EventType::Press);  // Language list, current language preselected
    CHECK(m.ui.listId() == ListId::Language);
    CHECK_EQ(m.ui.cursor(), 0);
    m.ev(3500, EventType::Turn, 4);
    m.ev(3600, EventType::Press);
    CHECK_EQ(m.s.lang, 4);
    CHECK(m.ui.listId() == ListId::Settings);
    m.ev(3700, EventType::Press);
    CHECK_EQ(m.ui.cursor(), 4);  // reopens on the chosen language
}

TEST(time_list) {
    Menu m;
    m.ev(3100, EventType::Turn, -3);
    m.ev(3200, EventType::Press);  // Settings
    m.ev(3300, EventType::Turn, 4);
    m.ev(3400, EventType::Press);  // Time
    CHECK(m.ui.listId() == ListId::Time);
    CHECK_EQ(m.ui.cursor(), 0);
    // Last sync is not selectable: the ring goes Time sync, Server, Back.
    const int expect[] = {1, -1, 0, 1};
    uint32_t t = 3400;
    for (int e : expect) {
        m.ev(t += 100, EventType::Turn, 1);
        CHECK_EQ(m.ui.cursor(), e);
    }
    m.ev(t += 100, EventType::HoldTurn, 1);  // a jump stops on the last selectable entry
    CHECK_EQ(m.ui.cursor(), 1);
    m.ev(t += 100, EventType::Turn, -1);
    m.ev(t += 100, EventType::Press);  // Time sync off
    CHECK(!m.s.timeSync);
    CHECK(m.ui.listId() == ListId::Time);
    m.ev(t += 100, EventType::Press);
    CHECK(m.s.timeSync);
}

TEST(server_entry) {
    Menu m;
    m.ev(3100, EventType::Turn, -3);
    m.ev(3200, EventType::Press);
    m.ev(3300, EventType::Turn, 4);
    m.ev(3400, EventType::Press);
    m.ev(3500, EventType::Turn, 1);
    m.ev(3600, EventType::Press);  // Server: text entry with the server filled in, on OK
    CHECK(m.ui.view() == View::Entry);
    CHECK_STR(m.ui.entryText(), "pool.ntp.org");
    CHECK_EQ(m.ui.entryIndex(), 85);
    m.ev(3700, EventType::HoldTurn, 1);  // abc
    uint32_t t = 3800;
    for (int i = 0; i < 70; i++) m.ev(t += 10, EventType::Press);
    CHECK_EQ(static_cast<int>(std::strlen(m.ui.entryText())), radio::kNtpServerMax);
    m.ev(t += 100, EventType::Turn, -1);  // OK
    m.ev(t += 100, EventType::Press);
    CHECK(m.ui.listId() == ListId::Time);  // back in the Time list
    CHECK_EQ(m.ui.cursor(), 1);
    CHECK_STR(m.s.ntpServer, ("pool.ntp.org" + std::string(51, 'a')).c_str());
    // A long press cancels: nothing stored.
    m.ev(t += 100, EventType::Press);
    m.ev(t += 100, EventType::Turn, -1);  // DEL
    m.ev(t += 100, EventType::Press);
    m.ev(t += 100, EventType::LongPress);
    CHECK(m.ui.view() == View::Home);
    CHECK_EQ(static_cast<int>(std::strlen(m.s.ntpServer)), radio::kNtpServerMax);
}

TEST(clock_set_until_power_off) {
    Sim h;
    h.tick(3000);
    CHECK_EQ(h.st.utcMs, -1);
    h.ntp(3000, 1794268800000LL);
    h.st.joined = false;  // network lost
    h.s.timeSync = false;  // time sync turned off
    h.tick(63000);
    CHECK_EQ(h.st.utcMs, 1794268800000LL + 60000);
    CHECK_EQ(h.st.syncAgeMs, 60000);
}
