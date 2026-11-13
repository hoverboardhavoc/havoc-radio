#include "input.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace input {

namespace {

volatile int s_detents = 0;
QuadDecoder s_decoder;
Buttons s_buttons;

// The script being taken, shared with the web server's task under a critical section. The semaphore is created by
// the first inject (download mode runs no web server and never needs it).
Injector s_injector;
SemaphoreHandle_t s_injectDone;
bool s_scriptTaken = false;  // UI loop only: the last step is taken, the frame after it not yet drawn

void IRAM_ATTR onEdge(void*) { s_detents = s_detents + s_decoder.feed(board::encoderAB()); }

}  // namespace

void start() {
    gpio_set_intr_type(static_cast<gpio_num_t>(board::kEncA), GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(static_cast<gpio_num_t>(board::kEncB), GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(static_cast<gpio_num_t>(board::kEncA), onEdge, nullptr);
    gpio_isr_handler_add(static_cast<gpio_num_t>(board::kEncB), onEdge, nullptr);
}

void poll(uint32_t now, EventQueue& out) {
    portENTER_CRITICAL();
    const int d = s_detents;
    s_detents = 0;
    portEXIT_CRITICAL();
    s_buttons.step(now, board::knobDown(), board::sideDown(), d, out);
}

bool inject(const Step* steps, int n) {
    if (!s_injectDone) s_injectDone = xSemaphoreCreateBinary();
    if (!s_injectDone) return false;
    xSemaphoreTake(s_injectDone, 0);
    portENTER_CRITICAL();
    const bool ok = s_injector.load(steps, n);
    portEXIT_CRITICAL();
    return ok;
}

bool waitInjected(uint32_t ms, int* taken) {
    const bool done = xSemaphoreTake(s_injectDone, pdMS_TO_TICKS(ms)) == pdTRUE;
    *taken = s_injector.taken();
    return done;
}

void takeInjected(uint32_t now, ui::View view, EventQueue& out) {
    portENTER_CRITICAL();
    const bool finished = s_injector.take(now, view, out);
    portEXIT_CRITICAL();
    if (finished) s_scriptTaken = true;
}

void drawn() {
    if (!s_scriptTaken) return;
    s_scriptTaken = false;
    // A script loaded since the last one finished is answered when it finishes in turn.
    portENTER_CRITICAL();
    const bool idle = !s_injector.busy();
    portEXIT_CRITICAL();
    if (idle) xSemaphoreGive(s_injectDone);
}

}  // namespace input
