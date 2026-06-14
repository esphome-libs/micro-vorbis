# microVorbis: Claude Development Guide

ESP-IDF component wrapping a forked Tremor (integer Vorbis decoder) with PSRAM support and an arena allocator for ESP32 platforms.

## Documentation Map

- [README.md](README.md) - Public API, usage, Kconfig options, performance and memory numbers
- [src/README.md](src/README.md) - Internal architecture: decode flow, memory pools (block arena, DSP setup arena, codebook heap), Tremor fork changes, Xtensa optimizations
- [src/tremor/CHANGES.md](src/tremor/CHANGES.md) - File-by-file changelog of the Tremor fork vs upstream

## Layout

```text
src/tremor/                 # Forked Tremor decoder, modified in place (no submodule, no patches)
src/                        # OggVorbisDecoder C++ wrapper + Vorbis header parsing
include/micro_vorbis/       # Public API headers
lib/micro-ogg-demuxer/      # Ogg parser (the only git submodule)
cmake/                      # Build modules (source lists, ESP-IDF and host configuration)
examples/decode_benchmark/  # ESP32 benchmark example
host_examples/              # vorbis_to_wav CLI decoder
tests/                      # OggVorbisDecoder unit tests (ctest; fixtures in tests/data/)
tests/fuzz/                 # libFuzzer harness (self-contained CMake project, not part of ctest)
```

## Build and Test

### ESP32 (PlatformIO)

```bash
cd examples/decode_benchmark
pio run -e esp32s3                       # build (also: -e esp32)
pio run -e esp32s3 -t upload -t monitor  # flash and watch benchmark output
pio run -e esp32s3 -t menuconfig         # Component config, microVorbis Decoder
```

### Host (macOS/Linux)

```bash
cd host_examples/vorbis_to_wav
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
./build/vorbis_to_wav input.ogg output.wav
```

### Unit tests

```bash
cd tests
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Fixtures are checked into `tests/data/`; regenerate with `tests/generate_test_data.sh` (needs ffmpeg and oggenc).

Add `-DENABLE_WERROR=ON` to the host or test cmake command to treat warnings as errors (off by default).

## Working Notes

- Edit Tremor files directly in `src/tremor/` - there is no staging or patching step
- Correctness validation: compare 16-bit output against ffmpeg's native Vorbis decoder (`ffmpeg -i x.ogg -c:a pcm_s16le ref.wav`); a correct decode has a peak difference of <= 2 LSB
- Use `cmp` (not `diff`) for binary WAV comparison
- Tremor uses fixed-point math throughout - watch for integer overflow when scaling samples
- Decoder state can be large and arena sizes scale with codec setup - prefer PSRAM when available
