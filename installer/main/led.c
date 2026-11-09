#include "led.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include "activity.h"
#include "flash_job.h"
#include "self_update.h"
#include "stage.h"

#define LED_GPIO GPIO_NUM_8  // active low
#define STEP_MS 100
#define DONE_MS 10000

static volatile int64_t s_done_us = -1;

void led_completed(void) { s_done_us = esp_timer_get_time(); }

static void led_task(void *arg)
{
    (void)arg;
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    for (unsigned step = 0;; step++) {
        int64_t done = s_done_us;
        bool on;
        if (activity_session_open() || flash_job_running() || self_update_running()) {
            on = step % 2 == 0;                            // 5 Hz
        } else if (done >= 0 && esp_timer_get_time() - done < DONE_MS * 1000LL) {
            on = true;
        } else if (!stage_complete()) {
            on = step % 20 < 6 && step % 2 == 0;           // 3 short blinks every 2 s
        } else {
            on = step % 10 < 5;                            // 1 Hz
        }
        gpio_set_level(LED_GPIO, on ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}

void led_start(void)
{
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
}
