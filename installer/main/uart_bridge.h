// The radio's serial header: the UART, the open-drain IO0/EN lines, the raw
// socket on TCP port 3333, the console WebSocket /ws/console and the line
// control endpoints. One radio operation at a time: a flash job takes the UART
// only while no socket or console client is connected, and while it runs the
// socket closes new connections and consoles get a busy message.

#pragma once

#include <stdbool.h>

#include "driver/uart.h"
#include "esp_http_server.h"

#define BRIDGE_UART UART_NUM_1

// Bring IO0 and EN up released and install the UART. Call first in app_main.
void uart_bridge_init(void);
// Register the HTTP endpoints and start the socket bridge, which also feeds the
// console clients of server.
void uart_bridge_start(httpd_handle_t server);

// ESP8266 ROM download-mode entry and normal reset.
void target_enter_bootloader(void);
void target_reset_run(void);

// The UART baud configured over /baud.
int bridge_baud(void);
bool bridge_io0_low(void);
bool bridge_en_low(void);
int bridge_console_clients(void);
bool bridge_socket_client(void);

// Take the UART for a flash job. Returns NULL, or why it cannot (a socket or
// console client is connected, or a job holds it).
const char *bridge_claim_for_job(void);
// Give the UART back at the configured baud.
void bridge_release_from_job(void);
