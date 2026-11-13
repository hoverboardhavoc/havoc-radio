cmake_minimum_required(VERSION 3.5)

# Components of the SDK that this firmware does not use.
set(EXCLUDE_COMPONENTS coap mqtt esp-wolfssl freemodbus libsodium protobuf-c protocomm
    wifi_provisioning fatfs wear_levelling spiffs esp_http_client esp_https_ota
    tcp_transport jsmn openssl esp_gdbstub spi_ram)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(havoc-radio)

# The app image must fit both app slots of partitions.csv.
partition_table_get_partition_info(ota_0_size "--partition-name ota_0" "size")
partition_table_get_partition_info(ota_1_size "--partition-name ota_1" "size")
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBIN=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
        -DSLOTS=ota_0:${ota_0_size},ota_1:${ota_1_size}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_app_size.cmake
    VERBATIM)

# The starting frequency guide, fetched from the data repository into the build directory and flashed at the guide
# partition with the app (tools/fetch_guide.py). Without it the build continues and the radio starts with no guide.
partition_table_get_partition_info(guide_offset "--partition-name guide" "offset")
set(guide_table "${CMAKE_BINARY_DIR}/guide/stations.bin")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/guide")
idf_build_get_property(python PYTHON)
execute_process(COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_guide.py" "${guide_table}")
if(EXISTS "${guide_table}")
    esptool_py_flash_project_args(guide ${guide_offset} "${guide_table}" FLASH_IN_PROJECT)
else()
    message(WARNING "No frequency guide: the install image carries none")
endif()

