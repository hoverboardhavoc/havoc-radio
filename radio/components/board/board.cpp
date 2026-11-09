#include "board.h"

#include "driver/gpio.h"
#include "driver/pwm.h"
#include "esp_attr.h"
#include "esp8266/pin_mux_register.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

namespace board {

namespace {
bool s_pwm = false;
constexpr uint32_t kPwmPeriodUs = 1000;
}  // namespace

void init() {
    // Outputs first, at their boot levels, so the straps never see a different level.
    gpio_set_level(static_cast<gpio_num_t>(kAmpMute), 1);
    gpio_set_level(static_cast<gpio_num_t>(kSiReset), 1);
    gpio_set_level(static_cast<gpio_num_t>(kLed), 0);
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << kAmpMute) | (1ULL << kSiReset) | (1ULL << kLed);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level(static_cast<gpio_num_t>(kAmpMute), 1);
    gpio_set_level(static_cast<gpio_num_t>(kSiReset), 1);
    gpio_set_level(static_cast<gpio_num_t>(kLed), 0);

    // The side button takes over UART0 RX; the log keeps UART0 TX.
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_GPIO3);
    gpio_config_t in = {};
    in.pin_bit_mask = (1ULL << kEncA) | (1ULL << kEncB) | (1ULL << kEncPush) | (1ULL << kSide);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);
}

void ampMute(bool muted) { gpio_set_level(static_cast<gpio_num_t>(kAmpMute), muted ? 1 : 0); }

void siReset() {
    gpio_set_level(static_cast<gpio_num_t>(kSiReset), 0);
    ets_delay_us(200);  // reset pulse, at least 100 us
    gpio_set_level(static_cast<gpio_num_t>(kSiReset), 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void led(int level) {
    if (level < 0) level = 0;
    if (level > 15) level = 15;
    const uint32_t duty = static_cast<uint32_t>(level * level) * kPwmPeriodUs / 225;
    if (!s_pwm) {
        if (!duty) return;
        const uint32_t pins[1] = {static_cast<uint32_t>(kLed)};
        uint32_t duties[1] = {duty};
        pwm_init(kPwmPeriodUs, duties, 1, pins);
        pwm_start();
        s_pwm = true;
        return;
    }
    if (!duty) {
        pwm_stop(0);
        pwm_deinit();
        s_pwm = false;
        gpio_set_direction(static_cast<gpio_num_t>(kLed), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(kLed), 0);
        return;
    }
    pwm_set_duty(0, duty);
    pwm_start();
}

bool knobDown() { return gpio_get_level(static_cast<gpio_num_t>(kEncPush)) == 0; }
bool sideDown() { return gpio_get_level(static_cast<gpio_num_t>(kSide)) == 0; }

uint8_t IRAM_ATTR encoderAB() {
    const uint32_t in = GPIO_REG_READ(GPIO_IN_ADDRESS);
    const uint8_t a = (in >> kEncA) & 1, b = (in >> kEncB) & 1;
    return static_cast<uint8_t>((a << 1) | b);
}

}  // namespace board
