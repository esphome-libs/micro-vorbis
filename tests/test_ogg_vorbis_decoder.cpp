// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* Unit tests for the OggVorbisDecoder wrapper.
 *
 * These tests target the wrapper's own logic (state machine, streaming
 * buffering, output-buffer recovery, reset, downmix and channel selection),
 * not the Tremor decode math, which is validated separately against ffmpeg.
 * Most tests are self-consistent: the all-at-once, full-buffer, native-channel
 * decode of a fixture serves as the reference the other configurations must
 * match, so intentional decoder changes don't invalidate golden data.
 *
 * Usage: test_ogg_vorbis_decoder <data_dir> [test_name]
 */

#include "micro_vorbis/ogg_vorbis_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using micro_vorbis::OggVorbisDecoder;
using micro_vorbis::OggVorbisResult;
using micro_vorbis::SpeakerRole;

static std::string g_data_dir;

// Abort the current test on the first failed condition, reporting the line.
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            return false;                                                     \
        }                                                                     \
    } while (0)

// Like CHECK(a == b) but reports both operands' values on failure. Use for
// integer comparisons where seeing the actual vs expected number is useful.
#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        const long long _va = static_cast<long long>(a);                \
        const long long _vb = static_cast<long long>(b);                \
        if (_va != _vb) {                                               \
            std::printf("    CHECK_EQ failed: %s == %s (%lld vs %lld) " \
                        "(line %d)\n",                                  \
                        #a, #b, _va, _vb, __LINE__);                    \
            return false;                                               \
        }                                                               \
    } while (0)

static std::vector<uint8_t> read_file(const std::string& name) {
    std::ifstream f(g_data_dir + "/" + name, std::ios::binary);
    if (!f) {
        std::printf("    cannot open fixture: %s\n", name.c_str());
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// ============================================================================
// Decode harness
// ============================================================================

struct DecodeOutput {
    std::vector<int16_t> pcm;  // Interleaved samples across all output channels
    uint32_t sample_rate{0};
    uint32_t channels{0};
    bool header_ready{false};
    bool eos{false};
    bool errored{false};
    OggVorbisResult error{micro_vorbis::OGG_VORBIS_DECODER_SUCCESS};
    size_t success_events{0};
    size_t too_small_events{0};
};

// Drive a decoder over `data`, offering at most `chunk` input bytes per call
// (simulating streamed input; the window grows if the decoder makes no
// progress, as a real ring buffer would accumulate). If `tiny_buf_bytes` is
// nonzero, every fresh packet is first offered that many output bytes, and on
// ERROR_OUTPUT_BUFFER_TOO_SMALL the call is retried with exactly the reported
// required size - exercising the pending-PCM recovery path on every frame.
static DecodeOutput decode_stream(OggVorbisDecoder& dec, const uint8_t* data, size_t len,
                                  size_t chunk = SIZE_MAX, size_t tiny_buf_bytes = 0) {
    DecodeOutput out;
    std::vector<int16_t> buf;  // Full-size output buffer (sized after STREAM_INFO_READY)
    size_t off = 0;
    size_t window = chunk;
    bool retry_with_required = false;
    size_t required_bytes = 0;

    // Generous safety bound: chunk=1 feeding needs at least one call per byte.
    size_t max_calls = 64 * (len + 1024);
    for (size_t calls = 0; calls < max_calls; calls++) {
        size_t avail = std::min(window, len - off);
        uint8_t* out_ptr = reinterpret_cast<uint8_t*>(buf.data());
        size_t out_size = buf.size() * sizeof(int16_t);
        if (retry_with_required) {
            out_size = required_bytes;  // Exact byte count reported by the decoder
        } else if (tiny_buf_bytes != 0 && !buf.empty()) {
            out_size = tiny_buf_bytes;
        }

        size_t consumed = 0;
        size_t bytes_written = 0;
        OggVorbisResult result =
            dec.decode(data + off, avail, out_ptr, out_size, consumed, bytes_written);
        off += consumed;
        retry_with_required = false;

        if (result == micro_vorbis::OGG_VORBIS_DECODER_STREAM_INFO_READY) {
            out.header_ready = true;
            const auto& info = dec.get_pcm_format();
            out.sample_rate = info.sample_rate();
            out.channels = info.num_channels();
            buf.resize(info.max_output_bytes() / sizeof(int16_t));
            continue;
        }

        if (result == micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL) {
            out.too_small_events++;
            required_bytes = dec.get_required_output_bytes();
            if (required_bytes == 0 || required_bytes > buf.size() * sizeof(int16_t)) {
                // the max_output_bytes() buffer promised this can't happen
                out.errored = true;
                out.error = result;
                return out;
            }
            retry_with_required = true;
            continue;
        }

        if (result < 0) {
            out.errored = true;
            out.error = result;
            return out;
        }

        if (result == micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM) {
            out.eos = true;
            return out;
        }

        if (bytes_written > 0) {
            out.success_events++;
            const size_t elems = bytes_written / sizeof(int16_t);
            out.pcm.insert(out.pcm.end(), buf.data(), buf.data() + elems);
        }

        if (consumed == 0 && bytes_written == 0) {
            if (off >= len) {
                return out;  // Input exhausted without an EOS page (truncated stream)
            }
            // No progress on a partial window: offer more input next call.
            window += chunk;
        } else {
            window = chunk;
        }
    }
    out.errored = true;  // Loop bound exhausted: decoder made no progress
    return out;
}

static DecodeOutput decode_file(OggVorbisDecoder& dec, const std::string& name,
                                size_t chunk = SIZE_MAX, size_t tiny_buf_bytes = 0) {
    std::vector<uint8_t> data = read_file(name);
    if (data.empty()) {
        DecodeOutput out;
        out.errored = true;
        return out;
    }
    return decode_stream(dec, data.data(), data.size(), chunk, tiny_buf_bytes);
}

// Reference decode: whole file at once, full buffer, file's native channels.
static DecodeOutput reference_decode(const std::string& name) {
    OggVorbisDecoder dec;
    return decode_file(dec, name);
}

static bool outputs_equal(const DecodeOutput& a, const DecodeOutput& b) {
    return a.sample_rate == b.sample_rate && a.channels == b.channels && a.eos == b.eos &&
           a.pcm == b.pcm;
}

// ============================================================================
// Tests
// ============================================================================

static bool test_pcm_format_contract() {
    OggVorbisDecoder dec;
    const auto& info = dec.get_pcm_format();
    CHECK(!info.is_valid());
    CHECK_EQ(info.sample_rate(), 0);
    CHECK_EQ(info.num_channels(), 0);
    CHECK_EQ(info.max_output_bytes(), 0);
    CHECK_EQ(dec.get_required_output_bytes(), 0);

    DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
    CHECK(!out.errored);
    CHECK(out.header_ready);
    CHECK(out.eos);
    CHECK_EQ(out.sample_rate, 44100);
    CHECK_EQ(out.channels, 2);
    // A packet outputs at most half a long block per channel; the long block is a power of two
    // in the Vorbis I legal range [64, 8192], so frames/channel is a power of two in [32, 4096].
    const uint32_t max_out = info.max_output_bytes();
    CHECK_EQ(max_out % (out.channels * info.bytes_per_sample()), 0);
    const uint32_t frames_per_channel = max_out / (out.channels * info.bytes_per_sample());
    CHECK(frames_per_channel >= 32);
    CHECK(frames_per_channel <= 4096);
    CHECK_EQ((frames_per_channel & (frames_per_channel - 1)), 0);
    CHECK(info.is_valid());
    CHECK_EQ(info.bits_per_sample(), 16);
    CHECK_EQ(info.bytes_per_sample(), 2);
    // ~2 seconds of stereo audio at 44.1 kHz, allow codec delay slack
    CHECK(out.pcm.size() > static_cast<size_t>(2 * 44100));
    return true;
}

static bool test_chunked_invariance() {
    struct Case {
        const char* file;
        size_t chunks[3];
    };
    // chunk=1 only on the smallest file to keep sanitizer runtime reasonable
    const Case cases[] = {
        {"stereo_8000.ogg", {1, 17, 4096}},
        {"stereo_44100.ogg", {17, 509, 4096}},
        {"mono_44100.ogg", {17, 4096, 0}},
        {"surround51_48000.ogg", {17, 4096, 0}},
    };
    for (const Case& c : cases) {
        DecodeOutput ref = reference_decode(c.file);
        CHECK(!ref.errored);
        CHECK(ref.eos);
        CHECK_EQ(ref.too_small_events, 0);  // Full-size buffer must always suffice
        for (size_t chunk : c.chunks) {
            if (chunk == 0) {
                continue;
            }
            OggVorbisDecoder dec;
            DecodeOutput out = decode_file(dec, c.file, chunk);
            if (out.errored || !outputs_equal(ref, out)) {
                std::printf("    mismatch: %s chunk=%zu (errored=%d, ref %zu vs %zu samples)\n",
                            c.file, chunk, out.errored ? 1 : 0, ref.pcm.size(), out.pcm.size());
                return false;
            }
        }
    }
    return true;
}

static bool test_small_output_buffer_recovery() {
    DecodeOutput ref = reference_decode("stereo_44100.ogg");
    CHECK(!ref.errored);

    // Offer every packet a 64-byte buffer first: smaller than any Vorbis block
    // (min 64 frames/channel), so every frame must take the TOO_SMALL ->
    // retry-with-required path, including the final frame on the EOS packet.
    {
        OggVorbisDecoder dec;
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg", SIZE_MAX, 64);
        CHECK(!out.errored);
        CHECK(out.eos);
        CHECK(out.too_small_events > 0);
        CHECK_EQ(out.too_small_events, out.success_events);
        CHECK(outputs_equal(ref, out));
    }

    // Same again with small input chunks: tiny-buffer recovery interleaved
    // with incremental input must still reproduce the reference exactly.
    {
        OggVorbisDecoder dec;
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg", 17, 64);
        CHECK(!out.errored);
        CHECK(out.eos);
        CHECK_EQ(out.too_small_events, out.success_events);
        CHECK(outputs_equal(ref, out));
    }
    return true;
}

static bool test_end_of_stream_sticky() {
    std::vector<uint8_t> data = read_file("stereo_8000.ogg");
    CHECK(!data.empty());

    OggVorbisDecoder dec;
    DecodeOutput out = decode_stream(dec, data.data(), data.size());
    CHECK(out.eos);

    // Any further call reports END_OF_STREAM and touches nothing
    std::vector<int16_t> buf(4096);
    for (int i = 0; i < 2; i++) {
        size_t consumed = 1;
        size_t bytes_written = 1;
        OggVorbisResult r =
            dec.decode(data.data(), data.size(), reinterpret_cast<uint8_t*>(buf.data()),
                       buf.size() * sizeof(int16_t), consumed, bytes_written);
        CHECK_EQ(r, micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM);
        CHECK_EQ(consumed, 0);
        CHECK_EQ(bytes_written, 0);
    }
    return true;
}

static bool test_reset_reuse() {
    // Decode stream A, reset, decode stream B with different parameters; the
    // result must match a fresh decoder bit for bit.
    DecodeOutput ref_a = reference_decode("stereo_44100.ogg");
    DecodeOutput ref_b = reference_decode("stereo_8000.ogg");
    DecodeOutput ref_c = reference_decode("mono_44100.ogg");
    CHECK(!ref_a.errored);
    CHECK(!ref_b.errored);
    CHECK(!ref_c.errored);

    OggVorbisDecoder dec;
    DecodeOutput a = decode_file(dec, "stereo_44100.ogg");
    CHECK(outputs_equal(ref_a, a));

    dec.reset();
    CHECK(!dec.get_pcm_format().is_valid());
    CHECK_EQ(dec.get_pcm_format().sample_rate(), 0);
    CHECK_EQ(dec.get_required_output_bytes(), 0);

    DecodeOutput b = decode_file(dec, "stereo_8000.ogg");
    CHECK(outputs_equal(ref_b, b));

    dec.reset();
    DecodeOutput c = decode_file(dec, "mono_44100.ogg");
    CHECK(outputs_equal(ref_c, c));

    // Configuration survives reset: a mono-downmixing decoder still downmixes.
    OggVorbisDecoder mono_dec(1);
    DecodeOutput m1 = decode_file(mono_dec, "stereo_44100.ogg");
    mono_dec.reset();
    DecodeOutput m2 = decode_file(mono_dec, "stereo_44100.ogg");
    CHECK(!m1.errored);
    CHECK_EQ(m1.channels, 1);
    CHECK(outputs_equal(m1, m2));
    return true;
}

static bool test_downmix_mono_from_stereo() {
    DecodeOutput ref = reference_decode("stereo_44100.ogg");
    CHECK(!ref.errored);

    OggVorbisDecoder dec(1);
    DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
    CHECK(!out.errored);
    CHECK(out.eos);
    CHECK_EQ(out.channels, 1);
    CHECK_EQ(out.sample_rate, ref.sample_rate);
    CHECK_EQ(out.pcm.size(), ref.pcm.size() / 2);

    // A stereo source takes the one-stage unity path in downmix_stereo() (no
    // BS.775 normalization), so mono is (L + R + 1) >> 1 of truncating-scaled
    // full-precision samples; vs the rounded native path the results may
    // differ by at most 1 LSB.
    for (size_t i = 0; i < out.pcm.size(); i++) {
        int expected = (ref.pcm[2 * i] + ref.pcm[2 * i + 1] + 1) >> 1;
        if (std::abs(out.pcm[i] - expected) > 1) {
            std::printf("    frame %zu: mono=%d expected~%d (L=%d R=%d)\n", i, out.pcm[i], expected,
                        ref.pcm[2 * i], ref.pcm[2 * i + 1]);
            return false;
        }
    }
    return true;
}

static bool test_upmix_stereo_from_mono() {
    DecodeOutput ref = reference_decode("mono_44100.ogg");
    CHECK(!ref.errored);

    OggVorbisDecoder dec(2);
    DecodeOutput out = decode_file(dec, "mono_44100.ogg");
    CHECK(!out.errored);
    CHECK_EQ(out.channels, 2);
    CHECK_EQ(out.pcm.size(), ref.pcm.size() * 2);

    for (size_t i = 0; i < ref.pcm.size(); i++) {
        // A mono source must be duplicated identically to both channels
        CHECK_EQ(out.pcm[2 * i], out.pcm[2 * i + 1]);
        // Mono sources use the one-stage unity path (truncating) vs the
        // rounded native path: at most 1 LSB apart
        if (std::abs(out.pcm[2 * i] - ref.pcm[i]) > 1) {
            std::printf("    frame %zu: L=%d native=%d\n", i, out.pcm[2 * i], ref.pcm[i]);
            return false;
        }
    }
    return true;
}

static bool test_downmix_stereo_from_multichannel() {
    // Per-source-layout fold weights in the Vorbis I channel order (spec
    // section 4.3.9), restated independently from downmix_stereo(): fronts at
    // unity, center/surrounds at -3 dB, LFE dropped, and the whole fold scaled
    // by the ITU-R BS.775 normalization gain 1/(row weight sum) so a full-scale
    // mix cannot clip. Error budget vs the decoder: the rounded int16 native
    // planes are each up to 0.5 LSB off, and the gain scales their weighted sum
    // back to exactly 0.5; the decoder's Q19 term truncations add < 0.1 (at most
    // 4 terms x 1/16 LSB, shrunk further by the gain) and its final Q15
    // truncation < 1, so a tolerance of 2 covers every layout.
    constexpr double M = 0.7071067811865476;
    constexpr int TOLERANCE = 2;
    // Fields ordered pointer, 8-byte-aligned arrays, then the 4-byte int to
    // avoid struct padding (the channel count for each row is also noted in the
    // comment above it).
    struct FoldCase {
        const char* file;
        double wl[8];  // Left-output weight per source plane
        double wr[8];  // Right-output weight per source plane
        uint32_t channels;
    };
    const FoldCase cases[] = {
        // 3: L C R
        {"surround30_44100.ogg", {1, M, 0}, {0, M, 1}, 3},
        // 4: FL FR RL RR
        {"quad_44100.ogg", {1, 0, M, 0}, {0, 1, 0, M}, 4},
        // 5: FL C FR RL RR
        {"surround50_44100.ogg", {1, M, 0, M, 0}, {0, M, 1, 0, M}, 5},
        // 6: FL C FR RL RR LFE (LFE dropped)
        {"surround51_48000.ogg", {1, M, 0, M, 0, 0}, {0, M, 1, 0, M, 0}, 6},
        // 7: FL C FR SL SR RC LFE (RC to both sides; LFE dropped)
        {"surround61_48000.ogg", {1, M, 0, M, 0, M, 0}, {0, M, 1, 0, M, M, 0}, 7},
        // 8: FL C FR SL SR RL RR LFE (LFE dropped)
        {"surround71_48000.ogg", {1, M, 0, M, 0, M, 0, 0}, {0, M, 1, 0, M, 0, M, 0}, 8},
    };

    for (const FoldCase& c : cases) {
        DecodeOutput ref = reference_decode(c.file);
        CHECK(!ref.errored);
        CHECK_EQ(ref.channels, c.channels);

        OggVorbisDecoder dec(2);
        DecodeOutput out = decode_file(dec, c.file);
        CHECK(!out.errored);
        CHECK(out.eos);
        CHECK_EQ(out.channels, 2);
        size_t frames = ref.pcm.size() / c.channels;
        CHECK_EQ(out.pcm.size(), frames * 2);

        // Normalization gain from the weight row sum (symmetric across L/R).
        double row_sum = 0;
        for (uint32_t ch = 0; ch < c.channels; ch++) {
            row_sum += c.wl[ch];
        }
        const double gain = 1.0 / row_sum;

        for (size_t i = 0; i < frames; i++) {
            const int16_t* f = &ref.pcm[i * c.channels];
            double exp_l = 0;
            double exp_r = 0;
            for (uint32_t ch = 0; ch < c.channels; ch++) {
                exp_l += c.wl[ch] * f[ch];
                exp_r += c.wr[ch] * f[ch];
            }
            exp_l *= gain;
            exp_r *= gain;
            // The unclipped reconstruction is only valid while the fold stays
            // inside int16 range; the normalization gain guarantees that for
            // in-range planes, but keep the guard so a future weight change
            // fails here explicitly, not as a bogus mismatch against the
            // decoder's clip_to_16()'d output.
            CHECK(std::abs(exp_l) < INT16_MAX);
            CHECK(std::abs(exp_r) < INT16_MAX);
            if (std::abs(out.pcm[2 * i] - exp_l) > TOLERANCE ||
                std::abs(out.pcm[2 * i + 1] - exp_r) > TOLERANCE) {
                std::printf("    %s frame %zu: L=%d R=%d expected~(%.1f, %.1f)\n", c.file, i,
                            out.pcm[2 * i], out.pcm[2 * i + 1], exp_l, exp_r);
                return false;
            }
        }
    }
    return true;
}

static bool test_selection_planes_bit_exact() {
    DecodeOutput ref = reference_decode("surround51_48000.ogg");
    CHECK(!ref.errored);
    CHECK_EQ(ref.channels, 6);
    size_t frames = ref.pcm.size() / 6;

    // {FL, FR} from 5.1 selects planes 0 and 2 of the Vorbis order (FL C FR RL
    // RR LFE). Both paths use the same rounded copy, and the keep-mask passed
    // to Tremor must not perturb kept planes, so this is bit-exact.
    {
        const SpeakerRole roles[] = {SpeakerRole::FL, SpeakerRole::FR};
        OggVorbisDecoder dec(roles, 2);
        DecodeOutput out = decode_file(dec, "surround51_48000.ogg");
        CHECK(!out.errored);
        CHECK(out.eos);
        CHECK_EQ(out.channels, 2);
        CHECK_EQ(out.pcm.size(), frames * 2);
        for (size_t i = 0; i < frames; i++) {
            if (out.pcm[2 * i] != ref.pcm[6 * i + 0] || out.pcm[2 * i + 1] != ref.pcm[6 * i + 2]) {
                std::printf("    frame %zu: got (%d, %d) want (%d, %d)\n", i, out.pcm[2 * i],
                            out.pcm[2 * i + 1], ref.pcm[6 * i + 0], ref.pcm[6 * i + 2]);
                return false;
            }
        }
    }

    // Single-role selection drops five of six planes via the keep-mask; the
    // surviving center plane must still match the full decode exactly.
    {
        const SpeakerRole roles[] = {SpeakerRole::FC};
        OggVorbisDecoder dec(roles, 1);
        DecodeOutput out = decode_file(dec, "surround51_48000.ogg");
        CHECK(!out.errored);
        CHECK_EQ(out.channels, 1);
        CHECK_EQ(out.pcm.size(), frames);
        for (size_t i = 0; i < frames; i++) {
            if (out.pcm[i] != ref.pcm[6 * i + 1]) {
                std::printf("    frame %zu: got %d want %d\n", i, out.pcm[i], ref.pcm[6 * i + 1]);
                return false;
            }
        }
    }
    return true;
}

static bool test_selection_absent_role_silence() {
    DecodeOutput ref = reference_decode("stereo_44100.ogg");
    CHECK(!ref.errored);
    size_t frames = ref.pcm.size() / 2;

    // BC doesn't exist in a stereo layout: pure silence, but frame count and
    // stream progress must match a normal decode.
    const SpeakerRole roles[] = {SpeakerRole::FL, SpeakerRole::BC};
    OggVorbisDecoder dec(roles, 2);
    DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
    CHECK(!out.errored);
    CHECK(out.eos);
    CHECK_EQ(out.channels, 2);
    CHECK_EQ(out.pcm.size(), frames * 2);
    for (size_t i = 0; i < frames; i++) {
        CHECK_EQ(out.pcm[2 * i], ref.pcm[2 * i]);  // FL passes through bit-exact
        CHECK_EQ(out.pcm[2 * i + 1], 0);           // BC absent: silence
    }
    return true;
}

static bool test_selection_mono_routing() {
    DecodeOutput ref = reference_decode("mono_44100.ogg");
    CHECK(!ref.errored);

    // A mono source routes FL, FR, and FC to its single plane; LFE does not.
    const SpeakerRole roles[] = {SpeakerRole::FL, SpeakerRole::FR, SpeakerRole::FC,
                                 SpeakerRole::LFE};
    OggVorbisDecoder dec(roles, 4);
    DecodeOutput out = decode_file(dec, "mono_44100.ogg");
    CHECK(!out.errored);
    CHECK_EQ(out.channels, 4);
    CHECK_EQ(out.pcm.size(), ref.pcm.size() * 4);
    for (size_t i = 0; i < ref.pcm.size(); i++) {
        CHECK_EQ(out.pcm[4 * i], ref.pcm[i]);
        CHECK_EQ(out.pcm[4 * i + 1], ref.pcm[i]);
        CHECK_EQ(out.pcm[4 * i + 2], ref.pcm[i]);
        CHECK_EQ(out.pcm[4 * i + 3], 0);
    }
    return true;
}

static bool test_selection_invalid_fallback() {
    DecodeOutput ref = reference_decode("stereo_44100.ogg");
    CHECK(!ref.errored);

    // Null roles, zero count, and oversized count all fall back to the file's
    // own channel count, identical to a default-constructed decoder.
    {
        OggVorbisDecoder dec(nullptr, 2);
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
        CHECK(outputs_equal(ref, out));
    }
    {
        const SpeakerRole roles[] = {SpeakerRole::FL};
        OggVorbisDecoder dec(roles, 0);
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
        CHECK(outputs_equal(ref, out));
    }
    {
        const SpeakerRole roles[9] = {};
        OggVorbisDecoder dec(roles, 9);
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
        CHECK(outputs_equal(ref, out));
    }
    // Out-of-range downmix request gets the same fallback
    {
        OggVorbisDecoder dec(5);
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
        CHECK(outputs_equal(ref, out));
    }
    return true;
}

static bool test_error_contract() {
    std::vector<uint8_t> data = read_file("stereo_44100.ogg");
    CHECK(!data.empty());
    // Sanity-check the fixture layout the corruptions below rely on:
    // byte 5 = header type (BOS), bytes 6-13 = granule position, byte 26 =
    // segment count (1), byte 28 = packet type 0x01, byte 29 = 'v' of "vorbis".
    // The single-segment first page is a libvorbis encoder artifact, not a
    // spec guarantee; these checks fail loudly if the fixtures are regenerated
    // with an encoder that lays out the first page differently.
    CHECK_EQ(std::memcmp(data.data(), "OggS", 4), 0);
    CHECK_EQ(data[5], 0x02);
    CHECK_EQ(data[26], 0x01);
    CHECK_EQ(data[28], 0x01);
    CHECK_EQ(data[29], 'v');

    size_t consumed = 0;
    size_t bytes_written = 0;
    std::vector<int16_t> buf(4096);

    // Null input is rejected outright
    {
        OggVorbisDecoder dec;
        OggVorbisResult r = dec.decode(nullptr, 16, reinterpret_cast<uint8_t*>(buf.data()),
                                       buf.size() * sizeof(int16_t), consumed, bytes_written);
        CHECK_EQ(r, micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID);
    }

    // Missing BOS flag on the identification page (spec 4.2.1)
    {
        std::vector<uint8_t> bad = data;
        bad[5] = 0x00;
        OggVorbisDecoder dec;
        DecodeOutput out = decode_stream(dec, bad.data(), bad.size());
        CHECK(out.errored);
        CHECK_EQ(out.error, micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID);
    }

    // Non-zero granule position on the identification page (spec 4.2.2)
    {
        std::vector<uint8_t> bad = data;
        bad[6] = 0x01;
        OggVorbisDecoder dec;
        DecodeOutput out = decode_stream(dec, bad.data(), bad.size());
        CHECK(out.errored);
        CHECK_EQ(out.error, micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID);
    }

    // Corrupted "vorbis" magic in the identification packet
    {
        std::vector<uint8_t> bad = data;
        bad[29] = 'x';
        OggVorbisDecoder dec;
        DecodeOutput out = decode_stream(dec, bad.data(), bad.size());
        CHECK(out.errored);
        CHECK_EQ(out.error, micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID);
    }

    // Once decoding, a null output pointer is invalid and a zero-byte buffer
    // reports TOO_SMALL (per the documented decode() contract)
    {
        OggVorbisDecoder dec;
        size_t off = 0;
        // Run header parsing until STREAM_INFO_READY. is_valid() alone would not
        // prove it: that turns true after the identification header, before
        // the decoder reaches STATE_DECODING, which is the state under test.
        bool header_ready = false;
        for (int i = 0; i < 1000 && !header_ready; i++) {
            OggVorbisResult r = dec.decode(data.data() + off, data.size() - off,
                                           reinterpret_cast<uint8_t*>(buf.data()),
                                           buf.size() * sizeof(int16_t), consumed, bytes_written);
            off += consumed;
            CHECK(r >= 0);
            header_ready = (r == micro_vorbis::OGG_VORBIS_DECODER_STREAM_INFO_READY);
        }
        CHECK(header_ready);

        OggVorbisResult r =
            dec.decode(data.data() + off, data.size() - off, nullptr, 0, consumed, bytes_written);
        CHECK_EQ(r, micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID);
        CHECK_EQ(consumed, 0);

        r = dec.decode(data.data() + off, data.size() - off, reinterpret_cast<uint8_t*>(buf.data()),
                       0, consumed, bytes_written);
        CHECK_EQ(r, micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL);
        CHECK_EQ(consumed, 0);
    }
    return true;
}

static bool test_crc_validation() {
    DecodeOutput ref = reference_decode("stereo_44100.ogg");
    CHECK(!ref.errored);

    // A pristine stream with CRC checking enabled decodes identically
    {
        OggVorbisDecoder dec(0, true);
        DecodeOutput out = decode_file(dec, "stereo_44100.ogg");
        CHECK(outputs_equal(ref, out));
    }

    // A corrupted byte in a later page must fail the page CRC
    {
        std::vector<uint8_t> bad = read_file("stereo_44100.ogg");
        CHECK(bad.size() > 200);
        bad[bad.size() - 100] ^= 0x55;
        OggVorbisDecoder dec(0, true);
        DecodeOutput out = decode_stream(dec, bad.data(), bad.size());
        CHECK(out.errored);
        CHECK(out.error < 0);
    }
    return true;
}

static bool test_corrupt_audio_packet_survives() {
    // With CRC validation off (the default), one bad audio packet must not kill
    // the stream. handle_audio_packet() skips a packet that vorbis_synthesis()
    // rejects (returns NEED_MORE_DATA) and decoding continues to EOS. The fuzz
    // harness covers memory safety on malformed input; this covers the wrapper
    // continuing past the bad packet.
    DecodeOutput ref = reference_decode("stereo_44100.ogg");
    CHECK(!ref.errored);
    CHECK(ref.eos);
    CHECK(!ref.pcm.empty());

    std::vector<uint8_t> bad = read_file("stereo_44100.ogg");
    CHECK(bad.size() > 1024);

    // Walk the Ogg page + segment structure to record the byte offset where each
    // Vorbis packet starts. A packet is a run of segments ending at the first
    // lacing value < 255, and may span pages. Page header layout: "OggS"(4)
    // ver(1) flags(1) granule(8) serial(4) seq(4) crc(4) nsegs(1) lacing(nsegs).
    std::vector<size_t> packet_starts;
    {
        bool in_packet = false;
        for (size_t pos = 0; pos + 27 <= bad.size() && std::memcmp(&bad[pos], "OggS", 4) == 0;) {
            const uint8_t nsegs = bad[pos + 26];
            const size_t seg_table = pos + 27;
            if (seg_table + nsegs > bad.size()) {
                break;
            }
            size_t off = seg_table + nsegs;  // start of this page's body
            for (uint8_t s = 0; s < nsegs; s++) {
                if (!in_packet) {
                    packet_starts.push_back(off);
                    in_packet = true;
                }
                const uint8_t lacing = bad[seg_table + s];
                off += lacing;
                if (lacing < 255) {
                    in_packet = false;  // packet ends on this segment
                }
            }
            pos = off;
        }
    }
    // Packets 0-2 are the id/comment/setup headers; 3+ are audio. Target the
    // audio packet nearest the audio midpoint (not the file midpoint: the header
    // packets sit at the front of the file), so decoded audio sits on both sides.
    CHECK(packet_starts.size() > 3);
    const size_t audio_mid = (packet_starts[3] + bad.size()) / 2;
    size_t target = 0;
    size_t best = SIZE_MAX;
    for (size_t i = 3; i < packet_starts.size(); i++) {
        const size_t s = packet_starts[i];
        const size_t d = (s > audio_mid) ? (s - audio_mid) : (audio_mid - s);
        if (s < bad.size() && d < best) {
            best = d;
            target = s;
        }
    }
    CHECK(target != 0);

    // Flip the low bit of that packet's first byte. Tremor reads bit 0 of an
    // audio packet as 0; setting it to 1 makes vorbis_synthesis() return
    // OV_ENOTAUDIO, so handle_audio_packet() skips the packet (NEED_MORE_DATA).
    // Page framing is untouched, so the demuxer and other packets stay valid.
    bad[target] |= 0x01;

    OggVorbisDecoder dec;  // default: CRC off, so the damage reaches Tremor
    DecodeOutput out = decode_stream(dec, bad.data(), bad.size());

    // The stream survives the bad packet: no fatal error, headers parsed, and
    // the EOS page is still reached.
    CHECK(!out.errored);
    CHECK(out.header_ready);
    CHECK(out.eos);
    CHECK_EQ(out.sample_rate, ref.sample_rate);
    CHECK_EQ(out.channels, ref.channels);

    // Only one packet was dropped, so nearly all of the audio is still produced.
    CHECK(out.pcm.size() >= ref.pcm.size() * 9 / 10);

    // Output before the damaged packet is unchanged. The packet sits near the
    // audio midpoint, so the first quarter of samples predates it even under VBR
    // and must match the reference exactly.
    const size_t prefix = ref.pcm.size() / 4;
    for (size_t i = 0; i < prefix; i++) {
        CHECK_EQ(out.pcm[i], ref.pcm[i]);
    }
    return true;
}

// ============================================================================
// Runner
// ============================================================================

struct TestCase {
    const char* name;
    bool (*fn)();
};

static const TestCase TESTS[] = {
    {"pcm_format_contract", test_pcm_format_contract},
    {"chunked_invariance", test_chunked_invariance},
    {"small_output_buffer_recovery", test_small_output_buffer_recovery},
    {"end_of_stream_sticky", test_end_of_stream_sticky},
    {"reset_reuse", test_reset_reuse},
    {"downmix_mono_from_stereo", test_downmix_mono_from_stereo},
    {"upmix_stereo_from_mono", test_upmix_stereo_from_mono},
    {"downmix_stereo_from_multichannel", test_downmix_stereo_from_multichannel},
    {"selection_planes_bit_exact", test_selection_planes_bit_exact},
    {"selection_absent_role_silence", test_selection_absent_role_silence},
    {"selection_mono_routing", test_selection_mono_routing},
    {"selection_invalid_fallback", test_selection_invalid_fallback},
    {"error_contract", test_error_contract},
    {"crc_validation", test_crc_validation},
    {"corrupt_audio_packet_survives", test_corrupt_audio_packet_survives},
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <data_dir> [test_name]\n", argv[0]);
        return 2;
    }
    g_data_dir = argv[1];
    const char* filter = (argc > 2) ? argv[2] : nullptr;

    int ran = 0;
    int failed = 0;
    for (const TestCase& t : TESTS) {
        if (filter && std::strcmp(filter, t.name) != 0) {
            continue;
        }
        ran++;
        std::printf("[ RUN  ] %s\n", t.name);
        bool ok = t.fn();
        std::printf("[ %s ] %s\n", ok ? "PASS" : "FAIL", t.name);
        if (!ok) {
            failed++;
        }
    }
    if (ran == 0) {
        std::fprintf(stderr, "no test matches '%s'\n", filter);
        return 2;
    }
    std::printf("%d/%d tests passed\n", ran - failed, ran);
    return failed == 0 ? 0 : 1;
}
