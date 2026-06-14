# microVorbis - Embedded Ogg Vorbis Decoder

[![CI](https://github.com/esphome-libs/micro-vorbis/actions/workflows/ci.yml/badge.svg)](https://github.com/esphome-libs/micro-vorbis/actions/workflows/ci.yml)
[![Component Registry](https://components.espressif.com/components/esphome/micro-vorbis/badge.svg)](https://components.espressif.com/components/esphome/micro-vorbis)

An Ogg Vorbis audio decoder optimized for embedded devices. Fixed-point decoder forked from Tremor (libvorbisidec), based on its `lowmem` branch, with an arena allocator for decode-path allocation. Designed as an ESP-IDF component with PSRAM support and Xtensa assembly optimizations. For internal architecture see [src/README.md](src/README.md); for the fork's changes relative to upstream Tremor see [src/tremor/CHANGES.md](src/tremor/CHANGES.md).

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Features

- **Fixed-point decoding**: Uses a forked Tremor (libvorbisidec); no FPU required
- **Arena allocator**: Single pre-sized arena per decode block eliminates per-frame heap allocations
- **PSRAM support**: Configurable memory placement with automatic fallback
- **Streaming decode**: Minimal internal buffering; reduces per-stream memory footprint
- **Zero-copy Ogg parsing**: Most Ogg packets parsed without copying via [microOggDemuxer](https://github.com/esphome-libs/micro-ogg-demuxer)
- **Channel output**: Smart-downmix to mono or stereo (ITU-R BS.775), or raw per-role selection of up to 8 channels

## Quick Start

### ESP-IDF Component

Requires ESP-IDF v5.0 or later. Add via the ESP Component Registry by listing it in your project's `idf_component.yml`:

```yaml
dependencies:
  esphome/micro-vorbis: "^0.1.0"
```

Then configure via `menuconfig` (Component config, microVorbis Decoder).

### PlatformIO

Add to `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
framework = espidf
lib_deps =
    esphome/micro-vorbis
```

### Host Build (Linux/macOS)

```bash
cd host_examples/vorbis_to_wav
cmake -B build && cmake --build build
./build/vorbis_to_wav input.ogg output.wav
```

## Usage Example

### Basic Decoding

```cpp
#include "micro_vorbis/ogg_vorbis_decoder.h"
#include "esp_log.h"

#include <cinttypes>  // PRIu32 for the uint32_t PcmFormat fields

static const char *TAG = "vorbis_example";

void vorbis_decode_example(const uint8_t *ogg_data, size_t ogg_size) {
    micro_vorbis::OggVorbisDecoder decoder;

    int16_t pcm_buffer[4096 * 2];  // Large enough for any stereo stream (see max_output_bytes())

    const uint8_t *input_ptr = ogg_data;
    size_t input_len = ogg_size;

    while (input_len > 0) {
        size_t bytes_consumed, bytes_written;

        micro_vorbis::OggVorbisResult result = decoder.decode(
            input_ptr, input_len,
            reinterpret_cast<uint8_t*>(pcm_buffer), sizeof(pcm_buffer),
            bytes_consumed, bytes_written
        );

        // result < 0 is an error; result >= 0 is success/informational. pcm_buffer is big enough
        // for any stereo stream, so OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL never fires
        // here; size buffers smaller than max_output_bytes() to handle that recoverable error.
        if (result < 0) {
            ESP_LOGE(TAG, "Decode error: %d", result);
            break;
        }

        // Process any decoded PCM first (bytes_written is the total across all channels)
        if (bytes_written > 0) {
            const auto& fmt = decoder.get_pcm_format();
            ESP_LOGI(TAG, "Decoded %zu bytes at %" PRIu32 " Hz, %" PRIu32 " channels",
                     bytes_written,
                     fmt.sample_rate(),
                     fmt.num_channels());
            // Send to I2S DAC, save to file, etc.
        }

        // ...then handle end-of-stream
        if (result == micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM) {
            break;  // Stream fully decoded
        }

        input_ptr += bytes_consumed;
        input_len -= bytes_consumed;
    }
}
```

See the [decode benchmark example](examples/decode_benchmark) for a complete ESP-IDF example with performance measurements.

### Host Platform Usage

See the [vorbis_to_wav example](host_examples/vorbis_to_wav) for a complete working example that converts Ogg Vorbis files to WAV format.

## API Reference

### `OggVorbisDecoder`

| Method | Description |
| ------ | ----------- |
| `OggVorbisDecoder(uint8_t channels = 0, bool enable_crc = false)` | Smart-downmix decoder. `channels`: 0 = stream default, 1 = mono, 2 = stereo; any other value uses the stream's channel count. |
| `OggVorbisDecoder(const SpeakerRole* roles, uint8_t count, bool enable_crc = false)` | Raw channel-selecting decoder; output channel `i` is fed by the source channel matching `roles[i]`. |
| `OggVorbisResult decode(const uint8_t* input, size_t input_len, uint8_t* output, size_t output_size_bytes, size_t& bytes_consumed, size_t& bytes_written)` | Decode input bytes to interleaved 16-bit PCM; returns an informational code (`>= 0`) or an error (`< 0`). |
| `void reset()` | Reset decoder state for a new stream. Configuration and the demuxer buffer are kept; the next stream re-allocates Tremor state. |
| `size_t get_required_output_bytes() const` | Exact byte count (all channels) the last audio packet needs; 0 before the first audio packet. |
| `const PcmFormat& get_pcm_format() const` | Output PCM format (sample rate, channels, bit depth) after `OGG_VORBIS_DECODER_STREAM_INFO_READY`. |

Both constructors defer all allocations to `decode()`. The OggDemuxer buffer is allocated on the first `decode()` call; the Tremor decoder state and codebook tables are allocated as the three header packets are processed, which can span several `decode()` calls when input arrives in small chunks. The smart-downmix constructor folds surround layouts to stereo per ITU-R BS.775: center/surround at -3 dB relative to the fronts, LFE dropped, and the mix normalized so a full-scale fold cannot clip (4.6-9.9 dB of attenuation, depending on the source layout). The raw channel-selecting constructor feeds each output channel unmixed at unity gain; `count` is 1-8 (an invalid count falls back to the stream's channel count), and a role absent from the file's layout emits silence.

Buffer sizing is expressed in bytes, matching the `uint8_t*` output buffer: `output_size_bytes` and `bytes_written` share that unit directly. `bytes_written` is the total across all channels (a stereo block of 1024 frames yields 2048 samples => 4096 bytes). When the output buffer is at least `get_pcm_format().max_output_bytes()` bytes, `decode()` never returns `OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL`; after that error, `get_required_output_bytes()` gives the exact byte count the last packet needs before retrying (0 before the first audio packet). Use `get_pcm_format().is_valid()` to check whether the format has been parsed.

`OggVorbisDecoder` is not thread-safe. Use each instance from a single task, or add external synchronization.

### `PcmFormat`

Describes the decoder's output (after any downmix or channel selection), available once `decode()` returns `OGG_VORBIS_DECODER_STREAM_INFO_READY` via `decoder.get_pcm_format()`:

| Method | Description |
| ------ | ----------- |
| `bits_per_sample()` | Output bit depth (always 16; Tremor emits 16-bit signed PCM) |
| `bytes_per_sample()` | Output bytes per sample (always 2) |
| `max_output_bytes()` | Safe output buffer size, in bytes, for one `decode()` call (all channels) |
| `num_channels()` | Output channel count (1 = mono, 2 = stereo, etc.) |
| `sample_rate()` | Sample rate in Hz (e.g., 44100, 48000) |
| `is_valid()` | Whether the format has been parsed |

### `OggVorbisResult`

Non-negative values (>= 0) are success/informational states; negative values are errors. Check `result < 0` to detect errors. One error, `OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL`, is recoverable: resize the output buffer and retry.

| Code | Value | Description |
| ---- | ----- | ----------- |
| `OGG_VORBIS_DECODER_SUCCESS` | `0` | An audio frame was decoded. Check `bytes_written` for the byte count. |
| `OGG_VORBIS_DECODER_STREAM_INFO_READY` | `1` | All three Vorbis headers parsed; the PCM format (sample rate, channels) is available. |
| `OGG_VORBIS_DECODER_NEED_MORE_DATA` | `2` | No audio produced this call; feed more input data. |
| `OGG_VORBIS_DECODER_END_OF_STREAM` | `3` | Stream fully decoded; no more audio frames. |
| `OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL` | `-1` | Output buffer too small (recoverable). Call `get_required_output_bytes()` and retry. |
| `OGG_VORBIS_DECODER_ERROR_INPUT_INVALID` | `-2` | Invalid Ogg or Vorbis stream structure. |
| `OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED` | `-3` | Memory allocation failed. |
| `OGG_VORBIS_DECODER_ERROR_DECODE_FAILED` | `-4` | Vorbis decode failed (corrupted or invalid packet). |

Only `ERROR_OUTPUT_BUFFER_TOO_SMALL` is recoverable in place. After any other error, call `reset()` and feed the stream again from its first page.

## Configuration

```bash
idf.py menuconfig
# Navigate to: Component config, microVorbis Decoder
```

### Memory Placement

Each memory type can be configured independently with four placement options:

- `Prefer PSRAM`: Try PSRAM first, fall back to internal RAM
- `Prefer internal RAM`: Try internal RAM first, fall back to PSRAM
- `PSRAM only`: Strict PSRAM placement, fails if unavailable
- `Internal only`: Strict internal RAM placement, fails if unavailable

| Memory Type | Kconfig Choice | Default | Notes |
| ----------- | -------------- | ------- | ----- |
| `Decoder state` | `MICRO_VORBIS_STATE_MEMORY_PREFERENCE` | Prefer PSRAM | Tremor decoder state and persistent buffers |
| `Demuxer buffers` | `MICRO_VORBIS_OGG_DECODER_MEMORY_PREFERENCE` | Prefer PSRAM | Ogg packet assembly (most packets use zero-copy) |
| `Codebook tables` | `MICRO_VORBIS_CODEBOOK_MEMORY_PREFERENCE` | Prefer internal RAM | Huffman decode trees; random-access in the decode hot path, so internal RAM is recommended |

The defaults above apply when PSRAM is enabled (`CONFIG_SPIRAM=y`). Without PSRAM, the PSRAM-related options are hidden and every choice defaults to `Internal only`.

## Performance

Decoding 48 kHz stereo Ogg Vorbis (~128 kbit/s VBR), single stream:

| Chip | Clock | Working buffers | Wall-clock (30s audio) | Per-task RTF | Throughput |
| ---- | ----- | --------------- | ---------------------- | ------------ | ---------- |
| ESP32 | 240 MHz | Internal RAM | 6.3s | 0.21 | 4.8x real-time |
| ESP32-S3 | 240 MHz | PSRAM | 3.2s | 0.11 | 9.5x real-time |
| ESP32-P4 | 360 MHz | PSRAM | 1.9s | 0.062 | 16.2x real-time |

On the original ESP32, PSRAM access is much slower than internal SRAM, so the benchmark places decoder state and demuxer buffers in internal RAM (`CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL=y`, `CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_INTERNAL=y`); the ESP32-S3 and ESP32-P4 leave them in PSRAM (the default) with negligible penalty.

The dual-core ESP32-S3 also scales across concurrent streams, each decoder instance on its own task and core:

| Concurrent tasks | Wall-clock (30s audio) | Per-task RTF | Throughput |
| ---------------- | ---------------------- | ------------ | ---------- |
| 1 | 3.2s | 0.11 | 9.5x real-time |
| 2 (one per core) | 5.4s | 0.18 | 5.6x real-time each |
| 3 | 9.5s | 0.32 | 3.2x real-time |
| 4 | 11.3s | 0.37 | 2.7x real-time |

Each stream decodes on a single thread. Running a second stream on the other core raises combined throughput to about 11x real-time, but the two cores share one PSRAM bus, so each task slows down under contention: two concurrent streams take 5.4s against 3.2s for one. See [examples/decode_benchmark/README.md](examples/decode_benchmark/README.md) for per-frame timing statistics, the test clip details, and instructions for running your own benchmark.

## Memory Usage

Heap use depends heavily on the encoded file: sample rate, channel count, and the codebooks the encoder chose all move it. As a reference point, the 48kHz stereo ~128kbit/s clip in the [benchmark example](examples/decode_benchmark/README.md) draws about 76KB total (roughly 20KB internal RAM and 56KB PSRAM) for decoder state, codebook tables, demuxer buffers, and the output buffer combined. Smaller streams use less. Use PSRAM when internal RAM is insufficient.

| Allocation | Size | Notes |
| ---------- | ---- | ----- |
| `Decoder state` | ~10-40KB | Tremor state: PCM history and decode/setup arenas. ~10KB mono/low-rate, ~40KB 48kHz stereo, more for multichannel; scales with block size and channels (PCM history dominates) |
| `Demuxer buffers` | ~5-10KB typical | Ogg packet assembly (zero-copy when possible); starts at 1KB, grows on demand to a 128KB cap |
| `Codebook tables` | ~5-20KB typical | Compact Huffman decode trees and quantized value arrays; scales with codebook complexity. Placed separately via `MICRO_VORBIS_CODEBOOK_MEMORY_PREFERENCE` |
| `Output buffer` | User-provided | Typically 2-8KB for stereo |

## Advanced Features

### Raw Channel Selection

The default constructor smart-downmixes surround streams to mono or stereo per ITU-R BS.775 (center/surround at -3 dB relative to the fronts, LFE dropped, normalized against clipping). To instead route specific source channels to the output unmixed at unity gain, construct the decoder with an array of `SpeakerRole` values, one per output channel:

```cpp
// Extract the unmixed front pair from a surround stream
micro_vorbis::SpeakerRole roles[] = {micro_vorbis::SpeakerRole::FL,
                                     micro_vorbis::SpeakerRole::FR};
micro_vorbis::OggVorbisDecoder decoder(roles, 2);
```

Output channel `i` is fed by the source channel matching `roles[i]`, resolved against the file's Vorbis channel layout; e.g., quad has no center; 7.1 uses the side channels `SL`/`SR`. A role absent from the file's layout emits silence, except that mono sources route all three front roles (`FL`, `FR`, `FC`) to their single channel. The selected count becomes the channel count reported by `get_pcm_format().num_channels()`. Available roles: `FL`, `FR`, `FC`, `LFE`, `RL`, `RR`, `SL`, `SR`, `BC`.

This is useful for sound systems where each device plays one channel of a multichannel stream, or for skipping the downmix when feeding an external mixer.

## Testing

The wrapper is covered by a ctest suite (`tests/`) that decodes oggenc-generated fixtures and checks output against self-consistent references: chunked streaming at adversarial chunk sizes, buffer-too-small recovery, reset, smart-downmix, raw channel selection, error codes, and CRC validation.

```bash
cd tests
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Fixtures are checked into `tests/data/`; regenerate them with `tests/generate_test_data.sh` (needs ffmpeg and oggenc). The host decoder can also be run directly under AddressSanitizer and UBSan:

```bash
cd host_examples/vorbis_to_wav
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
./build/vorbis_to_wav input.ogg output.wav
```

Add `-DENABLE_WERROR=ON` to either cmake command to treat warnings as errors (off by default).

## Known Limitations

- **No seeking support**: Decoder is designed for streaming, not random access
- **No metadata retrieval**: Vorbis comment tags (title, artist, and similar) are parsed and discarded, not exposed through the API

## License

- **microVorbis wrapper code** (examples, OggVorbisDecoder, host tools): [Apache License 2.0](LICENSE)
- **Tremor fork** (in `src/tremor/`): BSD-3-Clause license; see [src/tremor/COPYING](src/tremor/COPYING). Modifications relative to upstream are summarized in [src/tremor/CHANGES.md](src/tremor/CHANGES.md).
- See [NOTICE](NOTICE) for third-party attributions and downstream redistribution obligations.

## Links

- [Tremor](https://gitlab.xiph.org/xiph/tremor): Fixed-point Vorbis decoder (libvorbisidec), forked into `src/tremor/` with arena allocator and Xtensa optimizations
- [microOggDemuxer](https://github.com/esphome-libs/micro-ogg-demuxer): Zero-copy Ogg page parser
- [Vorbis Official Site](https://xiph.org/vorbis/)
- [Tremor Documentation](https://wiki.xiph.org/Tremor)
- [Ogg Container Format](https://xiph.org/ogg/)
