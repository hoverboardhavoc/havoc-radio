// Havoc Radio Installer (installer/SPEC.md): ESP32-C3 SuperMini firmware that
// installs and services Havoc Radio. An open hotspot whose DNS answers every
// name with the C3, the stock firmware's update server on port 10000 sending
// the carried image, a captive page, the flash job over the radio's serial
// header, the serial console, the raw socket bridge and its own update over
// WiFi.

#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include "activity.h"
#include "flash_job.h"
#include "led.h"
#include "self_update.h"
#include "server.h"
#include "stage.h"
#include "uart_bridge.h"
#include "web.h"
#include "wifi.h"

#define DNS_PORT 53
#define UPDATE_PORT 10000

static const char *TAG = "installer";

// ---- Platform of the DNS and update servers (server.h) -------------------------

uint32_t platform_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void platform_log(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
}

void platform_dns_query(const uint8_t ip[4], const char *name) { activity_dns(ip, name); }

const char *platform_image_hold(uint32_t *len) { return stage_hold(len); }

bool platform_image_read(uint32_t offset, uint8_t *buf, size_t len) { return stage_flat_read(offset, buf, len); }

void platform_image_release(void) { stage_release(); }

void platform_session(const uint8_t ip[4], const update_session *us, bool done) { activity_session(ip, us, done); }

static void server_task(void *arg)
{
    const server_config *cfg = arg;
    server_run(cfg);
    ESP_LOGE(TAG, "DNS and update servers stopped");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // The radio's IO0 and EN come up released before anything else can drive them.
    uart_bridge_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    activity_init();
    ESP_ERROR_CHECK(stage_init());
    led_start();
    wifi_start();

    httpd_handle_t server = web_start();
    stage_http_register(server);
    flash_job_http_register(server);
    uart_bridge_start(server);
    self_update_http_register(server);

    // DNS and the update server answer on the hotspot only.
    static server_config cfg = {
        .dns_port = DNS_PORT,
        .update_port = UPDATE_PORT,
        .answer_ip = {192, 168, 4, 1},
    };
    cfg.bind_addr = inet_addr(WIFI_HOTSPOT_IP_TEXT);
    xTaskCreate(server_task, "servers", 6144, &cfg, 5, NULL);

    self_update_start();
}
