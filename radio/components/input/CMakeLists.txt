idf_component_register(SRCS "input_logic.cpp" "input.cpp" INCLUDE_DIRS "include" REQUIRES ui PRIV_REQUIRES board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
