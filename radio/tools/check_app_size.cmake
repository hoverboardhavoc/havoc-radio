# Fails the build when the app image is longer than an app slot. The linker bounds only the code
# mapped from flash; the image also carries the IRAM and DRAM segments.
# Usage: cmake -DBIN=<app.bin> -DSLOTS=<name:size,...> -P check_app_size.cmake
file(SIZE "${BIN}" size)
string(REPLACE "," ";" SLOTS "${SLOTS}")
foreach(slot ${SLOTS})
    string(REPLACE ":" ";" slot "${slot}")
    list(GET slot 0 name)
    list(GET slot 1 slot_size)
    math(EXPR slot_size "${slot_size}")
    if(size GREATER slot_size)
        message(FATAL_ERROR "${BIN} is ${size} bytes, longer than the ${slot_size}-byte slot ${name}")
    endif()
    message(STATUS "${BIN}: ${size} of ${slot_size} bytes in ${name}")
endforeach()
