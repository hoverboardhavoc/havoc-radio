# The font tables are generated from ui/prototype.html at configure time; editing the prototype re-runs CMake.
set(proto "${CMAKE_CURRENT_LIST_DIR}/../../ui/prototype.html")
set(gen "${CMAKE_CURRENT_BINARY_DIR}/fonts_data.cpp")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    execute_process(COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_fonts.py" "${proto}" "${gen}"
                    RESULT_VARIABLE rc)
    if(rc)
        message(FATAL_ERROR "gen_fonts.py failed")
    endif()
    # The prototype itself is registered as a configure dependency by the ui component.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_fonts.py")
endif()

idf_component_register(SRCS "gfx.cpp" "${gen}" INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
