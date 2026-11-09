idf_component_register(SRCS "board.cpp" INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
