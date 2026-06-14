# cmake/esp-idf.cmake
# ESP-IDF specific build configuration for microVorbis

# Guard against multiple inclusion
if(__vorbis_esp_idf_defined)
    return()
endif()
set(__vorbis_esp_idf_defined TRUE)

# ==============================================================================
# tremor_configure_esp_idf
# ==============================================================================
# Main configuration function for ESP-IDF builds. Call this after
# idf_component_register() to set up all ESP-IDF specific configuration.
#
# Requires TREMOR_SOURCES to be populated in the calling scope (via
# tremor_get_sources) before this is called: the -w warning suppression below
# reads it. CMake's dynamic scoping makes the function see the caller's
# variable, but the coupling is implicit -- keep tremor_get_sources first.
#
# Arguments:
#   COMPONENT_LIB     - The component library target name
#   COMPONENT_DIR     - The component directory path
# ==============================================================================
function(tremor_configure_esp_idf COMPONENT_LIB COMPONENT_DIR)
    # Get IDF target
    idf_build_get_property(target IDF_TARGET)

    # Add micro-ogg-demuxer as a subdirectory
    if(NOT TARGET micro_ogg_demuxer)
        add_subdirectory(${COMPONENT_DIR}/lib/micro-ogg-demuxer
                         ${CMAKE_CURRENT_BINARY_DIR}/micro-ogg-demuxer)
    endif()
    target_link_libraries(${COMPONENT_LIB} PUBLIC micro_ogg_demuxer)

    # Internal includes (public include/ is set in the top-level CMakeLists.txt)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${COMPONENT_DIR}/src/tremor"
        "${COMPONENT_DIR}/src"
    )

    # Configure memory allocation based on Kconfig options
    _tremor_configure_memory_allocation(${COMPONENT_LIB})

    # Xtensa asm fast paths key off the compiler-defined __XTENSA__ macro
    # in the source. No build flag needed.

    # Set optimization flags
    tremor_set_optimization_flags(${COMPONENT_LIB})

    # Suppress warnings from upstream tremor and libogg code
    # These are third-party sources we don't control
    set_source_files_properties(
        ${TREMOR_SOURCES}
        PROPERTIES
        COMPILE_FLAGS "-w"
    )

    message(STATUS "Vorbis: Configured for ESP-IDF target ${target}")
endfunction()

# ==============================================================================
# Internal helper functions
# ==============================================================================

# Configure memory allocation based on Kconfig options
function(_tremor_configure_memory_allocation TARGET)
    # Vorbis decoder state memory preference
    if(CONFIG_MICRO_VORBIS_STATE_PREFER_PSRAM)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_STATE_PREFER_PSRAM)
        message(STATUS "Vorbis: Decoder state - prefer PSRAM, fall back to internal RAM")
    elseif(CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL)
        message(STATUS "Vorbis: Decoder state - prefer internal RAM, fall back to PSRAM")
    elseif(CONFIG_MICRO_VORBIS_STATE_PSRAM_ONLY)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_STATE_PSRAM_ONLY)
        message(STATUS "Vorbis: Decoder state - PSRAM only")
    elseif(CONFIG_MICRO_VORBIS_STATE_INTERNAL_ONLY)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_STATE_INTERNAL_ONLY)
        message(STATUS "Vorbis: Decoder state - internal RAM only")
    endif()

    # OggVorbisDecoder buffer memory preference
    if(CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_PSRAM)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_PSRAM)
        message(STATUS "Vorbis: Ogg decoder buffers - prefer PSRAM, fall back to internal RAM")
    elseif(CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_INTERNAL)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_INTERNAL)
        message(STATUS "Vorbis: Ogg decoder buffers - prefer internal RAM, fall back to PSRAM")
    elseif(CONFIG_MICRO_VORBIS_OGG_DECODER_PSRAM_ONLY)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_OGG_DECODER_PSRAM_ONLY)
        message(STATUS "Vorbis: Ogg decoder buffers - PSRAM only")
    elseif(CONFIG_MICRO_VORBIS_OGG_DECODER_INTERNAL_ONLY)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_OGG_DECODER_INTERNAL_ONLY)
        message(STATUS "Vorbis: Ogg decoder buffers - internal RAM only")
    endif()

    # Codebook table memory preference
    if(CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_INTERNAL)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_INTERNAL)
        message(STATUS "Vorbis: Codebook tables - prefer internal RAM, fall back to PSRAM")
    elseif(CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_PSRAM)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_PSRAM)
        message(STATUS "Vorbis: Codebook tables - prefer PSRAM, fall back to internal RAM")
    elseif(CONFIG_MICRO_VORBIS_CODEBOOK_PSRAM_ONLY)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_CODEBOOK_PSRAM_ONLY)
        message(STATUS "Vorbis: Codebook tables - PSRAM only")
    elseif(CONFIG_MICRO_VORBIS_CODEBOOK_INTERNAL_ONLY)
        target_compile_definitions(${TARGET} PRIVATE CONFIG_MICRO_VORBIS_CODEBOOK_INTERNAL_ONLY)
        message(STATUS "Vorbis: Codebook tables - internal RAM only")
    endif()
endfunction()
