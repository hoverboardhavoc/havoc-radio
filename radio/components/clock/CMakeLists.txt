idf_component_register(SRCS "clock_core.cpp" "utc_clock.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lwip esp8266 freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
