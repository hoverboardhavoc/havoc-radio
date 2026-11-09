idf_component_register(SRCS "power.cpp" INCLUDE_DIRS "include" PRIV_REQUIRES board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
