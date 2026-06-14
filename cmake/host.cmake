# cmake/host.cmake
# Host platform build configuration for microVorbis

# Guard against multiple inclusion
if(__vorbis_host_defined)
    return()
endif()
set(__vorbis_host_defined TRUE)

# ==============================================================================
# tremor_configure_host
# ==============================================================================
# Main configuration function for host builds (Linux, macOS, Windows).
# Call this after creating the library target to set up all host-specific
# configuration.
#
# Requires TREMOR_SOURCES to be populated in the calling scope (via
# tremor_get_sources) before this is called: the -w warning suppression below
# reads it. CMake's dynamic scoping makes the function see the caller's
# variable, but the coupling is implicit -- keep tremor_get_sources first.
#
# Arguments:
#   TARGET            - The library target name
#   SOURCE_DIR        - The source directory path (CMAKE_CURRENT_SOURCE_DIR)
# ==============================================================================
function(tremor_configure_host TARGET SOURCE_DIR)
    # Add micro-ogg-demuxer as a subdirectory
    if(NOT TARGET micro_ogg_demuxer)
        add_subdirectory(${SOURCE_DIR}/lib/micro-ogg-demuxer
                         ${CMAKE_CURRENT_BINARY_DIR}/micro-ogg-demuxer)
    endif()
    target_link_libraries(${TARGET} PUBLIC micro_ogg_demuxer)

    # Internal includes (public include/ is set in the top-level CMakeLists.txt)
    target_include_directories(${TARGET} PRIVATE
        ${SOURCE_DIR}/src/tremor
        ${SOURCE_DIR}/src
    )

    # Set language standards: C++14 for the wrapper sources, C11 for the
    # forked tremor C sources (otherwise they build at the compiler default).
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )

    # Set optimization flags
    tremor_set_optimization_flags(${TARGET})

    # Warning flags for first-party wrapper sources. The forked tremor sources
    # below override these with -w, so warnings only surface from our own code.
    target_compile_options(${TARGET} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        $<$<BOOL:${ENABLE_WERROR}>:-Werror>
    )

    # Suppress warnings from upstream tremor and libogg code
    # These are third-party sources we don't control
    set_source_files_properties(
        ${TREMOR_SOURCES}
        PROPERTIES
        COMPILE_FLAGS "-w"
    )

    message(STATUS "Vorbis: Building for host platform")
endfunction()
