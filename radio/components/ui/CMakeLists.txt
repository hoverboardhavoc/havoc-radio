# The string tables are generated from ui/prototype.html at configure time; editing the prototype re-runs CMake.
set(proto "${CMAKE_CURRENT_LIST_DIR}/../../ui/prototype.html")
set(gendir "${CMAKE_CURRENT_BINARY_DIR}/gen")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    file(MAKE_DIRECTORY "${gendir}")
    execute_process(COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_strings.py" "${proto}" "${gendir}"
                    RESULT_VARIABLE rc)
    if(rc)
        message(FATAL_ERROR "gen_strings.py failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${proto}"
                 "${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_strings.py")
endif()

idf_component_register(SRCS "ui.cpp" "${gendir}/i18n_strings.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES gfx radio clock guide)
target_include_directories(${COMPONENT_LIB} PUBLIC "${gendir}")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-threadsafe-statics -Wall -Wextra -Werror)
