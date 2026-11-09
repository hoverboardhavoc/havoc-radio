#include "flash_job.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_loader.h"
#include "esp_loader_io.h"

#include "led.h"
#include "self_update.h"
#include "stage.h"
#include "uart_bridge.h"

static const char *TAG = "flash";

// The ESP8266 ROM loader talks at this rate; the stub is loaded at it too.
#define ROM_BAUD 115200
#define DEFAULT_BAUD 460800
// The stub's largest FLASH_DATA payload (esp-flasher-stub frame buffer).
#define FLASH_BLOCK 0x4000
// A region that fails verification is rewritten at most this many times.
#define REGION_REWRITES 3

// esp-serial-flasher refuses esp_loader_change_transmission_rate() on the
// ESP8266 because its ROM has no CHANGE_BAUDRATE, but the flasher stub has it
// (esptool raises the ESP8266's baud through the stub the same way). The
// library's command is exported without a public declaration.
esp_loader_error_t loader_change_baudrate_cmd(esp_loader_t *loader, uint32_t new_baudrate,
                                              uint32_t old_baudrate);

// ---- Job state (written by the job task, read by GET /flash) -----------------

typedef enum { JOB_IDLE, JOB_RUNNING, JOB_DONE, JOB_FAILED } job_state_t;

static const char *const STATE_NAMES[] = {"idle", "running", "done", "failed"};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_running;
static struct {
    uint32_t job;  // increments per started job, so a host can tell its job apart
    job_state_t state;
    int region;
    uint32_t bytes_done;
    uint32_t bytes_total;
    uint32_t baud;
    uint32_t rewrites;
    bool connected;  // the job reached the radio's ROM loader
    int64_t start_us;
    int64_t end_us;
    char error[96];
    uint32_t count;
    uint32_t target[SL_MAX_REGIONS];
    bool verified[SL_MAX_REGIONS];
} s_job;

static uint32_t s_req_baud;
static bool s_reset_after;
static uint8_t s_buf[FLASH_BLOCK];

bool flash_job_running(void) { return s_running; }

#define LOCKED(stmt)                 \
    do {                             \
        taskENTER_CRITICAL(&s_lock); \
        stmt;                        \
        taskEXIT_CRITICAL(&s_lock);  \
    } while (0)

static void job_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void job_fail(const char *fmt, ...)
{
    char msg[sizeof(s_job.error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "job failed: %s", msg);
    LOCKED(strcpy(s_job.error, msg); s_job.state = JOB_FAILED; s_job.end_us = esp_timer_get_time());
}

static const char *loader_err_str(esp_loader_error_t err)
{
    switch (err) {
    case ESP_LOADER_SUCCESS: return "ok";
    case ESP_LOADER_ERROR_TIMEOUT: return "timeout";
    case ESP_LOADER_ERROR_IMAGE_SIZE: return "region past the end of target flash";
    case ESP_LOADER_ERROR_INVALID_MD5: return "md5 mismatch";
    case ESP_LOADER_ERROR_INVALID_PARAM: return "invalid parameter";
    case ESP_LOADER_ERROR_INVALID_TARGET: return "invalid target";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP: return "unsupported chip";
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC: return "unsupported function";
    case ESP_LOADER_ERROR_INVALID_RESPONSE: return "invalid response";
    default: return "loader error";
    }
}

// ---- esp-serial-flasher port on the bridge UART ---------------------------------
// The library's own ESP32 port reconfigures the reset and boot pins as push-pull
// outputs; this port reuses the bridge's open-drain IO0/EN sequences instead and
// the UART driver main.c already installed.

static int64_t s_time_end;

static esp_loader_error_t port_write(esp_loader_port_t *port, const uint8_t *data, uint16_t size,
                                     uint32_t timeout)
{
    (void)port;
    uart_write_bytes(BRIDGE_UART, data, size);
    esp_err_t err = uart_wait_tx_done(BRIDGE_UART, pdMS_TO_TICKS(timeout));
    return err == ESP_OK ? ESP_LOADER_SUCCESS
           : err == ESP_ERR_TIMEOUT ? ESP_LOADER_ERROR_TIMEOUT
           : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t port_read(esp_loader_port_t *port, uint8_t *data, uint16_t size,
                                    uint32_t timeout)
{
    (void)port;
    int n = uart_read_bytes(BRIDGE_UART, data, size, pdMS_TO_TICKS(timeout));
    return n < 0 ? ESP_LOADER_ERROR_FAIL : n < size ? ESP_LOADER_ERROR_TIMEOUT : ESP_LOADER_SUCCESS;
}

static void port_enter_bootloader(esp_loader_port_t *port)
{
    (void)port;
    target_enter_bootloader();
}

static void port_reset_target(esp_loader_port_t *port)
{
    (void)port;
    target_reset_run();
}

static void port_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    s_time_end = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t port_remaining_time(esp_loader_port_t *port)
{
    (void)port;
    int64_t remaining = (s_time_end - esp_timer_get_time()) / 1000;
    return remaining > 0 ? (uint32_t)remaining : 0;
}

static void port_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    usleep(ms * 1000);
}

static esp_loader_error_t port_change_rate(esp_loader_port_t *port, uint32_t baud)
{
    (void)port;
    return uart_set_baudrate(BRIDGE_UART, baud) == ESP_OK ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static void port_log(esp_loader_port_t *port, esp_loader_log_level_t level, const char *fmt,
                     va_list args)
{
    (void)port;
    (void)level;
    printf("serial-flasher: ");
    vprintf(fmt, args);
    printf("\n");
}

static const esp_loader_port_ops_t s_port_ops = {
    .enter_bootloader = port_enter_bootloader,
    .reset_target = port_reset_target,
    .start_timer = port_start_timer,
    .remaining_time = port_remaining_time,
    .delay_ms = port_delay_ms,
    .log = port_log,
    .change_transmission_rate = port_change_rate,
    .write = port_write,
    .read = port_read,
};

static esp_loader_port_t s_port = {.ops = &s_port_ops};

// ---- The job ----------------------------------------------------------------

static void set_uart_baud(uint32_t baud)
{
    uart_set_baudrate(BRIDGE_UART, baud);
    uart_flush_input(BRIDGE_UART);
}

static esp_loader_error_t connect_stub(esp_loader_t *loader)
{
    set_uart_baud(ROM_BAUD);
    esp_loader_init_serial(loader, &s_port);
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err = esp_loader_connect_with_stub(loader, &args);
    if (err == ESP_LOADER_SUCCESS && esp_loader_get_target(loader) != ESP8266_CHIP) {
        err = ESP_LOADER_ERROR_INVALID_TARGET;
    }
    return err;
}

// Reset the target into its ROM loader, load the stub, and raise the baud to
// the requested rate. If the target does not answer at that rate, start over
// at ROM_BAUD.
static esp_loader_error_t connect_target(esp_loader_t *loader)
{
    esp_loader_error_t err = connect_stub(loader);
    if (err == ESP_LOADER_SUCCESS) {
        LOCKED(s_job.connected = true);
    }
    if (err != ESP_LOADER_SUCCESS || s_req_baud == ROM_BAUD) {
        LOCKED(s_job.baud = ROM_BAUD);
        return err;
    }
    s_port.ops->start_timer(&s_port, 1000);
    err = loader_change_baudrate_cmd(loader, s_req_baud, ROM_BAUD);
    if (err == ESP_LOADER_SUCCESS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        set_uart_baud(s_req_baud);
        uint32_t flash_size = 0;
        err = esp_loader_flash_detect_size(loader, &flash_size);
        if (err == ESP_LOADER_SUCCESS) {
            ESP_LOGI(TAG, "stub at %" PRIu32 " baud, target flash %" PRIu32 " KB", s_req_baud,
                     flash_size / 1024);
            LOCKED(s_job.baud = s_req_baud);
            return ESP_LOADER_SUCCESS;
        }
    }
    ESP_LOGW(TAG, "no answer at %" PRIu32 " baud (%s), falling back to %d", s_req_baud,
             loader_err_str(err), ROM_BAUD);
    LOCKED(s_job.baud = ROM_BAUD);
    return connect_stub(loader);
}

static void add_progress(uint32_t n)
{
    LOCKED(s_job.bytes_done += n);
}

// Write one region (erase-only: erase it), then compare the target's
// SPI_FLASH_MD5 over the region with the MD5 of what it should hold.
static esp_loader_error_t write_region(esp_loader_t *loader, const sl_region_t *r)
{
    char md5[33];
    if (stage_region_md5(r, md5) != ESP_OK) {
        return ESP_LOADER_ERROR_FAIL;
    }
    esp_loader_error_t err;
    if (sl_is_erase(r)) {
        err = esp_loader_flash_erase_region(loader, r->target, r->length);
        if (err == ESP_LOADER_SUCCESS) {
            add_progress(r->length);
        }
    } else {
        esp_loader_flash_cfg_t cfg = {
            .offset = r->target,
            .image_size = r->length,
            .block_size = FLASH_BLOCK,
            .skip_verify = true,  // verified below against the staged bytes
        };
        err = esp_loader_flash_start(loader, &cfg);
        for (uint32_t off = 0; off < r->length && err == ESP_LOADER_SUCCESS; off += FLASH_BLOCK) {
            uint32_t n = MIN(FLASH_BLOCK, r->length - off);
            if (stage_read(r->stage + off, s_buf, n) != ESP_OK) {
                return ESP_LOADER_ERROR_FAIL;
            }
            err = esp_loader_flash_write(loader, &cfg, s_buf, n);
            if (err == ESP_LOADER_SUCCESS) {
                add_progress(n);
            }
        }
        if (err == ESP_LOADER_SUCCESS) {
            err = esp_loader_flash_finish(loader, &cfg);
        }
    }
    if (err != ESP_LOADER_SUCCESS) {
        return err;
    }
    return esp_loader_flash_verify_known_md5(loader, r->target, r->length, (const uint8_t *)md5);
}

static void job_task(void *arg)
{
    (void)arg;
    const sl_manifest_t *m = stage_manifest();
    esp_loader_t loader;
    bool connected = false;
    esp_loader_error_t err = ESP_LOADER_SUCCESS;
    uint32_t i;

    for (i = 0; i < m->count; i++) {
        const sl_region_t *r = &m->regions[i];
        uint32_t done_before;
        LOCKED(s_job.region = i; done_before = s_job.bytes_done);
        bool verified = false;
        for (int attempt = 0; attempt <= REGION_REWRITES && !verified; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "region %" PRIu32 " at 0x%" PRIx32 ": %s, rewrite %d", i, r->target,
                         loader_err_str(err), attempt);
                LOCKED(s_job.rewrites++; s_job.bytes_done = done_before);
            }
            // Any error other than an MD5 mismatch leaves the link in an
            // unknown state, so start the connection over.
            if (!connected) {
                err = connect_target(&loader);
                connected = err == ESP_LOADER_SUCCESS;
                if (!connected) {
                    continue;
                }
            }
            err = write_region(&loader, r);
            verified = err == ESP_LOADER_SUCCESS;
            connected = verified || err == ESP_LOADER_ERROR_INVALID_MD5;
        }
        if (!verified) {
            break;
        }
        ESP_LOGI(TAG, "region %" PRIu32 " at 0x%" PRIx32 " verified", i, r->target);
        LOCKED(s_job.verified[i] = true);
    }

    if (i < m->count) {
        job_fail("region %" PRIu32 " at 0x%" PRIx32 ": %s", i, m->regions[i].target,
                 loader_err_str(err));
    } else {
        if (s_reset_after) {
            target_reset_run();
        }
        LOCKED(s_job.state = JOB_DONE; s_job.end_us = esp_timer_get_time());
        ESP_LOGI(TAG, "job done: %" PRIu32 " regions verified in %lld ms at %" PRIu32 " baud, %" PRIu32
                 " rewrites", m->count, (s_job.end_us - s_job.start_us) / 1000, s_job.baud, s_job.rewrites);
        led_completed();
    }
    bridge_release_from_job();
    s_running = false;
    vTaskDelete(NULL);
}

// ---- HTTP -----------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, status, root);
}

static esp_err_t h_flash_post(httpd_req_t *req)
{
    uint32_t baud = DEFAULT_BAUD;
    bool reset_after = true;
    if (req->content_len > 0) {
        char body[128];
        if (req->content_len >= sizeof(body)) {
            return send_error(req, "400 Bad Request", "body too large");
        }
        size_t got = 0;
        while (got < req->content_len) {
            int n = httpd_req_recv(req, body + got, req->content_len - got);
            if (n <= 0) {
                return ESP_FAIL;
            }
            got += n;
        }
        body[got] = '\0';
        cJSON *root = cJSON_Parse(body);
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "baud");
        const cJSON *ra = cJSON_GetObjectItemCaseSensitive(root, "reset_after");
        bool bad = root == NULL || (b != NULL && !cJSON_IsNumber(b)) ||
                   (ra != NULL && !cJSON_IsBool(ra));
        if (!bad && b != NULL) {
            baud = (uint32_t)b->valuedouble;
        }
        if (!bad && ra != NULL) {
            reset_after = cJSON_IsTrue(ra);
        }
        cJSON_Delete(root);
        if (bad || baud < 9600 || baud > 921600) {
            return send_error(req, "400 Bad Request",
                              "need {\"baud\": 9600..921600, \"reset_after\": bool}");
        }
    }

    if (s_running) {
        return send_error(req, "409 Conflict", "flash job running");
    }
    if (self_update_running()) {
        return send_error(req, "409 Conflict", "installer update running");
    }
    if (!stage_complete()) {
        return send_error(req, "409 Conflict", "stage check has not passed");
    }
    const char *busy = bridge_claim_for_job();
    if (busy != NULL) {
        return send_error(req, "409 Conflict", busy);
    }

    const sl_manifest_t *m = stage_manifest();
    s_req_baud = baud;
    s_reset_after = reset_after;
    uint32_t total = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        total += m->regions[i].length;
    }
    uint32_t job;
    LOCKED(
        s_job.job++; job = s_job.job; s_job.state = JOB_RUNNING; s_job.region = 0;
        s_job.bytes_done = 0; s_job.bytes_total = total; s_job.baud = 0; s_job.rewrites = 0;
        s_job.connected = false;
        s_job.start_us = esp_timer_get_time(); s_job.end_us = 0; s_job.error[0] = '\0';
        s_job.count = m->count;
        for (uint32_t i = 0; i < m->count; i++) {
            s_job.target[i] = m->regions[i].target;
            s_job.verified[i] = false;
        });
    s_running = true;
    if (xTaskCreate(job_task, "flash_job", 6144, NULL, 5, NULL) != pdPASS) {
        s_running = false;
        bridge_release_from_job();
        job_fail("could not start the job task");
        return send_error(req, "500 Internal Server Error", "could not start the job task");
    }
    ESP_LOGI(TAG, "job %" PRIu32 " started: \"%s\", %" PRIu32 " regions, %" PRIu32 " bytes, baud %" PRIu32, job,
             m->label, m->count, total, baud);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "job", job);
    return send_json(req, "202 Accepted", root);
}

cJSON *flash_job_json(void)
{
    static typeof(s_job) snap;  // httpd runs one handler at a time
    LOCKED(snap = s_job);
    int64_t end = snap.state == JOB_RUNNING ? esp_timer_get_time() : snap.end_us;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "job", snap.job);
    cJSON_AddStringToObject(root, "state", STATE_NAMES[snap.state]);
    cJSON_AddNumberToObject(root, "region", snap.region);
    cJSON_AddNumberToObject(root, "bytes_done", snap.bytes_done);
    cJSON_AddNumberToObject(root, "bytes_total", snap.bytes_total);
    cJSON_AddNumberToObject(root, "baud", snap.baud);
    cJSON_AddNumberToObject(root, "rewrites", snap.rewrites);
    cJSON_AddBoolToObject(root, "connected", snap.connected);
    cJSON_AddNumberToObject(root, "elapsed_ms", snap.job ? (double)((end - snap.start_us) / 1000) : 0);
    cJSON_AddStringToObject(root, "error", snap.error);
    cJSON *regions = cJSON_AddArrayToObject(root, "regions");
    for (uint32_t i = 0; i < snap.count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "target", snap.target[i]);
        cJSON_AddBoolToObject(o, "verified", snap.verified[i]);
        cJSON_AddItemToArray(regions, o);
    }
    return root;
}

static esp_err_t h_flash_get(httpd_req_t *req)
{
    return send_json(req, "200 OK", flash_job_json());
}

void flash_job_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/flash", .method = HTTP_POST, .handler = h_flash_post},
        {.uri = "/flash", .method = HTTP_GET, .handler = h_flash_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}
