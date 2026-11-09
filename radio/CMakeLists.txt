cmake_minimum_required(VERSION 3.5)

# Components of the SDK that this firmware does not use.
set(EXCLUDE_COMPONENTS coap mqtt esp-wolfssl freemodbus libsodium protobuf-c protocomm
    wifi_provisioning fatfs wear_levelling spiffs esp_http_client esp_https_ota esp-tls
    tcp_transport jsmn openssl esp_gdbstub spi_ram)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(havoc-radio)
