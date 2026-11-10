#include "utc_clock.h"

#include <sys/time.h>

#include "clock_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"

namespace clk {

namespace {

Core s_core;  // written by the sync callback in the TCP/IP task, read by the UI loop and the web server

int64_t monoMs() { return esp_timer_get_time() / 1000; }

// The SDK's sntp_sync_time() has set the system time with settimeofday() before it calls this.
void onSync(struct timeval*) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t utc = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    const int64_t mono = monoMs();
    taskENTER_CRITICAL();
    s_core.synced(utc, mono);
    taskEXIT_CRITICAL();
    ESP_LOGI("clock", "synced, %lld s UTC", static_cast<long long>(tv.tv_sec));
}

}  // namespace

void init() {
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_time_sync_notification_cb(onSync);
}

Now now() {
    const int64_t mono = monoMs();
    taskENTER_CRITICAL();
    const Core c = s_core;
    taskEXIT_CRITICAL();
    Now n;
    n.utcMs = c.utcMs(mono);
    n.lastSyncMs = c.lastSyncMs();
    n.syncAgeMs = c.syncAgeMs(mono);
    return n;
}

}  // namespace clk
