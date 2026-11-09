idf_component_register(SRCS "store.cpp" INCLUDE_DIRS "include" REQUIRES radio PRIV_REQUIRES nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
