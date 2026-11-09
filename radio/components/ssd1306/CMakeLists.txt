idf_component_register(SRCS "ssd1306.cpp" INCLUDE_DIRS "include" PRIV_REQUIRES board i2c)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
