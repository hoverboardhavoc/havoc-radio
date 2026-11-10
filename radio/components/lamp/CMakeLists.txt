idf_component_register(SRCS "lamp_core.cpp" "lamp.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES radio
                       PRIV_REQUIRES board esp8266 freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
