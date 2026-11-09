#include "input.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

namespace input {

namespace {

volatile int s_detents = 0;
QuadDecoder s_decoder;
Buttons s_buttons;

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

}  // namespace input
