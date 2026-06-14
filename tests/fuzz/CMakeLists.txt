cmake_minimum_required(VERSION 3.16)
project(micro_vorbis_fuzz CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libFuzzer is the default. Apple clang ships without libclang_rt.fuzzer, so
# point CMAKE_CXX_COMPILER at Homebrew LLVM (e.g. /opt/homebrew/opt/llvm/bin/clang++).
# Pass -DFUZZ_USE_LIBFUZZER=OFF for the standalone harness instead.
option(FUZZ_USE_LIBFUZZER "Build as a libFuzzer target" ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing requires Clang. Set CMAKE_CXX_COMPILER to clang++.")
endif()

# ASan + UBSan. The left-shift-of-negative UB in Tremor's fixed-point math
# has been fixed in the fork (unsigned-cast shifts; see src/tremor/CHANGES.md),
# so the `shift` check stays on. `signed-integer-overflow` (deliberate modular
# int32 arithmetic in the MDCT/residue DSP) and `integer-divide-by-zero`
# (codebook params on malformed input) remain disabled so they don't drown out
# real memory-safety findings.
set(FUZZ_FLAGS "-g -fno-omit-frame-pointer -fsanitize=address,undefined")
set(FUZZ_FLAGS "${FUZZ_FLAGS} -fno-sanitize=signed-integer-overflow,integer-divide-by-zero")
set(FUZZ_FLAGS "${FUZZ_FLAGS} -fno-sanitize-recover=all")

if(FUZZ_USE_LIBFUZZER)
    set(FUZZ_FLAGS "${FUZZ_FLAGS} -fsanitize=fuzzer-no-link")
endif()

set(CMAKE_C_FLAGS          "${CMAKE_C_FLAGS} ${FUZZ_FLAGS}")
set(CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} ${FUZZ_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FUZZ_FLAGS}")

# Silence per-error stderr logging in the decoder during fuzzing; malformed
# inputs are the whole point of the corpus, so the demuxer-error fprintf
# (gated on this macro in ogg_vorbis_decoder.cpp) would drown out sanitizer
# output. Set globally so the static library TUs see it too. Convention
# borrowed from Chromium/OpenSSL; clang does not define it automatically.
add_compile_definitions(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1)

# On macOS with Homebrew LLVM, link against LLVM's libc++ so the sanitizer
# runtime and the C++ library agree. The clang++ driver already adds -lc++
# implicitly; the -L/-rpath just steers it to LLVM's copy over the system one.
# (An explicit -lc++ here would collide with the sanitizer-injected one and
# draw a harmless "ignoring duplicate libraries: '-lc++'" warning from ld.)
if(APPLE)
    get_filename_component(LLVM_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(LLVM_PREFIX  "${LLVM_BIN_DIR}" DIRECTORY)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L${LLVM_PREFIX}/lib/c++ -Wl,-rpath,${LLVM_PREFIX}/lib/c++")
endif()

# Pull in micro-ogg-demuxer (same pattern as host_examples/vorbis_to_wav).
if(NOT TARGET micro_ogg_demuxer)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/micro-ogg-demuxer
                     ${CMAKE_CURRENT_BINARY_DIR}/micro-ogg-demuxer)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-vorbis-build)

# --- Ogg Vorbis streaming fuzzer --------------------------------------------
add_executable(fuzz_ogg_vorbis_decode fuzz_ogg_vorbis_decode.cpp)
target_link_libraries(fuzz_ogg_vorbis_decode PRIVATE micro_vorbis)

if(FUZZ_USE_LIBFUZZER)
    target_compile_options(fuzz_ogg_vorbis_decode PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_ogg_vorbis_decode    PRIVATE -fsanitize=fuzzer)
else()
    target_compile_definitions(fuzz_ogg_vorbis_decode PRIVATE FUZZ_STANDALONE=1)
endif()
