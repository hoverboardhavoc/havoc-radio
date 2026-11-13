idf_component_register(SRCS "store.cpp" INCLUDE_DIRS "include" REQUIRES radio guide PRIV_REQUIRES nvs_flash esp8266)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
