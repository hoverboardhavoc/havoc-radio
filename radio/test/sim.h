// The firmware UI driven like the prototype: the prototype's demo state, a radio that applies commands to
// the model, and WiFi state that follows Join.
#pragma once
#include <cstring>
#include <string>

#include "clock_core.h"
#include "guide_core.h"
#include "i18n_strings.h"
#include "radio_model.h"
#include "ui.h"
#include "util.h"

struct Sim {
    static constexpr uint32_t kBoot = 1000;  // power-on time, as in tools/gen_goldens.py
    radio::State s;
    ui::Status st;
    ui::Ui ui;
    ui::Network nets[3] = {{"Lab", 3}, {"Telenor-5G", 2}, {"Guest", 1}};
    char ssid[33] = "Lab";
    std::string ps, rt;
    clk::Core clock;  // monotonic time: the scenario time plus kBoot, as the prototype's clock
    gfx::Frame frame;
    // The prototype's sample guide (radio/test/guide/sample.bin) and the lookup for the tuned frequency.
    std::string table = readFile(t::root() + "/radio/test/guide/sample.bin");
    guide::Info info;
    guide::Hits hits;
    bool guideOn = true;
    // Download mode at a step, drawn in place of the UI, and when the install prompt appeared.
    bool downloading = false;
    ui::Download dl;
    uint32_t promptAt = 0;
    std::string dlVersion;
    int64_t commands[64] = {};  // how often each radio::Cmd was emitted

    Sim() {
        radio::defaults(s);
        radio::presetAdd(s, "RWM", 4996000, radio::Mode::AM);
        radio::presetAdd(s, "RWM", 9996000, radio::Mode::AM);
        radio::presetAdd(s, "Ch 292", 6070000, radio::Mode::AM);
        radio::presetAdd(s, "40m", 7100000, radio::Mode::LSB);
        radio::presetAdd(s, "FM", 100700000, radio::Mode::FM);
        st.bars = 3;
        st.joined = true;
        st.ssid = ssid;
        st.ip = "192.168.0.57";
        st.nets = nets;
        st.netCount = 3;
        st.battery = 87;
        st.version = "2026.46.0";
        guide::check(guide::MemSource(table.data(), table.size()), table.size(), info);
        st.guide = &info;
        st.hits = &hits;
        ui.begin(kBoot);
    }

    void guideOff() {
        guideOn = false;
        st.guide = nullptr;
    }

    void wifiNone() {
        wifiOff();
        st.netSaved = false;
    }

    void wifiOff() {
        st.joined = false;
        st.ip = "192.168.4.1";
    }

    // RDS as the decoder hands it to the UI.
    void setPs(const std::string& t) {
        ps = t;
        st.ps = ps.c_str();
    }
    void setRt(const std::string& t) {
        rt = t;
        st.rt = rt.c_str();
    }

    void setLang(const char* code) {
        for (int i = 0; i < i18n::kLangCount; i++)
            if (!std::strcmp(i18n::kLangCode[i], code)) s.lang = static_cast<uint8_t>(i);
    }

    void apply(const ui::Output& o) {
        for (int i = 0; i < o.n; i++) {
            const radio::Command& c = o.cmds[i];
            commands[static_cast<int>(c.cmd)]++;
            switch (c.cmd) {
                case radio::Cmd::Seek:
                    st.seeking = true;
                    st.seekDir = static_cast<int8_t>(c.v);
                    break;
                case radio::Cmd::SeekStop: st.seeking = false; break;
                case radio::Cmd::Lamp: st.lamp = static_cast<radio::Lamp>(c.v); break;
                case radio::Cmd::NtpServer: radio::setNtpServer(s, ui.serverText()); break;
                case radio::Cmd::GuideUpdate:
                case radio::Cmd::FirmwareUpdate: break;
                case radio::Cmd::Join:
                    std::strncpy(ssid, ui.joinSsid(), sizeof ssid - 1);
                    st.netSaved = true;
                    st.joined = true;
                    st.ip = "192.168.0.57";
                    break;
                default: radio::apply(s, c, st.ssb, radio::isFm(s.mode) ? st.ps : ""); break;
            }
        }
    }

    // An NTP sync at time t: UTC in ms since 1970.
    void ntp(uint32_t t, int64_t utcMs) { clock.synced(utcMs, kBoot + t); }

    // Download mode ends in a restart: the splash, then the result on home (the prototype's restart()).
    void restart(uint32_t t, const char* result) {
        downloading = false;
        ui.begin(kBoot + t);
        if (result) ui.result(result, kBoot + t);
    }
    // A cancel shows Update cancelled for the firmware and nothing for the guide.
    void cancelDownload(uint32_t t) {
        restart(t, dl.firmware ? i18n::tr(s.lang, i18n::Str::toast_cancelled) : nullptr);
    }

    void tick(uint32_t t) {
        if (downloading && dl.step == ui::Download::Step::Prompt && t - promptAt > ui::kPromptMs) cancelDownload(t);
        st.utcMs = clock.utcMs(kBoot + t);
        st.syncAgeMs = clock.syncAgeMs(kBoot + t);
        if (guideOn && !radio::isFm(s.mode))
            guide::lookup(guide::MemSource(table.data(), table.size()), info, s.hz, st.utcMs, hits);
        else
            hits.n = 0;
        ui::Output o;
        ui.tick(kBoot + t, s, st, o);
        apply(o);
    }

    void ev(uint32_t t, ui::EventType type, int n = 0) {
        tick(t);
        // Download mode: the prompt's answer, or Back.
        if (downloading) {
            const ui::PromptAnswer a = dl.step == ui::Download::Step::Prompt ? ui::promptAnswer(type)
                                       : type == ui::EventType::Side         ? ui::PromptAnswer::Cancel
                                                                             : ui::PromptAnswer::None;
            if (a == ui::PromptAnswer::Install) dl.step = ui::Download::Step::Get;
            if (a == ui::PromptAnswer::Cancel) cancelDownload(t);
            return;
        }
        ui::Output o;
        ui.event({type, n}, kBoot + t, s, st, o);
        apply(o);
    }

    const gfx::Frame& render(uint32_t t) {
        tick(t);
        if (downloading)
            ui::drawDownload(frame, s.lang, dl);
        else
            ui.render(frame, kBoot + t, s, st);
        return frame;
    }
};
