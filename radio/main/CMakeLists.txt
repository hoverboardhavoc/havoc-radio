idf_component_register(SRCS "main.cpp"
                       REQUIRES board i2c ssd1306 gfx ui radio input store net web power clock lamp guide esp8266)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
# libstdc++ uses the pthread condition variables; the SDK's pthread component forces a symbol name that does
# not exist (pthread_include_pthread_cond_impl), so the implementation is pulled in here by its real name.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pthread_include_pthread_cond_var_impl")
