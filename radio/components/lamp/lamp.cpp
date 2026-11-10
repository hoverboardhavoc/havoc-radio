#include "lamp.h"

#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lamp_core.h"

namespace lamp {

namespace {

// Written by the UI loop, read by the timer callback.
radio::Lamp s_mode = radio::Lamp::Off;
int s_level = radio::kLightMax;
int64_t s_sosStart = 0;  // us
TimerHandle_t s_timer = nullptr;

// The only place the lamp output is set. It runs in the timer task, which has a higher priority than the UI loop,
// so a change from the loop never interrupts it; the loop's change queues a run of its own.
void onTimer(TimerHandle_t) {
    portENTER_CRITICAL();
    const radio::Lamp m = s_mode;
    const int level = s_level;
    const int64_t start = s_sosStart;
    portEXIT_CRITICAL();
    uint32_t t = 0, next = 0;
    if (m == radio::Lamp::Sos) {
        t = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000 % kSosCycleMs);
        sosLit(t, &next);
    }
    board::lamp(output(m, level, t));
    if (m == radio::Lamp::Sos) {
        const TickType_t ticks = pdMS_TO_TICKS(next - t + portTICK_PERIOD_MS - 1);
        xTimerChangePeriod(s_timer, ticks ? ticks : 1, 0);
    }
}

// Runs the callback on the next tick.
void update() { xTimerChangePeriod(s_timer, 1, portMAX_DELAY); }

}  // namespace

void start(int level) {
    s_level = level;
    s_timer = xTimerCreate("lamp", 1, pdFALSE, nullptr, onTimer);
}

void setMode(radio::Lamp m) {
    if (m == s_mode) return;
    portENTER_CRITICAL();
    if (m == radio::Lamp::Sos) s_sosStart = esp_timer_get_time();
    s_mode = m;
    portEXIT_CRITICAL();
    update();
}

void setLevel(int level) {
    if (level == s_level) return;
    portENTER_CRITICAL();
    s_level = level;
    portEXIT_CRITICAL();
    if (s_mode != radio::Lamp::Off) update();
}

radio::Lamp mode() { return s_mode; }

}  // namespace lamp
