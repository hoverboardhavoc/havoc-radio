# The page: installer/page/index.html with the wiring diagram and the stock pictures inlined, gzipped.
set(PAGE_SRC "${CMAKE_CURRENT_LIST_DIR}/../page/index.html")
set(WIRING_SVG "${CMAKE_CURRENT_LIST_DIR}/../../docs/images/installer-wiring.svg")
set(MAKE_PAGE "${CMAKE_CURRENT_LIST_DIR}/../tools/make_page.py")
file(GLOB PAGE_PICTURES "${CMAKE_CURRENT_LIST_DIR}/../../docs/images/stock/*.png")
set(PAGE_GZ "${CMAKE_BINARY_DIR}/page.html.gz")
set(MAKE_STAGE "${CMAKE_CURRENT_LIST_DIR}/../tools/make_stage.py")

idf_component_register(
    SRCS "main.c" "server.c" "dns_reply.c" "update_session.c" "stage_layout.c" "records.c"
         "stage.c" "flash_job.c" "uart_bridge.c" "wifi.c" "web.c" "activity.c" "led.c" "self_update.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_server esp_driver_gpio
             esp_driver_uart lwip esp_partition json mbedtls app_update bootloader_support
             esp_app_format)

idf_build_get_property(python PYTHON)

# The stage partition size for stage_layout.h, from partitions.csv. The
# partition_table component already reruns CMake when partitions.csv changes.
execute_process(COMMAND ${python} "${MAKE_STAGE}" --size
    OUTPUT_VARIABLE STAGE_SIZE OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MAKE_STAGE}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE SL_STAGE_MAX=${STAGE_SIZE})

add_custom_command(OUTPUT "${PAGE_GZ}"
    COMMAND ${python} "${MAKE_PAGE}" "${PAGE_SRC}" "${WIRING_SVG}" "${PAGE_GZ}"
    DEPENDS "${PAGE_SRC}" "${WIRING_SVG}" "${MAKE_PAGE}" ${PAGE_PICTURES}
    VERBATIM)
add_custom_target(installer_page DEPENDS "${PAGE_GZ}")
add_dependencies(${COMPONENT_LIB} installer_page)
target_add_binary_data(${COMPONENT_LIB} "${PAGE_GZ}" BINARY)
