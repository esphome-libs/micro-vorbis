# cmake/functions.cmake
# Helper functions for microVorbis build system

# Guard against multiple inclusion
if(__tremor_functions_defined)
    return()
endif()
set(__tremor_functions_defined TRUE)

# ==============================================================================
# tremor_set_optimization_flags
# ==============================================================================
# Sets common optimization compiler flags.
#
# Arguments:
#   TARGET - The target to apply flags to
# ==============================================================================
function(tremor_set_optimization_flags TARGET)
    target_compile_options(${TARGET} PRIVATE
        -ffunction-sections
        -fdata-sections
    )
    # Drop to -O1 under sanitizers so ASan/UBSan reports map cleanly back to
    # source lines; -O3 inlining/reordering otherwise muddies the diagnostics.
    # ENABLE_SANITIZERS is unset on the ESP-IDF path, so it stays at -O3 there.
    if(ENABLE_SANITIZERS)
        target_compile_options(${TARGET} PRIVATE -O1)
    else()
        target_compile_options(${TARGET} PRIVATE -O3)
    endif()
endfunction()
