cmake_minimum_required(VERSION 3.16)
project(micro_vorbis_tests CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# script/clang-tidy.sh lints the tests against this build's compile DB
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to enable sanitizers for debugging
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Treat warnings as errors (also picked up by the micro_vorbis library build below)
option(ENABLE_WERROR "Treat warnings as errors" OFF)

if(ENABLE_SANITIZERS)
    message(STATUS "Building with AddressSanitizer and UndefinedBehaviorSanitizer")
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# Add microOggDemuxer library first
if(NOT TARGET micro_ogg_demuxer)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/micro-ogg-demuxer
                     ${CMAKE_CURRENT_BINARY_DIR}/micro-ogg-demuxer)
endif()

# Add microVorbis as a subdirectory (builds as a standard host library)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. micro-vorbis-build)

add_executable(test_ogg_vorbis_decoder test_ogg_vorbis_decoder.cpp)
target_link_libraries(test_ogg_vorbis_decoder PRIVATE micro_vorbis)
target_compile_options(test_ogg_vorbis_decoder PRIVATE
    -Wall
    -Wextra
    $<$<BOOL:${ENABLE_WERROR}>:-Werror>
)

# Test fixtures are checked in; regenerate with generate_test_data.sh if the
# fixture set needs to change (requires ffmpeg + oggenc).
set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
foreach(fixture
    mono_44100.ogg
    stereo_44100.ogg
    stereo_8000.ogg
    surround30_44100.ogg
    quad_44100.ogg
    surround50_44100.ogg
    surround51_48000.ogg
    surround61_48000.ogg
    surround71_48000.ogg)
    if(NOT EXISTS ${TEST_DATA_DIR}/${fixture})
        message(FATAL_ERROR "Test fixture missing: ${fixture}; run generate_test_data.sh first")
    endif()
endforeach()

enable_testing()

set(DECODER_TESTS
    pcm_format_contract
    chunked_invariance
    small_output_buffer_recovery
    end_of_stream_sticky
    reset_reuse
    downmix_mono_from_stereo
    upmix_stereo_from_mono
    downmix_stereo_from_multichannel
    selection_planes_bit_exact
    selection_absent_role_silence
    selection_mono_routing
    selection_invalid_fallback
    error_contract
    crc_validation
    corrupt_audio_packet_survives
)

foreach(test_name IN LISTS DECODER_TESTS)
    add_test(NAME ${test_name}
             COMMAND test_ogg_vorbis_decoder ${TEST_DATA_DIR} ${test_name})
endforeach()
