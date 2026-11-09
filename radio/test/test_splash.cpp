// The splash: every frame of the animation against the per-pixel computation it replaced, its drawing time on
// the host, and the splash end reported once.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sim.h"
#include "test.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// The splash as drawn before the wheel geometry was precomputed: every pixel's distance and angle each frame.
void referenceSplash(gfx::Frame& f, uint32_t tms) {
    static const double kTongues[5][2] = {{-0.6, 6}, {-0.15, 10}, {0.3, 12}, {0.75, 10}, {1.2, 7}};
    const double t = tms;
    const double k = t / 900 < 1 ? t / 900 : 1, ease = 1 - std::pow(1 - k, 3);
    const int cx = static_cast<int>(radio::jsRound(-16 + ease * 33)), cy = 16;
    const double phi = (cx + 16) / 13.0 + (t - 900 > 0 ? t - 900 : 0) * 0.004;
    const double heat = k < 1 ? 1.15 : 0.9;
    double flick[5];
    for (int i = 0; i < 5; i++) flick[i] = 0.72 + 0.28 * std::sin(t * 0.013 * (1 + 0.37 * i) + i * 1.7);
    const int x0 = cx - 32 > 0 ? cx - 32 : 0, x1 = cx + 14 < gfx::W - 1 ? cx + 14 : gfx::W - 1;
    for (int y = 0; y < gfx::H; y++)
        for (int x = x0; x <= x1; x++) {
            const double dx = x - cx, dy = y - cy, r = std::hypot(dx, dy), th = std::atan2(dy, dx);
            bool on = false;
            if (r <= 10.6) {
                const double fr = std::fmod(std::fmod((th - phi) / (2 * kPi) * 5, 1.0) + 1, 1.0);
                const bool window = r >= 3.4 && r <= 8.4 && fr > 0.18 && fr < 0.82;
                on = !window && r >= 1.3;
            } else if (r >= 12 && r <= 13.3) {
                const double g = std::fmod(std::fmod((th - phi) / (2 * kPi) * 16, 1.0) + 1, 1.0);
                on = g >= 0.2;
            } else if (r >= 14.5) {
                double d = th - kPi;
                d = std::atan2(std::sin(d), std::cos(d));
                d -= 0.35 * (r - 14.5) / 10;
                double len = 0;
                for (int i = 0; i < 5; i++) {
                    double w = 1 - std::fabs(d - kTongues[i][0]) / 0.3;
                    if (w < 0) w = 0;
                    const double l = kTongues[i][1] * heat * flick[i] * w;
                    if (l > len) len = l;
                }
                on = r - 14.5 < len;
            }
            if (on) f.px(x, y, 1);
        }
    if (t > 800) {
        const int n = std::min(std::max(static_cast<int>(std::floor((t - 800) / 45)), 0), 10);
        char word[11];
        std::memcpy(word, "HOVERBOARD", n);
        word[n] = 0;
        gfx::text(f, 40, 1, word);
    }
    if (t > 1250) {
        gfx::text2x(f, 40, 10, "HAVOC");
        const int reveal = 40 + std::min(std::max(static_cast<int>(std::floor((t - 1250) / 6)), 0), 52);
        f.fill(reveal, 10, 128 - reveal, 13, 0);
    }
    if (t > 1700) gfx::textR(f, 128, 24, "radio");
}

}  // namespace

TEST(splash_matches_per_pixel_maths) {
    Sim sim;
    int diff = 0;
    for (uint32_t t = 0; t <= ui::kSplashMs; t++) {
        const gfx::Frame& got = sim.render(t);
        gfx::Frame want;
        want.clear();
        referenceSplash(want, t);
        if (std::memcmp(got.buf, want.buf, sizeof want.buf) && diff++ < 5)
            t::fail(__FILE__, __LINE__, "splash frame differs at t=" + std::to_string(t));
    }
    CHECK_EQ(diff, 0);
}

TEST(splash_draw_time) {
    // Every millisecond of the animation, the geometry already computed (as on the radio).
    Sim sim;
    sim.render(0);
    gfx::Frame f;
    const auto t0 = std::chrono::steady_clock::now();
    int frames = 0;
    for (uint32_t t = 0; t <= ui::kSplashMs; t++, frames++) sim.ui.render(f, Sim::kBoot + t, sim.s, sim.st);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    std::printf("splash: %.1f us a frame on the host over %d frames\n", us / frames, frames);
    CHECK(sim.ui.view() == ui::View::Splash);
}

TEST(splash_end_reported_once) {
    const auto ends = [](const ui::Output& o) {
        int n = 0;
        for (int i = 0; i < o.n; i++) n += o.cmds[i].cmd == radio::Cmd::SplashEnd;
        return n;
    };
    // By time.
    Sim a;
    int n = 0;
    for (uint32_t t = 0; t <= 4000; t += 10) {
        ui::Output o;
        a.ui.tick(Sim::kBoot + t, a.s, a.st, o);
        n += ends(o);
        if (t == ui::kSplashMs) CHECK_EQ(n, 0);
    }
    CHECK_EQ(n, 1);
    // Skipped by an input, then ticks and more input.
    Sim b;
    ui::Output o;
    b.ui.event({ui::EventType::Turn, 1}, Sim::kBoot + 500, b.s, b.st, o);
    n = ends(o);
    CHECK(b.ui.view() == ui::View::Home);
    for (uint32_t t = 510; t <= 4000; t += 10) {
        ui::Output p;
        b.ui.tick(Sim::kBoot + t, b.s, b.st, p);
        n += ends(p);
    }
    ui::Output q;
    b.ui.event({ui::EventType::Press, 0}, Sim::kBoot + 4100, b.s, b.st, q);
    CHECK_EQ(n + ends(q), 1);
}
