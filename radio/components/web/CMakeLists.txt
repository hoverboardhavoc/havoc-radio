idf_component_register(SRCS "web.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server app_update json lwip radio ui net power store
                       EMBED_TXTFILES "page/index.html")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
