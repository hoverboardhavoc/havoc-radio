# The SSB patch is fetched into the build directory and embedded when available (tools/fetch_ssb_patch.py).
set(embed)
set(ssb 0)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(python PYTHON)
    set(patch "${build_dir}/ssb/ssb_patch.bin")
    file(MAKE_DIRECTORY "${build_dir}/ssb")
    execute_process(COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/../../tools/fetch_ssb_patch.py" "${patch}")
    if(EXISTS "${patch}")
        set(embed EMBED_FILES "${patch}")
        set(ssb 1)
    endif()
endif()

idf_component_register(SRCS "radio_model.cpp" "radio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES rds guide
                       PRIV_REQUIRES board si4732 esp8266
                       ${embed})
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_SSB_PATCH=${ssb})
