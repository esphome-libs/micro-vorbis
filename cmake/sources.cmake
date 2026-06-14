# cmake/sources.cmake
# Source file definitions for microVorbis
# Separated from main CMakeLists.txt for maintainability

# Guard against multiple inclusion
if(__tremor_sources_defined)
    return()
endif()
set(__tremor_sources_defined TRUE)

# ==============================================================================
# tremor_get_sources
# ==============================================================================
# Populates source file lists from the forked tremor in src/tremor/.
# bitwise.c (from libogg) is included directly in the fork.
#
# Arguments:
#   COMPONENT_DIR - Root directory of the microVorbis component
# ==============================================================================
function(tremor_get_sources COMPONENT_DIR)
    set(TREMOR_DIR "${COMPONENT_DIR}/src/tremor")

    # --------------------------------------------------------------------------
    # Tremor codec sources (forked) + libogg bitwise.c
    # --------------------------------------------------------------------------
    set(TREMOR_SOURCES
        ${TREMOR_DIR}/bitwise.c
        ${TREMOR_DIR}/block.c
        ${TREMOR_DIR}/codebook.c
        ${TREMOR_DIR}/floor0.c
        ${TREMOR_DIR}/floor1.c
        ${TREMOR_DIR}/info.c
        ${TREMOR_DIR}/mapping0.c
        ${TREMOR_DIR}/mdct.c
        ${TREMOR_DIR}/registry.c
        ${TREMOR_DIR}/res012.c
        ${TREMOR_DIR}/synthesis.c
        ${TREMOR_DIR}/window.c
        PARENT_SCOPE
    )

    # No longer a separate list; bitwise.c is in TREMOR_SOURCES
    set(OGG_BITWISE_SOURCES "" PARENT_SCOPE)
endfunction()

# ==============================================================================
# Non-tremor sources (these don't depend on TREMOR_DIR)
# ==============================================================================

# Ogg Vorbis decoder (C++ wrapper) - in our src/ directory
set(OGG_VORBIS_SOURCES
    src/vorbis_header.cpp
    src/ogg_vorbis_decoder.cpp
)
