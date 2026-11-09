#include "power.h"

#include "board.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace power {

namespace {
volatile int s_raw = 0;
volatile bool s_empty = false;

void task(void*) {
    int low = 0;
    for (;;) {
        uint16_t v = 0;
        if (adc_read(&v) == ESP_OK) {
            s_raw = v;
            low = v < board::kBatteryEmptyRaw ? low + 1 : 0;
            if (low >= kEmptyReadings) s_empty = true;
        }
        vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
    }
}
}  // namespace

int percentFor(int r) {
    if (r <= board::kBatteryLowRaw) return 0;
    if (r >= board::kBatteryFullRaw) return 100;
    return (r - board::kBatteryLowRaw) * 100 / (board::kBatteryFullRaw - board::kBatteryLowRaw);
}

void start() {
    adc_config_t cfg = {};
    cfg.mode = ADC_READ_TOUT_MODE;
    cfg.clk_div = 8;
    adc_init(&cfg);
    xTaskCreate(task, "power", 1536, nullptr, 3, nullptr);
}

int raw() { return s_raw; }
int percent() { return percentFor(s_raw); }
bool empty() { return s_empty; }

}  // namespace power
