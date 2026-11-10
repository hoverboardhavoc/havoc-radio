// The firmware UI driven like the prototype: the prototype's demo state, a radio that applies commands to
// the model, and WiFi state that follows Join.
#pragma once
#include <cstring>
#include <string>

#include "i18n_strings.h"
#include "radio_model.h"
#include "ui.h"

struct Sim {
    static constexpr uint32_t kBoot = 1000;  // power-on time, as in tools/gen_goldens.py
    radio::State s;
    ui::Status st;
    ui::Ui ui;
    ui::Network nets[3] = {{"Lab", 3}, {"Telenor-5G", 2}, {"Guest", 1}};
    char ssid[33] = "Lab";
    std::string ps, rt;
    gfx::Frame frame;

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
        ui.begin(kBoot);
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
            switch (c.cmd) {
                case radio::Cmd::Seek:
                    st.seeking = true;
                    st.seekDir = static_cast<int8_t>(c.v);
                    break;
                case radio::Cmd::SeekStop: st.seeking = false; break;
                case radio::Cmd::Join:
                    std::strncpy(ssid, ui.joinSsid(), sizeof ssid - 1);
                    st.joined = true;
                    st.ip = "192.168.0.57";
                    break;
                default: radio::apply(s, c, st.ssb, radio::isFm(s.mode) ? st.ps : ""); break;
            }
        }
    }

    void tick(uint32_t t) {
        ui::Output o;
        ui.tick(kBoot + t, s, st, o);
        apply(o);
    }

    void ev(uint32_t t, ui::EventType type, int n = 0) {
        tick(t);
        ui::Output o;
        ui.event({type, n}, kBoot + t, s, st, o);
        apply(o);
    }

    const gfx::Frame& render(uint32_t t) {
        tick(t);
        ui.render(frame, kBoot + t, s, st);
        return frame;
    }
};
