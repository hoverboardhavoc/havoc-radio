idf_component_register(SRCS "download_core.cpp" "download.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES guide
                       PRIV_REQUIRES ui input ssd1306 store nvs_flash app_update esp8266 esp-tls tcpip_adapter esp_event
                                     freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
