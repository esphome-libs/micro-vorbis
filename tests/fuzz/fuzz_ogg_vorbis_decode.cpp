// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Fuzz harness for micro_vorbis::OggVorbisDecoder.
//
// Feeds raw Ogg Vorbis container bytes through the streaming decoder in
// variably-sized chunks, so libFuzzer's coverage feedback explores both
// chunk-boundary bugs (Ogg page spanning, header staging) and the Tremor
// bitstream decoder.
//
// A configuration byte (and, in role-select mode, a count + per-channel roles)
// is consumed from the TAIL of the input so the front stays an intact Ogg
// payload. This steers which OggVorbisDecoder constructor and options are
// exercised -- downmix targets, CRC validation, and raw channel selection --
// across runs, while a small set of Tier 1 structural invariants are asserted
// on every decode (see the oracle block below).
//
// Two build modes:
//   1. libFuzzer:  compile with -fsanitize=fuzzer,address,undefined, which
//      exposes LLVMFuzzerTestOneInput. Use with a corpus directory:
//          ./fuzz_ogg_vorbis_decode corpus_ogg/
//   2. Standalone: compile with FUZZ_STANDALONE defined. Takes file paths on
//      argv for crash reproduction, or with no args runs a torture battery.

#include "micro_vorbis/ogg_vorbis_decoder.h"
#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

using micro_vorbis::OggVorbisDecoder;
using micro_vorbis::OggVorbisResult;

// 64 KiB headroom for any plausible Vorbis frame (block size up to 8192
// samples * 2 channels * 2 bytes = 32 KiB; rounded up). If the decoder
// reports ERROR_OUTPUT_BUFFER_TOO_SMALL we grow on demand.
static constexpr size_t INITIAL_PCM_BYTES = 64UL * 1024;
static constexpr size_t MAX_PCM_BYTES = 1UL * 1024 * 1024;

// Upper bound on the chunk-size control bytes pulled from the TAIL (via
// FuzzedDataProvider, alongside the config/role bytes). Iterating over them
// drives the streaming chunk sizes while the front payload stays intact for
// the codec, so libFuzzer mutates stream bytes handed to the decoder rather
// than bytes the harness eats.
static constexpr size_t MAX_CONTROL_BYTES = 64;

// One streaming pass: feed `payload` to `decoder` in control-byte-sized chunks,
// asserting the Tier 1 oracle on every decode. Factored out so it can run twice
// across a reset() to exercise the re-stream path (arena free+realloc,
// vorbis_synthesis_restart, second STREAM_INFO_READY) that a single pass misses.
// `pcm` grows on demand and is reused across passes.
static void run_decode_pass(OggVorbisDecoder& decoder, const std::vector<uint8_t>& payload,
                            const std::vector<uint8_t>& ctrl, int expected_channels,
                            std::vector<uint8_t>& pcm) {
    size_t offset = 0;
    size_t total_decoded_bytes = 0;
    int iterations = 0;
    size_t ctrl_idx = 0;

    constexpr int MAX_ITERATIONS = 4096;
    constexpr size_t MAX_DECODED_BYTES = (1U << 20) * sizeof(int16_t);

    while (offset < payload.size() && iterations < MAX_ITERATIONS &&
           total_decoded_bytes < MAX_DECODED_BYTES) {
        // Derive a chunk size from the next control byte: 1..8161. Spans
        // values smaller than an Ogg page header (27 bytes) and larger than a
        // typical page (4 KiB) so the boundary paths get exercised. Control
        // bytes cycle, so a short ctrl buffer still yields varied chunk sizes.
        uint8_t ctrl_byte = ctrl[ctrl_idx++ % ctrl.size()];
        size_t chunk_size = 1 + (static_cast<size_t>(ctrl_byte) * 32);
        if (chunk_size > payload.size() - offset) {
            chunk_size = payload.size() - offset;
        }
        if (chunk_size == 0) {
            chunk_size = 1;
        }

        size_t consumed = 0;
        size_t bytes_written = 0;
        OggVorbisResult result = decoder.decode(payload.data() + offset, chunk_size, pcm.data(),
                                                pcm.size(), consumed, bytes_written);

        // ---- Tier 1 oracle: structural invariants on a single decode ----------
        const auto& info = decoder.get_pcm_format();
        if (info.is_valid()) {
            const uint32_t n = info.num_channels();
            // Channel count must be positive once the identification header parsed,
            // and must equal the configured target whenever that target is fixed.
            if (n == 0) {
                std::abort();
            }
            if (expected_channels >= 0 && n != static_cast<uint32_t>(expected_channels)) {
                std::abort();
            }
            // Output is interleaved across all channels, so each decoded frame
            // contributes n samples of bytes_per_sample bytes => the total byte count
            // is always a multiple of n * bytes_per_sample.
            if (bytes_written > 0 && (bytes_written % (n * info.bytes_per_sample())) != 0) {
                std::abort();
            }
            // A single decode() must never write more than max_output_bytes(), the
            // per-call upper bound callers use to size the output buffer.
            if (bytes_written > info.max_output_bytes()) {
                std::abort();
            }
        }
        // The decoder must never report more bytes than the output buffer holds.
        if (bytes_written > pcm.size()) {
            std::abort();
        }

        if (result == micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL) {
            size_t needed = decoder.get_required_output_bytes();
            if (needed == 0 || needed > MAX_PCM_BYTES) {
                break;
            }
            pcm.resize(needed);
            // Don't advance offset; retry with the larger buffer.
            continue;
        }

        // Forward-progress guarantee: if the decoder didn't consume anything
        // and didn't ask for a bigger buffer, bail out instead of spinning.
        if (consumed == 0) {
            break;
        }
        if (consumed > chunk_size) {
            // Decoder claimed to consume more than we handed it, a contract
            // violation we want the fuzzer to flag.
            std::abort();
        }

        offset += consumed;
        total_decoded_bytes += bytes_written;
        iterations++;

        if (result < 0) {
            break;
        }
    }
}

// NOLINTNEXTLINE(readability-identifier-naming): fixed libFuzzer entry point name
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzedDataProvider fdp(data, size);

    // Decoder configuration, consumed from the TAIL of the input. FuzzedDataProvider
    // integral reads come off the back of the buffer, so the payload PREFIX is preserved:
    // only the trailing config/control bytes are peeled off, leaving the front of a real
    // .ogg seed (or the OSS-Fuzz corpus) intact. Up to ~74 bytes are peeled (1 cfg byte;
    // in role-select mode 1 count plus up to 8 role bytes; up to 64 chunk-control bytes),
    // so an externally-supplied .ogg loses that much off its last page. Seeds from
    // generate_seeds.sh append a config tail to avoid this, keeping the whole stream
    // intact. One config byte picks the constructor variant, CRC setting, and whether
    // to replay the stream across a reset(); role-select mode pulls its count and
    // per-channel roles from the tail too. An exhausted provider reads 0, i.e. the
    // historical default: follow-the-file channels, CRC off, single pass.
    const uint8_t cfg = fdp.ConsumeIntegral<uint8_t>();
    const bool enable_crc = (cfg & 0x01) != 0;
    const uint8_t mode = (cfg >> 1) & 0x03;  // 0 passthrough, 1 mono, 2 stereo, 3 role-select
    // Bit 3: replay the whole stream a second time across a reset(), exercising the
    // re-stream path (arena free+realloc, vorbis_synthesis_restart, second
    // STREAM_INFO_READY). Gated on a bit so non-replay inputs keep full throughput.
    const bool replay = (cfg & 0x08) != 0;

    // Role-select parameters. sel_count is passed VERBATIM (0..255): values outside
    // 1..8 deliberately exercise the constructor's invalid-count fallback (which
    // resolves to the file's own channel count).
    uint8_t sel_count = 0;
    micro_vorbis::SpeakerRole roles[8] = {};
    if (mode == 3) {
        sel_count = fdp.ConsumeIntegral<uint8_t>();
        const uint8_t fill = sel_count <= 8 ? sel_count : 8;
        for (uint8_t i = 0; i < fill; i++) {
            // 9 loudspeaker roles (FL..BC); modulo keeps every byte a valid enumerator.
            roles[i] = static_cast<micro_vorbis::SpeakerRole>(fdp.ConsumeIntegral<uint8_t>() % 9);
        }
    }

    // The output channel count the harness can predict, for the Tier 1 oracle below.
    // -1 means non-determinate: passthrough and every fallback resolve to the file's
    // own channel count, which the harness does not know up front.
    int expected_channels = -1;
    if (mode == 1 || mode == 2) {
        expected_channels = mode;  // explicit mono/stereo downmix target
    } else if (mode == 3 && sel_count >= 1 && sel_count <= 8) {
        expected_channels = sel_count;  // valid raw selection => exactly this many output channels
    }

    // Reserve ~1/8 of the input (capped) for chunk-size control. If the input
    // is tiny, fall back to a single neutral control byte so the decoder still
    // sees the full payload.
    size_t ctrl_len = std::min(MAX_CONTROL_BYTES, fdp.remaining_bytes() / 8);
    std::vector<uint8_t> ctrl;
    ctrl.reserve(ctrl_len + 1);
    for (size_t i = 0; i < ctrl_len; i++) {
        ctrl.push_back(fdp.ConsumeIntegral<uint8_t>());
    }
    if (ctrl.empty()) {
        ctrl.push_back(0x20);  // neutral default; ~1 KiB chunks
    }

    std::vector<uint8_t> payload = fdp.ConsumeRemainingBytes<uint8_t>();
    if (payload.empty()) {
        return 0;
    }

    // OggVorbisDecoder is neither copyable nor movable, so build it in place via
    // optional::emplace rather than assigning a constructed temporary.
    std::optional<OggVorbisDecoder> dec;
    if (mode == 3) {
        dec.emplace(roles, sel_count, enable_crc);
    } else {
        dec.emplace(/*channels=*/mode, enable_crc);  // mode 0/1/2 maps to the channel target
    }
    OggVorbisDecoder& decoder = *dec;
    std::vector<uint8_t> pcm(INITIAL_PCM_BYTES);

    run_decode_pass(decoder, payload, ctrl, expected_channels, pcm);

    if (replay) {
        // Replay the same payload across a reset() to drive the re-stream path.
        decoder.reset();
        run_decode_pass(decoder, payload, ctrl, expected_channels, pcm);
    }

    decoder.reset();
    return 0;
}

#ifdef FUZZ_STANDALONE

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}

uint32_t lcg_next(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

std::vector<uint8_t> build_random_blob(uint32_t seed, size_t len) {
    std::vector<uint8_t> buf(len);
    uint32_t state = seed;
    for (size_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>(lcg_next(state) >> 24);
    }
    // Sprinkle OggS capture patterns so the demuxer engages on random data.
    for (size_t i = 0; i + 27 < buf.size(); i += 200 + (seed % 400)) {
        buf[i + 0] = 'O';
        buf[i + 1] = 'g';
        buf[i + 2] = 'g';
        buf[i + 3] = 'S';
        buf[i + 4] = 0;  // stream_structure_version
        // Header type flag (random), granule, serial, seq, crc, segs...
    }
    return buf;
}

void mutate_in_place(std::vector<uint8_t>& buf, uint32_t& rng_state) {
    if (buf.empty())
        return;
    int n = 1 + static_cast<int>((lcg_next(rng_state) >> 24) & 0x07);
    for (int i = 0; i < n; i++) {
        uint32_t r = lcg_next(rng_state);
        size_t pos = r % buf.size();
        uint32_t kind = (r >> 24) & 0x07;
        switch (kind) {
            case 0:
            case 1:
                buf[pos] ^= static_cast<uint8_t>(1u << ((r >> 8) & 0x07));
                break;
            case 2:
            case 3:
                buf[pos] = static_cast<uint8_t>(r >> 16);
                break;
            case 4: {
                static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80,
                                                      0xFF, 0xFE, 0x55, 0xAA};
                buf[pos] = interesting[(r >> 16) & 0x07];
                break;
            }
            case 5: {
                size_t run = 1 + ((r >> 16) & 0x0F);
                for (size_t k = 0; k < run && pos + k < buf.size(); k++) {
                    buf[pos + k] = 0;
                }
                break;
            }
            case 6:
                buf[pos] = static_cast<uint8_t>(buf[pos] + 1);
                break;
            default:
                buf[pos] = static_cast<uint8_t>(buf[pos] - 1);
                break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Mutation mode: "./fuzz_ogg_vorbis_decode -mutate <seedfile>"
    if (argc >= 3 && std::strcmp(argv[1], "-mutate") == 0) {
        std::vector<uint8_t> seed = read_file(argv[2]);
        if (seed.empty()) {
            std::fprintf(stderr, "[fuzz] seed file %s is empty or missing\n", argv[2]);
            return 1;
        }
        const char* iter_env = std::getenv("FUZZ_ITERATIONS");
        const int iters = iter_env ? std::atoi(iter_env) : 2000;
        std::printf("[fuzz] mutation mode: seed=%s (%zu bytes), %d iterations\n", argv[2],
                    seed.size(), iters);

        uint32_t rng_state = 0xC0FFEEu;
        std::vector<uint8_t> scratch;
        scratch.reserve(seed.size());

        LLVMFuzzerTestOneInput(seed.data(), seed.size());

        for (int i = 0; i < iters; i++) {
            scratch = seed;
            mutate_in_place(scratch, rng_state);
            LLVMFuzzerTestOneInput(scratch.data(), scratch.size());
            if ((i + 1) % 200 == 0) {
                std::printf("[fuzz] %d/%d mutated iterations ok\n", i + 1, iters);
            }
        }
        std::printf("[fuzz] mutation fuzzing complete, no sanitizer failures\n");
        return 0;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            std::vector<uint8_t> data = read_file(argv[i]);
            std::printf("[fuzz] %s (%zu bytes)\n", argv[i], data.size());
            LLVMFuzzerTestOneInput(data.data(), data.size());
        }
        std::printf("[fuzz] %d file(s) processed cleanly\n", argc - 1);
        return 0;
    }

    std::printf("[fuzz] standalone torture mode\n");

    // Empty / tiny inputs.
    {
        const uint8_t nothing[1] = {0};
        LLVMFuzzerTestOneInput(nothing, 0);
        LLVMFuzzerTestOneInput(nothing, 1);
    }

    // Bare OggS capture with nothing after.
    {
        const uint8_t trunc[5] = {'O', 'g', 'g', 'S', 0};
        LLVMFuzzerTestOneInput(trunc, sizeof(trunc));
    }

    // Claimed-huge segment count.
    {
        uint8_t hdr[28] = {'O', 'g', 'g', 'S', 0, 0x02};
        hdr[26] = 0xFF;  // page_segments = 255
        LLVMFuzzerTestOneInput(hdr, sizeof(hdr));
    }

    // Random blobs with sprinkled OggS headers.
    const char* iter_env = std::getenv("FUZZ_ITERATIONS");
    const int kIterations = iter_env ? std::atoi(iter_env) : 100;
    for (int i = 0; i < kIterations; i++) {
        size_t len = 512 + (static_cast<size_t>(i) * 37) % (32 * 1024);
        std::vector<uint8_t> blob = build_random_blob(static_cast<uint32_t>(i) * 2654435761u, len);
        LLVMFuzzerTestOneInput(blob.data(), blob.size());
        if ((i + 1) % 200 == 0) {
            std::printf("[fuzz] %d/%d random iterations ok\n", i + 1, kIterations);
        }
    }

    std::printf("[fuzz] standalone torture complete, no sanitizer failures\n");
    return 0;
}

#endif  // FUZZ_STANDALONE
