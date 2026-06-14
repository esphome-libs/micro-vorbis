cmake_minimum_required(VERSION 3.16)
project(vorbis_to_wav CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to enable sanitizers for debugging
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Configure sanitizers if enabled
if(ENABLE_SANITIZERS)
    message(STATUS "Building with AddressSanitizer and UndefinedBehaviorSanitizer")
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# Add microOggDemuxer library first
# Check if it's already been added
if(NOT TARGET micro_ogg_demuxer)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/micro-ogg-demuxer
                     ${CMAKE_CURRENT_BINARY_DIR}/micro-ogg-demuxer)
endif()

# Add microVorbis as a subdirectory
# This will build it as a standard library (not ESP-IDF component)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-vorbis-build)

# Create vorbis_to_wav executable
add_executable(vorbis_to_wav
    vorbis_to_wav.cpp
    wav_writer.cpp
)

# Link against micro_vorbis library
target_link_libraries(vorbis_to_wav PRIVATE
    micro_vorbis
)

# Warning flags for the vorbis_to_wav executable itself (the library's own
# flags are set in cmake/host.cmake). ENABLE_WERROR is defined by the root
# CMakeLists added above.
target_compile_options(vorbis_to_wav PRIVATE
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

# Install target
install(TARGETS vorbis_to_wav
    RUNTIME DESTINATION bin
)
