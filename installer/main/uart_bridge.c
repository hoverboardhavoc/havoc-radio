#include "uart_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "uart";

// ESP32-C3 SuperMini to the radio's header U15. Avoids the C3 strapping pins
// (GPIO2/8/9), the native-USB pins (18/19) and the UART0 console pins (20/21).
#define PIN_UART_TX GPIO_NUM_4  // C3 UART1 TX -> radio RXD
#define PIN_UART_RX GPIO_NUM_5  // C3 UART1 RX <- radio TXD
#define PIN_IO0 GPIO_NUM_6      // open-drain control of the radio's IO0 (GPIO0)
#define PIN_EN GPIO_NUM_7       // open-drain control of the radio's EN (CH_PD)

#define UART_RX_BUF 4096
#define SOCKET_PORT 3333
// The radio's boot ROM, bootloader and Havoc Radio all log at 74880.
#define DEFAULT_BAUD 74880
#define BRIDGE_BUF 1024
#define BRIDGE_SELECT_MS 20
#define CONSOLE_FRAME_MAX 512
#define MAX_CLIENTS 16

// Line level convention: 0 = drive low (asserted), 1 = release to Hi-Z.
#define LINE_ASSERT 0
#define LINE_RELEASE 1

static volatile int s_baud = DEFAULT_BAUD;
static httpd_handle_t s_server;

// The socket client, the consoles and a flash job share the UART.
static portMUX_TYPE s_owner_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_socket_client;
static bool s_flash_job;

static bool job_owns(void)
{
    taskENTER_CRITICAL(&s_owner_lock);
    bool job = s_flash_job;
    taskEXIT_CRITICAL(&s_owner_lock);
    return job;
}

// ---- IO0 / EN --------------------------------------------------------------
// Power-up safety: IO0 and EN are open-drain and released (Hi-Z) at boot and
// between every operation; the firmware drives them low only in the explicit
// sequences below. At the C3's own reset the GPIOs come up as inputs, and the
// output latch is set high before output is enabled, so the lines are never
// pulled low while the C3 restarts and the radio boots its own firmware. Driving
// low only, the C3 never sources current into the radio's lines.

static void lines_init(void)
{
    gpio_set_level(PIN_IO0, LINE_RELEASE);
    gpio_set_level(PIN_EN, LINE_RELEASE);
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_IO0) | (1ULL << PIN_EN),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_IO0, LINE_RELEASE);
    gpio_set_level(PIN_EN, LINE_RELEASE);
}

// Classic esptool sequence: hold IO0 low, pulse EN low, release EN so the chip
// samples IO0 low and starts its ROM loader, then release IO0.
void target_enter_bootloader(void)
{
    gpio_set_level(PIN_IO0, LINE_ASSERT);
    gpio_set_level(PIN_EN, LINE_ASSERT);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_EN, LINE_RELEASE);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_IO0, LINE_RELEASE);
}

// IO0 released, pulse EN low: the chip boots its own firmware.
void target_reset_run(void)
{
    gpio_set_level(PIN_IO0, LINE_RELEASE);
    gpio_set_level(PIN_EN, LINE_ASSERT);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_EN, LINE_RELEASE);
}

bool bridge_io0_low(void) { return gpio_get_level(PIN_IO0) == 0; }
bool bridge_en_low(void) { return gpio_get_level(PIN_EN) == 0; }

// ---- UART ------------------------------------------------------------------

void uart_bridge_init(void)
{
    lines_init();
    uart_config_t uc = {
        .baud_rate = s_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART, UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART, PIN_UART_TX, PIN_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// While a flash job holds the UART the new baud is only recorded; the job
// applies it when it gives the UART back.
static void apply_baud(int baud)
{
    s_baud = baud;
    if (!job_owns()) {
        uart_set_baudrate(BRIDGE_UART, baud);
    }
}

int bridge_baud(void) { return s_baud; }

// ---- Console clients -----------------------------------------------------------

// The WebSocket clients of the server; /ws/console is its only WebSocket.
static int console_fds(int *fds)
{
    size_t n = MAX_CLIENTS;
    int all[MAX_CLIENTS];
    if (s_server == NULL || httpd_get_client_list(s_server, &n, all) != ESP_OK) {
        return 0;
    }
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, all[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            if (fds) {
                fds[count] = all[i];
            }
            count++;
        }
    }
    return count;
}

int bridge_console_clients(void) { return console_fds(NULL); }
bool bridge_socket_client(void) { return s_socket_client; }

const char *bridge_claim_for_job(void)
{
    // Checked again under the lock for the socket; a console that connects
    // after this check gets the busy message.
    if (console_fds(NULL) > 0) {
        return "console client connected";
    }
    const char *err = NULL;
    taskENTER_CRITICAL(&s_owner_lock);
    if (s_flash_job) {
        err = "flash job running";
    } else if (s_socket_client) {
        err = "socket-bridge client connected";
    } else {
        s_flash_job = true;
    }
    taskEXIT_CRITICAL(&s_owner_lock);
    return err;
}

void bridge_release_from_job(void)
{
    uart_set_baudrate(BRIDGE_UART, s_baud);
    uart_flush_input(BRIDGE_UART);
    taskENTER_CRITICAL(&s_owner_lock);
    s_flash_job = false;
    taskEXIT_CRITICAL(&s_owner_lock);
}

// Text frames must be UTF-8: bytes other than printable ASCII, tab, CR, LF and
// ESC (boot ROM noise at the wrong baud) become '?'. The page removes the ANSI
// colour sequences that start with ESC.
static void console_broadcast(const uint8_t *data, int len)
{
    int fds[MAX_CLIENTS];
    int n = console_fds(fds);
    if (n == 0) {
        return;
    }
    char text[BRIDGE_BUF];
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];
        text[i] = (c >= 0x20 && c < 0x7F) || c == '\t' || c == '\r' || c == '\n' || c == 0x1B ? (char)c : '?';
    }
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = (size_t)len};
    for (int i = 0; i < n; i++) {
        httpd_ws_send_frame_async(s_server, fds[i], &frame);
    }
}

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text)};
    return httpd_ws_send_frame(req, &frame);
}

static esp_err_t h_console(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {  // the handshake is done
        if (job_owns()) {
            send_text(req, "busy: a flash job is using the radio's serial port\r\n");
            httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
            ESP_LOGW(TAG, "console client refused: flash job running");
        } else {
            ESP_LOGI(TAG, "console client connected, baud %d", s_baud);
        }
        return ESP_OK;
    }
    uint8_t buf[CONSOLE_FRAME_MAX];
    httpd_ws_frame_t frame = {0};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len > sizeof(buf)) {
        return ESP_FAIL;
    }
    frame.payload = buf;
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.type == HTTPD_WS_TYPE_TEXT || frame.type == HTTPD_WS_TYPE_BINARY) {
        if (job_owns()) {
            return send_text(req, "busy: a flash job is using the radio's serial port\r\n");
        }
        uart_write_bytes(BRIDGE_UART, buf, frame.len);
    }
    return ESP_OK;
}

// ---- Socket bridge on TCP port 3333 ------------------------------------------------
// One task accepts one client at a time, forwards its bytes to the UART, and
// forwards the radio's bytes to it and to the console clients. While a flash job
// holds the UART it reads nothing.

static int listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(SOCKET_PORT),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void socket_accept(int lfd, int *client)
{
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int fd = accept(lfd, (struct sockaddr *)&src, &slen);
    if (fd < 0) {
        return;
    }
    char ip[16];
    inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
    taskENTER_CRITICAL(&s_owner_lock);
    bool busy = s_flash_job || s_socket_client;
    if (!busy) {
        s_socket_client = true;
    }
    taskEXIT_CRITICAL(&s_owner_lock);
    if (busy) {
        ESP_LOGW(TAG, "socket client %s refused: %s", ip, *client >= 0 ? "a client is connected" : "flash job running");
        close(fd);
        return;
    }
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    // Apply the configured baud fresh for each connection.
    apply_baud(s_baud);
    uart_flush_input(BRIDGE_UART);
    *client = fd;
    ESP_LOGI(TAG, "socket client %s connected, baud %d", ip, s_baud);
}

static void socket_drop(int *client)
{
    close(*client);
    *client = -1;
    taskENTER_CRITICAL(&s_owner_lock);
    s_socket_client = false;
    taskEXIT_CRITICAL(&s_owner_lock);
    ESP_LOGI(TAG, "socket client disconnected");
}

static void bridge_task(void *arg)
{
    (void)arg;
    int lfd = listen_socket();
    if (lfd < 0) {
        ESP_LOGE(TAG, "socket bridge: cannot listen on tcp/%d", SOCKET_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "socket bridge on tcp/%d", SOCKET_PORT);
    int client = -1;
    uint8_t buf[BRIDGE_BUF];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;
        if (client >= 0) {
            FD_SET(client, &rfds);
            maxfd = client > lfd ? client : lfd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = BRIDGE_SELECT_MS * 1000};
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(lfd, &rfds)) {
            socket_accept(lfd, &client);
        }
        if (r > 0 && client >= 0 && FD_ISSET(client, &rfds)) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                socket_drop(&client);
            } else {
                uart_write_bytes(BRIDGE_UART, buf, n);
            }
        }
        if (job_owns()) {
            continue;
        }
        int len;
        while ((len = uart_read_bytes(BRIDGE_UART, buf, sizeof(buf), 0)) > 0) {
            for (int off = 0; client >= 0 && off < len;) {
                int w = send(client, buf + off, len - off, 0);
                if (w < 0) {
                    socket_drop(&client);
                    break;
                }
                off += w;
            }
            console_broadcast(buf, len);
        }
    }
}

// ---- HTTP line control ---------------------------------------------------------------

// The value of a single query key as an integer, or fallback if absent.
static int query_int(httpd_req_t *req, const char *key, int fallback)
{
    char q[64];
    char val[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) {
        return fallback;
    }
    return atoi(val);
}

static esp_err_t reply(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t h_enter_bootloader(httpd_req_t *req)
{
    target_enter_bootloader();
    ESP_LOGI(TAG, "enter-bootloader");
    return reply(req, "ok: target in download mode\n");
}

static esp_err_t h_reset_run(httpd_req_t *req)
{
    target_reset_run();
    ESP_LOGI(TAG, "reset-run");
    return reply(req, "ok: target reset to run\n");
}

static esp_err_t h_line(httpd_req_t *req, gpio_num_t pin, const char *name)
{
    int level = query_int(req, "level", -1);
    if (level != 0 && level != 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        return reply(req, "need level=0 (drive low) or level=1 (release)\n");
    }
    gpio_set_level(pin, level);
    char msg[32];
    snprintf(msg, sizeof(msg), level == LINE_ASSERT ? "ok: %s low\n" : "ok: %s released\n", name);
    return reply(req, msg);
}

static esp_err_t h_io0(httpd_req_t *req) { return h_line(req, PIN_IO0, "io0"); }
static esp_err_t h_en(httpd_req_t *req) { return h_line(req, PIN_EN, "en"); }

static esp_err_t h_baud(httpd_req_t *req)
{
    int b = query_int(req, "b", -1);
    if (b < 300 || b > 4000000) {
        httpd_resp_set_status(req, "400 Bad Request");
        return reply(req, "need b=<baud> in [300, 4000000]\n");
    }
    apply_baud(b);
    char msg[48];
    snprintf(msg, sizeof(msg), "ok: baud %d\n", b);
    ESP_LOGI(TAG, "baud set to %d", b);
    return reply(req, msg);
}

void uart_bridge_start(httpd_handle_t server)
{
    s_server = server;
    const httpd_uri_t uris[] = {
        {.uri = "/enter-bootloader", .method = HTTP_GET, .handler = h_enter_bootloader},
        {.uri = "/reset-run", .method = HTTP_GET, .handler = h_reset_run},
        {.uri = "/io0", .method = HTTP_GET, .handler = h_io0},
        {.uri = "/en", .method = HTTP_GET, .handler = h_en},
        {.uri = "/baud", .method = HTTP_GET, .handler = h_baud},
        {.uri = "/ws/console", .method = HTTP_GET, .handler = h_console, .is_websocket = true},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
    xTaskCreate(bridge_task, "bridge", 6144, NULL, 5, NULL);
}
