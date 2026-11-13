idf_component_register(SRCS "guide_core.cpp" "guide_slots.cpp" "scan_core.cpp" "guide.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES spi_flash esp8266 freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
