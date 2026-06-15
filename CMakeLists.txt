# CMakeLists.txt for microVorbis
# Supports both ESP-IDF component builds and standalone host builds
#
# Uses a forked copy of tremor in src/tremor/.
#
# Structure:
#   cmake/sources.cmake   - Source file definitions (function-based)
#   cmake/functions.cmake - Helper functions
#   cmake/esp-idf.cmake   - ESP-IDF specific configuration
#   cmake/host.cmake      - Host platform configuration

# ==============================================================================
# Build type detection
# ==============================================================================

if(DEFINED IDF_TARGET)
    set(ESP_IDF_BUILD TRUE)
else()
    set(ESP_IDF_BUILD FALSE)
endif()

# Set source directory variable for consistent use
if(ESP_IDF_BUILD)
    set(VORBIS_COMPONENT_DIR ${COMPONENT_DIR})
else()
    set(VORBIS_COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ==============================================================================
# Include CMake modules
# ==============================================================================

include(${VORBIS_COMPONENT_DIR}/cmake/sources.cmake)
include(${VORBIS_COMPONENT_DIR}/cmake/functions.cmake)

# ==============================================================================
# ESP-IDF Build
# ==============================================================================

if(ESP_IDF_BUILD)
    include(${VORBIS_COMPONENT_DIR}/cmake/esp-idf.cmake)

    # Get IDF target
    idf_build_get_property(target IDF_TARGET)

    # Get sources from forked tremor
    tremor_get_sources(${COMPONENT_DIR})

    # Collect all sources
    set(ESP_VORBIS_SOURCES
        ${TREMOR_SOURCES}
        ${OGG_VORBIS_SOURCES}
    )

    # Register the component (include/ is the only public header dir)
    idf_component_register(
        SRCS ${ESP_VORBIS_SOURCES}
        INCLUDE_DIRS "include"
    )

    # Apply ESP-IDF configuration
    tremor_configure_esp_idf(${COMPONENT_LIB} ${COMPONENT_DIR})

# ==============================================================================
# Host Build
# ==============================================================================

else()
    cmake_minimum_required(VERSION 3.16)
    project(micro_vorbis VERSION 0.1.1 LANGUAGES C CXX)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/host.cmake)

    # Get sources from forked tremor
    tremor_get_sources(${CMAKE_CURRENT_SOURCE_DIR})

    # Collect all sources
    set(HOST_VORBIS_SOURCES
        ${TREMOR_SOURCES}
        ${OGG_VORBIS_SOURCES}
    )

    # Create the library; include/ is the only public header dir (matches ESP-IDF)
    add_library(micro_vorbis STATIC ${HOST_VORBIS_SOURCES})
    target_include_directories(micro_vorbis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Host build options
    option(ENABLE_WERROR "Treat warnings as errors" OFF)

    # Apply host configuration
    tremor_configure_host(micro_vorbis ${CMAKE_CURRENT_SOURCE_DIR})

endif()
