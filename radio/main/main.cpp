// Startup and the UI loop: input events go through the UI, its commands to the radio, network and web
// modules, and the frame goes to the display when it changes.
#include <cstring>

#include "board.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i18n_strings.h"
#include "i2c_bus.h"
#include "input.h"
#include "net.h"
#include "power.h"
#include "radio.h"
#include "ssd1306.h"
#include "store.h"
#include "ui.h"
#include "utc_clock.h"
#include "web.h"

namespace {

constexpr const char* kVersion = "2026.46.0";
constexpr uint32_t kFrameMs = 33;
constexpr uint32_t kEmptyMessageMs = 3000;

ui::Ui s_ui;
gfx::Frame s_frame, s_shown;
rds::Decoder s_rds;
char s_ps[rds::kPsBuf], s_rt[rds::kRtBuf];
bool s_netWanted = false;  // the splash has ended: start the network after the next frame
bool s_netUp = false;

uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint8_t contrastRegister(int level) { return static_cast<uint8_t>(level * 17); }

void handle(const ui::Output& out) {
    for (int i = 0; i < out.n; i++) {
        const radio::Command& c = out.cmds[i];
        switch (c.cmd) {
            case radio::Cmd::Join: net::join(s_ui.joinSsid(), s_ui.joinPassword()); break;
            case radio::Cmd::Scan: net::scan(); break;
            case radio::Cmd::Install: web::decide(true); break;
            case radio::Cmd::CancelInstall: web::decide(false); break;
            case radio::Cmd::SplashEnd: s_netWanted = true; break;
            case radio::Cmd::NtpServer:
                if (radio::edit([](radio::State& st, void* p) { return radio::setNtpServer(st, static_cast<const char*>(p)); },
                                const_cast<char*>(s_ui.serverText())))
                    store::touch();
                break;
            default:
                if (radio::submit(c)) store::touch();
                break;
        }
    }
}

void show(bool force) {
    if (force || std::memcmp(s_frame.buf, s_shown.buf, sizeof s_frame.buf)) {
        ssd1306::push(s_frame.buf);
        s_shown = s_frame;
    }
}

// Battery empty: say so, then mute, power the receiver down, turn the LED off and sleep with no wake source.
// The slide switch restarts the radio.
[[noreturn]] void powerOff(const radio::State& s, const ui::Status& st) {
    s_ui.message(i18n::tr(s.lang, i18n::Str::battery_empty));
    s_ui.render(s_frame, millis(), s, st);
    show(true);
    radio::shutdown();
    board::led(0);
    vTaskDelay(pdMS_TO_TICKS(kEmptyMessageMs));
    ssd1306::display(false);
    esp_wifi_stop();
    esp_deep_sleep(0);
    for (;;) vTaskDelay(portMAX_DELAY);
}

}  // namespace

extern "C" void app_main() {
    board::init();
    i2c::init();
    if (!ssd1306::init()) ESP_LOGE("main", "display not answering");
    store::init();

    radio::State s;
    radio::defaults(s);
    store::loadState(s);
    ssd1306::contrast(contrastRegister(s.contrast));
    board::led(s.light);
    int contrast = s.contrast, light = s.light;

    radio::start(s);
    input::start();
    power::start();
    clk::init();

    ui::prepareSplash();
    s_ui.begin(millis());
    uint32_t lastFrame = 0;
    for (;;) {
        const uint32_t now = millis();
        input::EventQueue q;
        input::poll(now, q);

        net::Info ni;
        if (s_netUp) net::info(ni);
        const radio::Live live = radio::live();
        ui::Status st;
        st.bars = live.bars;
        st.seeking = live.seeking;
        st.seekDir = live.seekDir;
        st.ssb = live.ssb;
        st.joined = ni.joined;
        st.ssid = ni.ssid;
        st.ip = ni.ip;
        st.nets = ni.nets;
        st.netCount = ni.netCount;
        st.battery = power::percent();
        st.version = kVersion;
        radio::rdsState(s_rds);
        s_rds.ps(s_ps, sizeof s_ps, true);
        s_rds.rt(s_rt, sizeof s_rt, true);
        st.rdsSync = s_rds.sync();
        st.ps = s_ps;
        st.rt = s_rt;
        st.pty = s_rds.pty();
        st.rtSeq = s_rds.rtSeq();
        const clk::Now clock = clk::now();
        st.utcMs = clock.utcMs;
        st.syncAgeMs = clock.syncAgeMs;

        radio::snapshot(s);
        for (int i = 0; i < q.n; i++) {
            ui::Output out;
            s_ui.event(q.ev[i], now, s, st, out);
            handle(out);
            radio::snapshot(s);
        }
        ui::Output out;
        s_ui.tick(now, s, st, out);
        handle(out);
        if (web::takeConfirmRequest()) s_ui.confirmInstall(now);
        if (power::empty()) powerOff(s, st);

        if (s.contrast != contrast) ssd1306::contrast(contrastRegister(contrast = s.contrast));
        if (s.light != light) board::led(light = s.light);

        if (s_netWanted || now - lastFrame >= kFrameMs) {
            lastFrame = now;
            s_ui.render(s_frame, now, s, st);
            show(false);
        }
        // WiFi starts once the splash is over and the first frame after it is on screen: joining holds the CPU
        // at a higher priority than this loop for about a second. The web server needs the network stack that
        // net::start brings up.
        if (s_netWanted) {
            s_netWanted = false;
            net::start();
            web::start(kVersion);
            s_netUp = true;
        }
        if (s_netUp) net::timeSync(s.timeSync, s.ntpServer);
        store::poll(now);
        vTaskDelay(1);
    }
}
