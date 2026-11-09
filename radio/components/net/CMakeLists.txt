idf_component_register(SRCS "net.cpp" INCLUDE_DIRS "include" REQUIRES ui PRIV_REQUIRES store mdns esp8266 tcpip_adapter)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
