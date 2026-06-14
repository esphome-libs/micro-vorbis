# ESP32 microVorbis Decode Benchmark

Benchmarks Vorbis decoding performance by decoding a 30-second Ogg Vorbis clip in a loop, reporting per-frame timing statistics (min/max/avg/stddev). Demonstrates concurrent decoding with independent decoder instances across up to 4 tasks pinned to alternating cores.

## Features

- Embedded 30-second test audio clip (public domain):
  - **MUSIC**: Stereo orchestral music, ~128kbit/s (~453KB)
- Per-frame timing with statistical analysis
- Concurrent decoding demonstration with 1, 2, 3, and 4 independent decode tasks
- Tasks pinned to alternating cores (task 0 on core 0, task 1 on core 1, etc.)
- Pre-configured for maximum performance (240MHz, 360MHz on the P4, PSRAM, integer-only decoding)

## Building and Flashing

### Prerequisites

- **PlatformIO** (recommended) OR ESP-IDF v5.0 or later
- ESP32, ESP32-S3, or ESP32-P4 development board with PSRAM

### Option 1: PlatformIO (Recommended)

PlatformIO provides a simplified build process with automatic dependency management.

```bash
cd examples/decode_benchmark

# Build the project (-e esp32 for a plain ESP32, -e esp32p4 for the P4 eval board)
pio run -e esp32s3

# Upload and monitor
pio run -e esp32s3 -t upload -t monitor
```

The PlatformIO configuration uses the parent microVorbis repository as a component, so no additional setup is required.

### Option 2: Native ESP-IDF

```bash
cd examples/decode_benchmark
idf.py set-target esp32s3   # or esp32, esp32p4
idf.py build
idf.py flash monitor
```

### Configuration Options

#### PlatformIO

The default configuration is optimized for maximum performance. To customize:

1. Edit `sdkconfig.defaults` to change Vorbis-specific settings
2. Use `pio run -t menuconfig` for full ESP-IDF configuration

#### Native ESP-IDF

```bash
idf.py menuconfig
```

Navigate to **Component config, microVorbis Decoder** to adjust:

- Memory preferences (PSRAM vs internal RAM for state/buffers)

## Expected Output

Each iteration tests 1, 2, 3, and 4 concurrent tasks. The sample run below is on an ESP32-S3 (240 MHz, octal PSRAM, decoder state in PSRAM). Per-target numbers are in the [Performance Scaling](#performance-scaling) tables.

```text
I (1021) DECODE_BENCH: --- VORBIS (Vorbis) - 1 concurrent task ---
I (1021) DECODE_BENCH: Task 0 starting VORBIS decode...
I (4191) DECODE_BENCH: Task 0 finished (3164 ms)
I (4191) DECODE_BENCH: Task 0: Frame (us): min=285 max=2674 avg=2221.8 sd=175.9 (n=1415)
I (4191) DECODE_BENCH: Task 0: Total: 3164 ms (setup: 13 ms, decode: 3150 ms), 30.0s audio, RTF: 0.105 (9.5x), decode RTF: 0.105 (9.5x), core 0
I (4211) DECODE_BENCH: Task 0: Decoder footprint: 76056 bytes (internal: 20296, PSRAM: 55760) (arena + PCM buffer)

I (4221) DECODE_BENCH: --- VORBIS (Vorbis) - 2 concurrent tasks ---
I (4231) DECODE_BENCH: Task 0 starting VORBIS decode...
I (4231) DECODE_BENCH: Task 1 starting VORBIS decode...
I (9631) DECODE_BENCH: Task 0 finished (5401 ms)
I (9631) DECODE_BENCH: Task 1 finished (5398 ms)
I (9631) DECODE_BENCH: Task 0: Frame (us): min=321 max=4127 avg=3801.6 sd=307.3 (n=1415)
I (9641) DECODE_BENCH: Task 0: Total: 5401 ms (setup: 14 ms, decode: 5386 ms), 30.0s audio, RTF: 0.180 (5.6x), decode RTF: 0.180 (5.6x), core 0
I (9651) DECODE_BENCH: Task 1: Frame (us): min=291 max=4111 avg=3800.1 sd=311.2 (n=1415)
I (9661) DECODE_BENCH: Task 1: Total: 5398 ms (setup: 15 ms, decode: 5383 ms), 30.0s audio, RTF: 0.180 (5.6x), decode RTF: 0.179 (5.6x), core 1

...

I (30621) DECODE_BENCH: --- Summary ---
I (30621) DECODE_BENCH: VORBIS (Vorbis):
I (30621) DECODE_BENCH:   1 task:     3169 ms
I (30631) DECODE_BENCH:   2 tasks:    5409 ms
I (30631) DECODE_BENCH:   3 tasks:    9522 ms
I (30641) DECODE_BENCH:   4 tasks:   11269 ms
I (30641) DECODE_BENCH: All decodes successful: YES
I (30641) DECODE_BENCH: Free heap: 17106796 bytes
I (30651) DECODE_BENCH: Min free heap ever:     16670084 bytes
I (30651) DECODE_BENCH: Min free internal ever: 118508 bytes
I (30661) DECODE_BENCH: Min free PSRAM ever:    16551576 bytes
```

### Output Fields

- **Frame (us)**: Per-frame decode time statistics (min/max/avg/sd in microseconds, n = frame count)
- **Total**: Wall-clock time to process all audio, split into one-time `setup` (decoder init) and `decode`
- **RTF**: Real-Time Factor (total_time / audio_duration). RTF < 1 means faster than real-time
- **decode RTF**: Same factor with setup time excluded, so it reflects decode speed alone
- **Nx**: How many times faster than real-time playback (1/RTF)
- **Decoder footprint**: Bytes held by the decoder while running (arena + PCM buffer), split by internal RAM vs PSRAM
- **core N**: Which CPU core the task ran on

### Performance Scaling

The benchmark shows how performance scales with concurrent tasks on the dual-core ESP32-S3 (octal PSRAM, 240 MHz), where the numbers below were measured.

**VORBIS (stereo ~128kbit/s)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 3.2s | 0.11 (9.5x) | Single task on one core |
| 2 | 5.4s | 0.18 (5.6x) | One task per core |
| 3 | 9.5s | 0.32 (3.2x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 11.3s | 0.37 (2.7x) | Two tasks per core |

Each stream decodes on a single thread. A second stream on the other core raises combined throughput to about 11x real-time, but the two cores share one PSRAM bus, so each task slows down under contention.

The dual-core ESP32-P4 (hex PSRAM, 360 MHz) decodes the same clip about 1.7x faster per stream and scales nearly linearly to two tasks (one per core) before bus contention shows up at three and four:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 1.9s | 0.062 (16.2x) | Single task on one core |
| 2 | 2.0s | 0.065 (15.4x) | One task per core, ~30x combined |
| 3 | 4.6s | 0.153 (6.6x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 10.1s | 0.337 (3.0x) | Two tasks per core |

The plain single-core ESP32 (quad PSRAM WROVER, 240 MHz) caps the benchmark at one task via the `-DDECODE_BENCH_MAX_CONCURRENT_TASKS=1` flag in `platformio.ini`, and its `sdkconfig.defaults.esp32` places decoder state and demuxer buffers in internal RAM because PSRAM is much slower on the original ESP32:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 6.3s | 0.21 (4.8x) | Single task, working buffers in internal RAM |

## Concurrent Decoding

This example runs multiple independent decoder instances in parallel. Each FreeRTOS task:

- Creates its own `OggVorbisDecoder` instance
- Gets its own decoder state, allocated through the component's PSRAM-aware `heap_caps` allocators (placement set by Kconfig, PSRAM by default)
- Is pinned to a specific core (alternating 0, 1, 0, 1)
- Decodes independently without interference

All tasks decode simultaneously with correct results, confirming that separate instances do not interfere. A single instance is still not thread-safe: never share one across tasks. To decode multiple streams at once, give each task its own decoder.

**Memory usage with concurrent tasks:**

- Each task needs its own decoder footprint (~76KB for this clip; varies by file)
- With 4 concurrent tasks: roughly 304KB total
- Plus 32KB FreeRTOS stack per task

## Configuration

The default configuration uses 240MHz and enables PSRAM with `CONFIG_SPIRAM=y` and `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`. The component does not rely on `CONFIG_SPIRAM_USE_MALLOC`: it overrides Tremor's allocation macros with `heap_caps`-based functions and places each allocation by Kconfig preference (decoder state prefers PSRAM, codebook tables prefer internal RAM).

To reduce PSRAM usage or improve performance, adjust memory preferences via menuconfig:

- **Decoder state**: Prefer PSRAM (default with PSRAM enabled), prefer internal, PSRAM only, internal only
- **Ogg decoder buffers**: Prefer PSRAM (default with PSRAM enabled), prefer internal, PSRAM only, internal only
- **Codebook tables**: Prefer internal RAM (default with PSRAM enabled), prefer PSRAM, PSRAM only, internal only. Codebook lookups are random-access in the decode hot path, so moving them to PSRAM frees internal RAM (codebooks are typically 5-20KB per decoder) at a decode-speed cost.

The per-target `sdkconfig.defaults.esp32` overrides the first two to internal RAM (`CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL=y`, `CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_INTERNAL=y`) because PSRAM access is much slower on the original ESP32; codebook tables already default to internal RAM on all targets. The ESP32-S3 keeps the PSRAM default, where the placement penalty is negligible.

## Regenerating Test Audio

The included test audio uses public domain recordings. To regenerate or use different audio:

```bash
# Download source (e.g., from Musopen Collection on Archive.org)
curl -L -o source.flac "https://archive.org/download/MusopenCollectionAsFlac/..."

# Extract 30 seconds and encode to Vorbis
ffmpeg -i source.flac -ss 60 -t 30 -c:a libvorbis -q:a 5 main/test_audio.ogg

# Convert to C header
python3 convert_ogg.py main/test_audio.ogg main/test_audio.h \
    --name test_vorbis_music_data \
    --description "Audio description here"
```

Keep clips ~30 seconds to fit in flash.

**Note**: Vorbis quality is set with `-q:a` (0-10, 3-6 typical for music). Use `-b:a` for constant bitrate encoding.

## Memory Usage

| Type | Size | Notes |
| ---- | ---- | ----- |
| Flash | ~754KB | ~300KB code + 453KB music |
| Task stack | 32KB each | Per FreeRTOS task |
| Decoder footprint | 76,056 bytes per stream (20,296 internal + 55,760 PSRAM) | Measured for this clip (48kHz stereo, ~128kbit/s). Covers Tremor decoder state, codebook tables, Ogg demuxer buffers, and the output PCM buffer. The benchmark logs this per single-task run as "Decoder footprint" |
| Static RAM | ~14.5KB | Global variables |

These bytes are specific to this clip. A different file can need more or less: the figure scales with sample rate, channel count, and the codebooks the encoder used.

With 4 concurrent decode tasks the decoder footprint is roughly 4x the single-stream number, about 304KB total (~79KB internal, ~218KB PSRAM), plus a 32KB stack per task. In this run free internal RAM bottomed out at 118,508 bytes. Decoder allocations land in PSRAM by default through the component's `heap_caps` allocators.

## Troubleshooting

| Problem | Solution |
| ------- | -------- |
| Watchdog timeout | Disable in menuconfig: Component config, ESP System Settings, Task Watchdog |
| Stack overflow | Increase task stack size (32KB default) |
| Allocation failures | Check PSRAM is enabled (`CONFIG_SPIRAM=y`); this example uses `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` so PSRAM is reachable through `heap_caps` |
| Concurrent decode fails | Check each task has sufficient heap for decoder state |

## Technical Details

**Music Audio**: Beethoven Symphony No. 3 "Eroica", Op. 55, Movement I, 30s extract.

- Performer: Czech National Symphony Orchestra
- Source: [Musopen Collection](https://archive.org/details/MusopenCollectionAsFlac) on Archive.org
- License: Public Domain
- Format: Ogg Vorbis 48kHz stereo ~128kbit/s VBR
- Size: 452,876 bytes

**Decoder**: Tremor (integer-only Vorbis decoder from Xiph.Org Foundation)

- Uses fixed-point arithmetic throughout (no floating-point operations)
- Designed for embedded systems without FPU
- Lower memory footprint than reference libvorbis

**Timing**: Uses `esp_timer_get_time()` for microsecond precision. Only measures `decoder.decode()` calls that produce samples.

## Performance Notes

- **Integer-only decoding**: Tremor uses fixed-point math exclusively. No floating-point option available.
- **PSRAM configuration**: The component overrides Tremor's allocation macros (see `src/tremor/custom_allocator.h`) with `heap_caps`-based functions, so memory placement is explicit and set by Kconfig rather than the libc malloc heap. With PSRAM enabled, decoder state defaults to PSRAM; codebook tables, which are hit with random access in the decode hot path, default to faster internal RAM. This example enables PSRAM with `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`, which keeps PSRAM out of the regular malloc heap and reachable only through `heap_caps`.
- **Xtensa optimizations**: Uses `clamps` instruction for sample clipping on ESP32/ESP32-S2/ESP32-S3. Enabled automatically on Xtensa targets (compile-time `#ifdef __XTENSA__`).
- **Decoder-only**: Tremor is a decoder library only. No encoder equivalent exists.
