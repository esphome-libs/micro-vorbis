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

// Ogg Vorbis streaming decoder wrapper for ESP-IDF - OggVorbisDecoder implementation

#include "micro_vorbis/ogg_vorbis_decoder.h"

#include "ivorbiscodec.h"
#include "vorbis_header.h"
#include <micro_ogg/ogg_demuxer.h>
#include <ogg/ogg.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#include <cstdio>
#include <cstdlib>
#endif

#include <cstdint>
#include <cstring>
#include <new>

namespace micro_vorbis {

namespace {
// RFC 3533: Invalid/unknown granule position (-1 in two's complement)
constexpr uint64_t INVALID_GRANULE_POSITION = 0xFFFFFFFFFFFFFFFFULL;

// Vorbis setup headers can be large (codebooks, floor configs, etc.)
// Typical identification + comment headers are small, but setup headers
// can reach several KB depending on encoding complexity.
constexpr size_t MIN_VORBIS_PACKET_SIZE = 1024;    // Initial buffer allocation
constexpr size_t MAX_VORBIS_PACKET_SIZE = 131072;  // 128 KB max for any Vorbis packet

// Tremor fixed-point format: PCM samples are s7.24 (full scale +/-1.0 = +/-2^24)
// Shift right by 9 bits to convert to 16-bit signed integer range
constexpr int TREMOR_PCM_SHIFT = 9;
// Half-LSB bias added before the truncating >>TREMOR_PCM_SHIFT so the result
// rounds to nearest (ties toward +inf) instead of flooring. Costs one add per
// sample; halves the worst-case quantization error from 1 LSB to 1/2 LSB.
constexpr ogg_int32_t TREMOR_PCM_ROUND = 1 << (TREMOR_PCM_SHIFT - 1);
}  // namespace

/// @brief Pimpl struct holding Tremor decoder state to avoid exposing Tremor headers
struct OggVorbisDecoder::TremorState {
    vorbis_info vi{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    bool info_initialized{false};
    bool dsp_initialized{false};
    bool block_initialized{false};
};

// Convert micro_ogg::OggPacket metadata to an ogg_packet for tremor API calls
static ogg_packet make_ogg_packet(const uint8_t* data, size_t length, bool is_bos, bool is_eos,
                                  int64_t granule_pos, int64_t packetno) {
    ogg_packet op = {};
    // Tremor's ogg_packet.packet is non-const (legacy C ABI), but synthesis only reads it.
    op.packet = const_cast<unsigned char*>(data);
    op.bytes = static_cast<long>(length);
    op.b_o_s = is_bos ? 1 : 0;
    op.e_o_s = is_eos ? 1 : 0;
    op.granulepos = granule_pos;
    op.packetno = packetno;
    return op;
}

// Clip a 32-bit value to 16-bit signed range [-32768, 32767]
static inline int16_t clip_to_16(ogg_int32_t val) {
#ifdef __XTENSA__
    // Xtensa clamps: single-cycle saturation to [-2^15, 2^15-1]
    ogg_int32_t result;
    __asm__("clamps %0, %1, 15" : "=r"(result) : "r"(val));
    return static_cast<int16_t>(result);
#else
    if (val > INT16_MAX) {
        return INT16_MAX;
    }
    if (val < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int16_t>(val);
#endif
}

// Round a Tremor s7.24 sample to nearest and clip to int16. The half-LSB bias is
// added as unsigned so the add wraps instead of overflowing (signed-overflow UB)
// on a near-INT32_MAX sample from a malformed stream. Bit-identical to
// (val + TREMOR_PCM_ROUND) >> TREMOR_PCM_SHIFT for any value full scale (+/-2^24)
// can reach.
static inline int16_t round_to_16(ogg_int32_t val) {
    return clip_to_16(static_cast<ogg_int32_t>(static_cast<ogg_uint32_t>(val) + TREMOR_PCM_ROUND) >>
                      TREMOR_PCM_SHIFT);
}

// Smart-downmix coefficients. The fold follows ITU-R BS.775 (also the ATSC A/52
// Lo/Ro defaults): front channels weighted 1.0, center and surround channels
// 0.7071 (-3 dB), LFE dropped, and the whole equation scaled by a per-layout
// normalization gain g = 1 / (sum of one output channel's weights) so that a
// full-scale mix cannot exceed int16 range. BS.775 leaves overload protection
// as an overall attenuation of the linear fold; applying it after the sum is
// algebraically identical to folding it into the weights but quantizes the
// output only once.
//
// The mix runs in two truncating mulhi() stages:
//   stage 1:  mulhi(sample, weight_Q27) -> Q19 terms, summed in int32
//             (4 guard bits of precision below the output LSB; the worst case
//             of four weighted terms per output channel stays far inside int32)
//   stage 2:  mulhi(sum_Q19, gain_Q28)  -> Q15 output
// Note: the direct-copy paths add a half-LSB bias (TREMOR_PCM_ROUND) before
// their >>TREMOR_PCM_SHIFT to round to nearest; mulhi truncates, so a downmix
// can sit up to ~1 LSB below an exactly rounded fold. This is intentional;
// rounding was scoped to the explicit shifts only.
//
// Mono and stereo sources have a single unity weight per output (g = 1), so
// they keep the one-stage Q23 form whose truncation matches the native copy
// exactly: mulhi(s, DMX_UNITY) == s >> TREMOR_PCM_SHIFT (both signs).
constexpr int32_t DMX_UNITY = 1 << 23;    // 1.0    in Q23: mono/stereo passthrough
constexpr int32_t DMX_W_UNITY = 1 << 27;  // 1.0    in Q27: front channels
constexpr int32_t DMX_W_M3DB = 94906266;  // 0.7071 in Q27: center and surround channels
constexpr int32_t DMX_G_3_4 = 157245850;  // 1/1.7071 in Q28 (-4.6 dB): 3/4-channel layouts
constexpr int32_t DMX_G_5_6 = 111189606;  // 1/2.4142 in Q28 (-7.7 dB): 5/6-channel layouts
constexpr int32_t DMX_G_7_8 = 86000611;   // 1/3.1213 in Q28 (-9.9 dB): 7/8-channel layouts

// Signed multiply-high: the top 32 bits of a 32x32 product. Scales a full-precision
// tremor sample (or partial sum) by a fixed-point coefficient, discarding low bits
// only once (at the >>32). Lowers to a single MULSH on Xtensa. The result is
// NOT clamped; callers accumulate in int32 so downmix sums don't saturate per-term.
static inline ogg_int32_t mulhi(ogg_int32_t s, int32_t m) {
    return static_cast<ogg_int32_t>((static_cast<int64_t>(s) * m) >> 32);
}

// Downmix the planar tremor PCM at sample index i to a stereo (lo, ro) pair, following
// the Vorbis I channel order (spec section 4.3.9; note the center channel sits between
// L and R, and the LFE is last). Weights and normalization follow ITU-R BS.775 (see the
// coefficient block above). Outputs are in int16 (Q15) scale but unclamped (kept in
// int32) so the caller can average to mono and/or saturate.
static inline void downmix_stereo(ogg_int32_t** pcm, size_t i, uint8_t channels, ogg_int32_t& lo,
                                  ogg_int32_t& ro) {
    switch (channels) {
        case 1:  // M
            lo = ro = mulhi(pcm[0][i], DMX_UNITY);
            break;
        case 2:  // L R
            lo = mulhi(pcm[0][i], DMX_UNITY);
            ro = mulhi(pcm[1][i], DMX_UNITY);
            break;
        case 3: {  // L C R
            ogg_int32_t c = mulhi(pcm[1][i], DMX_W_M3DB);
            lo = mulhi(mulhi(pcm[0][i], DMX_W_UNITY) + c, DMX_G_3_4);
            ro = mulhi(mulhi(pcm[2][i], DMX_W_UNITY) + c, DMX_G_3_4);
            break;
        }
        case 4:  // FL FR RL RR
            lo = mulhi(mulhi(pcm[0][i], DMX_W_UNITY) + mulhi(pcm[2][i], DMX_W_M3DB), DMX_G_3_4);
            ro = mulhi(mulhi(pcm[1][i], DMX_W_UNITY) + mulhi(pcm[3][i], DMX_W_M3DB), DMX_G_3_4);
            break;
        case 5: {  // FL C FR RL RR
            ogg_int32_t c = mulhi(pcm[1][i], DMX_W_M3DB);
            lo = mulhi(mulhi(pcm[0][i], DMX_W_UNITY) + c + mulhi(pcm[3][i], DMX_W_M3DB), DMX_G_5_6);
            ro = mulhi(mulhi(pcm[2][i], DMX_W_UNITY) + c + mulhi(pcm[4][i], DMX_W_M3DB), DMX_G_5_6);
            break;
        }
        case 6: {  // FL C FR RL RR LFE  (LFE dropped)
            ogg_int32_t c = mulhi(pcm[1][i], DMX_W_M3DB);
            lo = mulhi(mulhi(pcm[0][i], DMX_W_UNITY) + c + mulhi(pcm[3][i], DMX_W_M3DB), DMX_G_5_6);
            ro = mulhi(mulhi(pcm[2][i], DMX_W_UNITY) + c + mulhi(pcm[4][i], DMX_W_M3DB), DMX_G_5_6);
            break;
        }
        case 7: {  // FL C FR SL SR BC LFE  (LFE dropped; back center to both sides)
            ogg_int32_t c = mulhi(pcm[1][i], DMX_W_M3DB);
            ogg_int32_t rc = mulhi(pcm[5][i], DMX_W_M3DB);
            lo = mulhi(mulhi(pcm[0][i], DMX_W_UNITY) + c + mulhi(pcm[3][i], DMX_W_M3DB) + rc,
                       DMX_G_7_8);
            ro = mulhi(mulhi(pcm[2][i], DMX_W_UNITY) + c + mulhi(pcm[4][i], DMX_W_M3DB) + rc,
                       DMX_G_7_8);
            break;
        }
        case 8: {  // FL C FR SL SR RL RR LFE  (LFE dropped)
            ogg_int32_t c = mulhi(pcm[1][i], DMX_W_M3DB);
            lo = mulhi(mulhi(pcm[0][i], DMX_W_UNITY) + c + mulhi(pcm[3][i], DMX_W_M3DB) +
                           mulhi(pcm[5][i], DMX_W_M3DB),
                       DMX_G_7_8);
            ro = mulhi(mulhi(pcm[2][i], DMX_W_UNITY) + c + mulhi(pcm[4][i], DMX_W_M3DB) +
                           mulhi(pcm[6][i], DMX_W_M3DB),
                       DMX_G_7_8);
            break;
        }
        default:  // >8 channels: best-effort, take the first two planes as-is
            lo = mulhi(pcm[0][i], DMX_UNITY);
            ro = mulhi(pcm[1][i], DMX_UNITY);
            break;
    }
}

// Resolve a loudspeaker role to its source plane index for a stream with `channels`
// planes, following the Vorbis I channel order (spec section 4.3.9). Returns -1 if the
// role is absent from that layout (the caller emits silence). Mono routes the three
// front roles to its single plane so a fronts selection from a mono file duplicates it.
static int role_to_plane(SpeakerRole role, uint8_t channels) {
    using R = SpeakerRole;
    if (channels == 1) {
        return (role == R::FL || role == R::FR || role == R::FC) ? 0 : -1;
    }
    // Per-count layouts; trailing padding is unread (loop bounded by `channels`).
    static const R LAYOUTS[9][8] = {
        {},                                                         // 0 (unused)
        {},                                                         // 1 (handled above)
        {R::FL, R::FR},                                             // 2: L R
        {R::FL, R::FC, R::FR},                                      // 3: L C R
        {R::FL, R::FR, R::RL, R::RR},                               // 4: FL FR RL RR
        {R::FL, R::FC, R::FR, R::RL, R::RR},                        // 5: FL C FR RL RR
        {R::FL, R::FC, R::FR, R::RL, R::RR, R::LFE},                // 6: FL C FR RL RR LFE
        {R::FL, R::FC, R::FR, R::SL, R::SR, R::BC, R::LFE},         // 7: FL C FR SL SR BC LFE
        {R::FL, R::FC, R::FR, R::SL, R::SR, R::RL, R::RR, R::LFE},  // 8: FL C FR SL SR RL RR LFE
    };
    if (channels < 2 || channels > 8) {
        return -1;
    }
    for (uint8_t i = 0; i < channels; i++) {
        if (LAYOUTS[channels][i] == role) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Minimal synthetic Vorbis comment packet for tremor:
// 0x03 + "vorbis" (7 bytes header) + vendor_len=0 (4 bytes) + comment_count=0 (4 bytes) + framing=1
static const uint8_t MINIMAL_COMMENT_PACKET[] = {
    0x03, 'v',  'o',  'r',  'b', 'i', 's',  // packet type + magic
    0x00, 0x00, 0x00, 0x00,                 // vendor string length = 0
    0x00, 0x00, 0x00, 0x00,                 // comment count = 0
    0x01                                    // framing bit
};

// ============================================================================
// Lifecycle
// ============================================================================

OggVorbisDecoder::OggVorbisDecoder(uint8_t channels, bool enable_crc)
    : ogg_demuxer_(nullptr),
      tremor_(nullptr),
      enable_crc_(enable_crc),
      requested_channels_(channels <= 2 ? channels : 0) {
    // Only 0 (follow the file), 1 (mono), and 2 (stereo) are supported output targets.
    // Any other request falls back to following the file's channel count.
    //
    // Lazy allocation: all resources allocated on first decode() call
    // Constructor guaranteed to succeed
}

OggVorbisDecoder::OggVorbisDecoder(const SpeakerRole* roles, uint8_t count, bool enable_crc)
    : ogg_demuxer_(nullptr), tremor_(nullptr), enable_crc_(enable_crc) {
    // Raw channel selection: each output channel maps to one source plane by role.
    // An invalid count (null roles, 0, or >8) leaves the selection unset, so the
    // decoder falls back to the file's channel count, mirroring the downmix
    // constructor's treatment of an out-of-range `channels` value.
    //
    // Lazy allocation: all resources allocated on first decode() call
    // Constructor guaranteed to succeed
    if (roles != nullptr && count > 0 && count <= 8) {
        for (uint8_t i = 0; i < count; i++) {
            this->sel_[i] = roles[i];
        }
        this->sel_count_ = count;
    }
}

OggVorbisDecoder::~OggVorbisDecoder() {
    // reset() clears all tremor state (block/dsp/info) in the correct order.
    // ogg_demuxer_ and tremor_ are then automatically freed by unique_ptr.
    this->reset();
}

void OggVorbisDecoder::reset() {
    // Free tremor decode state (block/dsp/info arenas, freed in order). The demuxer
    // and its buffer are preserved below; the next stream re-allocates tremor state.
    if (this->tremor_) {
        if (this->tremor_->block_initialized) {
            vorbis_block_clear(&this->tremor_->vb);
            this->tremor_->block_initialized = false;
        }
        if (this->tremor_->dsp_initialized) {
            vorbis_dsp_clear(&this->tremor_->vd);
            this->tremor_->dsp_initialized = false;
        }
        if (this->tremor_->info_initialized) {
            vorbis_info_clear(&this->tremor_->vi);
            this->tremor_->info_initialized = false;
        }
    }

    if (this->ogg_demuxer_) {
        this->ogg_demuxer_->reset();
    }

    this->state_ = STATE_EXPECT_IDENTIFICATION;
    // Note: enable_crc_, requested_channels_, and the channel selection (sel_/sel_count_)
    // are NOT reset - they are configuration values
    this->comment_magic_len_ = 0;
    this->pcm_format_ = PcmFormat{};
    this->packet_count_ = 0;
    this->required_output_bytes_ = 0;
    this->received_eos_ = false;
    this->has_pending_pcm_ = false;
}

// ============================================================================
// Core Decoding API
// ============================================================================

OggVorbisResult OggVorbisDecoder::decode(const uint8_t* input, size_t input_len, uint8_t* output,
                                         size_t output_size_bytes, size_t& bytes_consumed,
                                         size_t& bytes_written) {
    // Initialize output parameters before any early returns
    bytes_consumed = 0;
    bytes_written = 0;

    // Validate input pointer
    if (!input) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Validate output buffer only when decoding audio (not during header parsing)
    if (this->state_ == STATE_DECODING) {
        if (!output) {
            return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
        }
        if (output_size_bytes == 0) {
            return OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL;
        }
    }

    // Lazy allocation: create demuxer on first use
    if (!this->ogg_demuxer_) {
        micro_ogg::OggDemuxerConfig ogg_config;
        ogg_config.min_buffer_size = MIN_VORBIS_PACKET_SIZE;
        ogg_config.max_buffer_size = MAX_VORBIS_PACKET_SIZE;
        ogg_config.enable_crc = this->enable_crc_;

#ifdef ESP_PLATFORM
        // Use preference-aware allocators on ESP32 (configurable via Kconfig)
        struct AllocFns {
            static void* alloc_fn(size_t size) {
#if defined(CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_PSRAM)
                return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_INTERNAL)
                return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_OGG_DECODER_PSRAM_ONLY)
                return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_OGG_DECODER_INTERNAL_ONLY)
                return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
                // Default: prefer PSRAM with fallback to internal RAM
                return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
            }
            static void* realloc_fn(void* ptr, size_t size) {
#if defined(CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_PSRAM)
                return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_OGG_DECODER_PREFER_INTERNAL)
                return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_OGG_DECODER_PSRAM_ONLY)
                return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_OGG_DECODER_INTERNAL_ONLY)
                return heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
                // Default: prefer PSRAM with fallback to internal RAM
                return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
            }
            static void free_fn(void* ptr) {
                heap_caps_free(ptr);
            }
        };
        ogg_config.alloc = AllocFns::alloc_fn;
        ogg_config.realloc = AllocFns::realloc_fn;
        ogg_config.free = AllocFns::free_fn;
#endif

        // Honor the documented contract: report OOM as ERROR_ALLOCATION_FAILED rather
        // than letting throwing operator new abort (ESP-IDF builds disable exceptions).
        this->ogg_demuxer_.reset(new (std::nothrow) micro_ogg::OggDemuxer(ogg_config));
        if (!this->ogg_demuxer_) {
            return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
        }
    }

    // Lazy allocation: create tremor state on first use
    if (!this->tremor_) {
        this->tremor_.reset(new (std::nothrow) TremorState());
        if (!this->tremor_) {
            return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
        }
    }

    // If previous call returned ERROR_OUTPUT_BUFFER_TOO_SMALL, output pending PCM
    // without consuming new input from the demuxer. This must run before the
    // end-of-stream guard below so the final frame is never dropped when it
    // arrives on the EOS packet but does not fit the output buffer.
    if (this->has_pending_pcm_) {
        return this->output_pcm(output, output_size_bytes, bytes_written);
    }

    // End of stream: once the EOS packet has been fully decoded, report that the
    // stream is complete on every subsequent call.
    if (this->received_eos_) {
        return OGG_VORBIS_DECODER_END_OF_STREAM;
    }

    // Stream through comment header using get_next_data() to avoid buffering
    if (this->state_ == STATE_EXPECT_COMMENT || this->state_ == STATE_STREAMING_COMMENT) {
        return this->stream_vorbis_comment(input, input_len, bytes_consumed);
    }

    // Get next packet from demuxer
    micro_ogg::OggDemuxState parse_state = this->ogg_demuxer_->get_next_packet(input, input_len);
    bytes_consumed = parse_state.bytes_consumed;

    // Handle demuxer results
    if (parse_state.result == micro_ogg::OGG_NEED_MORE_DATA) {
        return OGG_VORBIS_DECODER_NEED_MORE_DATA;
    }

    if (parse_state.result == micro_ogg::OGG_PACKET_SKIPPED) {
        // The demuxer dropped a packet too large for max_buffer_size. During audio
        // decode this is one lost frame, so keep going. During header parsing it means
        // a required header (identification or setup) was dropped and the stream can
        // never be configured, so return an error instead of looping on NEED_MORE_DATA.
        // The comment header streams through stream_vorbis_comment() and is never
        // skipped, so STATE_DECODING is the only non-header state reachable here.
        if (this->state_ != STATE_DECODING) {
            return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
        }
        return OGG_VORBIS_DECODER_NEED_MORE_DATA;
    }

    if (parse_state.result == micro_ogg::OGG_OK) {
        // We have a complete packet - process it
        return this->process_packet(parse_state.packet, output, output_size_bytes, bytes_written);
    }

    // Demuxer encountered error
#if !defined(ESP_PLATFORM) && !defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
    const char* error_msg = "Unknown error";
    switch (parse_state.result) {
        case micro_ogg::OGG_INVALID_CAPTURE:
            error_msg = "Invalid Ogg capture pattern";
            break;
        case micro_ogg::OGG_INVALID_VERSION:
            error_msg = "Unsupported Ogg version";
            break;
        case micro_ogg::OGG_CRC_FAILED:
            error_msg = "CRC checksum validation failed";
            break;
        case micro_ogg::OGG_STREAM_SEQUENCE_ERROR:
            error_msg = "Page sequence number mismatch";
            break;
        case micro_ogg::OGG_STREAM_BOS_ERROR:
            error_msg = "BOS flag violation (invalid placement)";
            break;
        case micro_ogg::OGG_STREAM_EOS_ERROR:
            error_msg = "EOS flag violation (EOS with continued packet)";
            break;
        case micro_ogg::OGG_STREAM_SERIAL_MISMATCH:
            error_msg = "Stream serial mismatch (concatenated stream)";
            break;
        case micro_ogg::OGG_STREAM_CONTINUATION_ERROR:
            error_msg = "Continuation flag inconsistent with previous page";
            break;
        case micro_ogg::OGG_ALLOCATION_FAILED:
            error_msg = "Memory allocation failed";
            break;
        default:
            break;
    }
    fprintf(stderr, "OggVorbisDecoder: Ogg demuxer error (%d): %s\n", parse_state.result,
            error_msg);
#endif

    if (parse_state.result == micro_ogg::OGG_ALLOCATION_FAILED) {
        return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
    }
    return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
}

// ============================================================================
// Decode Pipeline
// ============================================================================

OggVorbisResult OggVorbisDecoder::process_packet(const micro_ogg::OggPacket& packet,
                                                 uint8_t* output, size_t output_size_bytes,
                                                 size_t& bytes_written) {
    const uint8_t* packet_data = packet.data;
    size_t packet_len = packet.length;
    int64_t granule_pos = packet.granule_position;
    bool is_bos = packet.is_bos;
    bool is_eos = packet.is_eos;

    // Dispatch to state handler
    switch (this->state_) {
        case STATE_EXPECT_IDENTIFICATION:
            bytes_written = 0;
            return this->handle_identification_packet(packet_data, packet_len, granule_pos, is_bos);

        case STATE_EXPECT_SETUP:
            bytes_written = 0;
            return this->handle_setup_packet(packet_data, packet_len, granule_pos);

        case STATE_DECODING:
            return this->handle_audio_packet(packet_data, packet_len, granule_pos, is_eos, output,
                                             output_size_bytes, bytes_written);

        case STATE_EXPECT_COMMENT:
        case STATE_STREAMING_COMMENT:
            // Handled via stream_vorbis_comment() in decode(), not here.
            break;
    }

    return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
}

OggVorbisResult OggVorbisDecoder::handle_identification_packet(const uint8_t* packet_data,
                                                               size_t packet_len,
                                                               int64_t granule_pos, bool is_bos) {
    // Vorbis I spec Section 4.2.1: First header packet must have BOS flag
    if (!is_bos) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Validate this is a Vorbis identification header
    if (!is_vorbis_identification(packet_data, packet_len)) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Vorbis I spec Section 4.2.2: Granule position of identification header page must be 0.
    // Accept the -1 "no position" sentinel as well, matching the comment and setup handlers.
    if (granule_pos != 0 && static_cast<uint64_t>(granule_pos) != INVALID_GRANULE_POSITION) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Parse our own identification structure for metadata extraction
    VorbisIdentification vorbis_id{};
    VorbisHeaderResult header_result =
        parse_vorbis_identification(packet_data, packet_len, vorbis_id);
    if (header_result != VORBIS_HEADER_OK) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Initialize tremor info structure
    vorbis_info_init(&this->tremor_->vi);
    this->tremor_->info_initialized = true;

    // vorbis_info_init() allocates codec_setup; a NULL result is OOM, not invalid input.
    if (this->tremor_->vi.codec_setup == nullptr) {
        return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
    }

    // Feed the identification header to tremor via ogg_packet
    ogg_packet op =
        make_ogg_packet(packet_data, packet_len, true, false, granule_pos, this->packet_count_);
    int ret = vorbis_synthesis_headerin(&this->tremor_->vi, &op);
    if (ret != 0) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Store stream parameters from our parsed identification header
    this->pcm_format_.sample_rate_ = vorbis_id.sample_rate;

    // Long block size (1 << blocksize_1); max_output_bytes() halves it for the per-packet bound.
    this->pcm_format_.max_block_size_ = 1U << vorbis_id.blocksize_1;

    // Resolve output channel count. Precedence: an explicit channel selection wins,
    // then the constructor's smart-downmix request, then the file's own channel count.
    this->pcm_format_.num_channels_ = (this->sel_count_ > 0)             ? this->sel_count_
                                      : (this->requested_channels_ != 0) ? this->requested_channels_
                                                                         : vorbis_id.channel_count;

    this->packet_count_++;
    this->state_ = STATE_EXPECT_COMMENT;
    // Identification parsed, but comment and setup headers are still required.
    return OGG_VORBIS_DECODER_NEED_MORE_DATA;
}

OggVorbisResult OggVorbisDecoder::stream_vorbis_comment(const uint8_t* input, size_t input_len,
                                                        size_t& bytes_consumed) {
    // Use get_next_data() for streaming (no internal buffering)
    micro_ogg::OggDemuxState parse_state = this->ogg_demuxer_->get_next_data(input, input_len);
    bytes_consumed = parse_state.bytes_consumed;

    if (parse_state.result == micro_ogg::OGG_NEED_MORE_DATA) {
        return OGG_VORBIS_DECODER_NEED_MORE_DATA;
    }

    if (parse_state.result != micro_ogg::OGG_OK) {
        if (parse_state.result == micro_ogg::OGG_ALLOCATION_FAILED) {
            return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
        }
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    const uint8_t* data = parse_state.packet.data;
    size_t data_len = parse_state.packet.length;

    if (this->state_ == STATE_EXPECT_COMMENT) {
        this->comment_magic_len_ = 0;
        this->state_ = STATE_STREAMING_COMMENT;
    }

    // Accumulate magic bytes (0x03 + "vorbis" = 7 bytes) if we haven't collected all yet
    if (this->comment_magic_len_ < 7) {
        size_t needed = 7 - this->comment_magic_len_;
        size_t copy_len = (data_len < needed) ? data_len : needed;
        memcpy(this->comment_magic_buf_ + this->comment_magic_len_, data, copy_len);
        this->comment_magic_len_ += static_cast<uint8_t>(copy_len);

        // Validate once we have all 7 bytes
        if (this->comment_magic_len_ == 7) {
            if (!is_vorbis_comment(this->comment_magic_buf_, 7)) {
                return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
            }
        }
    }

    // Vorbis I spec Section 4.2.2: Granule position of comment header page must be 0
    if (parse_state.packet.is_last_on_page) {
        int64_t gp = parse_state.packet.granule_position;
        if (gp != 0 && static_cast<uint64_t>(gp) != INVALID_GRANULE_POSITION) {
            return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
        }
    }

    // Check if we've reached the end of the comment packet
    if (parse_state.packet.is_end_of_packet) {
        // Validate magic was fully received
        if (this->comment_magic_len_ < 7) {
            return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
        }

        // Feed a minimal synthetic comment to tremor to mark the comment header
        // seen (required before the setup header is accepted)
        ogg_packet op = make_ogg_packet(MINIMAL_COMMENT_PACKET, sizeof(MINIMAL_COMMENT_PACKET),
                                        false, false, 0, this->packet_count_);
        int ret = vorbis_synthesis_headerin(&this->tremor_->vi, &op);
        if (ret != 0) {
            return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
        }

        this->packet_count_++;
        this->state_ = STATE_EXPECT_SETUP;
    }

    // Comment header still streaming, or done but the setup header is still required.
    return OGG_VORBIS_DECODER_NEED_MORE_DATA;
}

OggVorbisResult OggVorbisDecoder::handle_setup_packet(const uint8_t* packet_data, size_t packet_len,
                                                      int64_t granule_pos) {
    // Validate this is a Vorbis setup header
    if (!is_vorbis_setup(packet_data, packet_len)) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Vorbis I spec Section 4.2.2: Granule position of setup header page must be 0
    if (granule_pos != 0 && static_cast<uint64_t>(granule_pos) != INVALID_GRANULE_POSITION) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Feed the setup header to tremor
    ogg_packet op =
        make_ogg_packet(packet_data, packet_len, false, false, granule_pos, this->packet_count_);
    int ret = vorbis_synthesis_headerin(&this->tremor_->vi, &op);
    if (ret != 0) {
        // A nonzero return is a malformed setup header or an out-of-memory failure during
        // codebook unpack. Tremor collapses both to OV_EBADHEADER, so OOM here surfaces as
        // ERROR_INPUT_INVALID rather than ERROR_ALLOCATION_FAILED.
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Channel-selection optimization: when raw selection is active, tell Tremor
    // to allocate and synthesize only the planes we actually output. The entropy
    // decode and coupling still run for every channel, so kept planes are
    // bit-exact; this skips the floor-apply / iMDCT / window / overlap-add for
    // the rest AND never allocates their PCM history buffers. The mask is fixed
    // before init so dropped channels cost nothing. Only selection mode narrows
    // it; the downmix and native paths keep all planes (keep=nullptr).
    const int stream_channels = this->tremor_->vi.channels;
    // Vorbis channel count is a uint8_t, so a 256-bit (32-byte) mask covers it.
    unsigned char keep[VORBIS_KEEP_BYTES(256)] = {0};
    const unsigned char* keep_mask = nullptr;
    if (this->sel_count_ > 0) {
        for (uint8_t ch = 0; ch < this->sel_count_; ch++) {
            int p = role_to_plane(this->sel_[ch], static_cast<uint8_t>(stream_channels));
            if (p >= 0 && p < stream_channels) {
                vorbis_keep_set(keep, p);
            }
        }
        keep_mask = keep;
    }

    // Initialize DSP state from the fully parsed headers
    ret = vorbis_synthesis_init_ex(&this->tremor_->vd, &this->tremor_->vi, keep_mask,
                                   stream_channels);
    if (ret != 0) {
        return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
    }
    this->tremor_->dsp_initialized = true;

    // Initialize block for synthesis
    ret = vorbis_block_init(&this->tremor_->vd, &this->tremor_->vb);
    if (ret != 0) {
        return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED;
    }
    this->tremor_->block_initialized = true;

    this->packet_count_++;
    this->state_ = STATE_DECODING;
    // All three headers parsed and DSP initialized: the PCM format is now available.
    return OGG_VORBIS_DECODER_STREAM_INFO_READY;
}

OggVorbisResult OggVorbisDecoder::handle_audio_packet(const uint8_t* packet_data, size_t packet_len,
                                                      int64_t granule_pos, bool is_eos,
                                                      uint8_t* output, size_t output_size_bytes,
                                                      size_t& bytes_written) {
    bytes_written = 0;

    // Mark EOS seen
    if (is_eos) {
        this->received_eos_ = true;
    }

    // Empty audio packets are invalid
    if (packet_len == 0) {
        return OGG_VORBIS_DECODER_ERROR_INPUT_INVALID;
    }

    // Create ogg_packet for tremor synthesis
    ogg_packet op =
        make_ogg_packet(packet_data, packet_len, false, is_eos, granule_pos, this->packet_count_);

    // Synthesize audio from the Vorbis packet
    int ret = vorbis_synthesis(&this->tremor_->vb, &op);
    if (ret != 0) {
        // Non-zero is a non-audio packet (OV_ENOTAUDIO, e.g. a stray header) or a
        // corrupt audio packet (OV_EBADPACKET, or a negative mapping-inverse code).
        // Skip it instead of aborting. The first-block warmup that produces no PCM
        // is not seen here: synthesis succeeds and pcmout returns 0, handled in
        // output_pcm(). Advance packet_count_ so Tremor reads this as a real gap.
        this->packet_count_++;
        return OGG_VORBIS_DECODER_NEED_MORE_DATA;
    }

    // Feed the synthesized block into the DSP state
    ret = vorbis_synthesis_blockin(&this->tremor_->vd, &this->tremor_->vb);
    if (ret != 0) {
        // Advance packet_count_ to match the other paths, even though this error
        // is terminal for the caller.
        this->packet_count_++;
        return OGG_VORBIS_DECODER_ERROR_DECODE_FAILED;
    }

    // Read and output PCM samples
    return this->output_pcm(output, output_size_bytes, bytes_written);
}

OggVorbisResult OggVorbisDecoder::output_pcm(uint8_t* output, size_t output_size_bytes,
                                             size_t& bytes_written) {
    bytes_written = 0;

    // Read available PCM samples from the DSP state
    ogg_int32_t** pcm = nullptr;
    int available_samples = vorbis_synthesis_pcmout(&this->tremor_->vd, &pcm);

    if (available_samples <= 0 || pcm == nullptr) {
        // No samples available yet (normal during startup)
        this->packet_count_++;
        return OGG_VORBIS_DECODER_NEED_MORE_DATA;
    }

    size_t num_samples = static_cast<size_t>(available_samples);
    uint8_t stream_channels = static_cast<uint8_t>(this->tremor_->vi.channels);
    const uint8_t output_channels = static_cast<uint8_t>(this->pcm_format_.num_channels());

    // Output byte count (all channels) this packet needs
    this->required_output_bytes_ = num_samples * output_channels * sizeof(int16_t);

    // Check if output buffer is large enough; output_size_bytes is in bytes, as is the requirement
    if (output_size_bytes < this->required_output_bytes_) {
        // Samples remain in DSP state - set pending flag so next decode() call
        // outputs them without consuming a new packet from the demuxer
        this->has_pending_pcm_ = true;
        return OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL;
    }

    // Convert from tremor fixed-point s7.24 format to interleaved 16-bit PCM
    int16_t* pcm_output = reinterpret_cast<int16_t*>(output);

    if (this->sel_count_ > 0) {
        // Raw channel selection: each output channel copies one source plane at unity
        // (no mixing), or emits silence if that role is absent from the file's layout.
        for (uint8_t ch = 0; ch < output_channels; ch++) {
            const int plane = role_to_plane(this->sel_[ch], stream_channels);
            if (plane < 0) {
                // Role absent from this layout: emit silence for the channel.
                for (size_t i = 0; i < num_samples; i++) {
                    pcm_output[i * output_channels + ch] = 0;
                }
            } else {
                const ogg_int32_t* src = pcm[plane];
                size_t i = 0;
                for (; i + 3 < num_samples; i += 4) {
                    pcm_output[i * output_channels + ch] = round_to_16(src[i]);
                    pcm_output[(i + 1) * output_channels + ch] = round_to_16(src[i + 1]);
                    pcm_output[(i + 2) * output_channels + ch] = round_to_16(src[i + 2]);
                    pcm_output[(i + 3) * output_channels + ch] = round_to_16(src[i + 3]);
                }
                for (; i < num_samples; i++) {
                    pcm_output[i * output_channels + ch] = round_to_16(src[i]);
                }
            }
        }
    } else if (output_channels == stream_channels) {
        for (uint8_t ch = 0; ch < output_channels; ch++) {
            const ogg_int32_t* src = pcm[ch];
            size_t i = 0;
            for (; i + 3 < num_samples; i += 4) {
                pcm_output[i * output_channels + ch] = round_to_16(src[i]);
                pcm_output[(i + 1) * output_channels + ch] = round_to_16(src[i + 1]);
                pcm_output[(i + 2) * output_channels + ch] = round_to_16(src[i + 2]);
                pcm_output[(i + 3) * output_channels + ch] = round_to_16(src[i + 3]);
            }
            for (; i < num_samples; i++) {
                pcm_output[i * output_channels + ch] = round_to_16(src[i]);
            }
        }
    } else {
        // Channel conversion. output_channels is guaranteed to be 1 (mono) or 2 (stereo).
        // Compute a full-precision stereo (lo, ro) pair per the Vorbis channel order; mono
        // is the half-sum of that pair.
        if (output_channels == 1) {
            size_t i = 0;
            for (; i + 3 < num_samples; i += 4) {
                ogg_int32_t lo0 = 0, ro0 = 0, lo1 = 0, ro1 = 0, lo2 = 0, ro2 = 0, lo3 = 0, ro3 = 0;
                downmix_stereo(pcm, i, stream_channels, lo0, ro0);
                downmix_stereo(pcm, i + 1, stream_channels, lo1, ro1);
                downmix_stereo(pcm, i + 2, stream_channels, lo2, ro2);
                downmix_stereo(pcm, i + 3, stream_channels, lo3, ro3);
                // Shift is division by 2 for averaging the channels and add 1 for rounding.
                pcm_output[i] = clip_to_16((lo0 + ro0 + 1) >> 1);
                pcm_output[i + 1] = clip_to_16((lo1 + ro1 + 1) >> 1);
                pcm_output[i + 2] = clip_to_16((lo2 + ro2 + 1) >> 1);
                pcm_output[i + 3] = clip_to_16((lo3 + ro3 + 1) >> 1);
            }
            for (; i < num_samples; i++) {
                ogg_int32_t lo = 0, ro = 0;
                downmix_stereo(pcm, i, stream_channels, lo, ro);
                pcm_output[i] = clip_to_16((lo + ro + 1) >> 1);
            }
        } else {
            size_t i = 0;
            for (; i + 3 < num_samples; i += 4) {
                ogg_int32_t lo0 = 0, ro0 = 0, lo1 = 0, ro1 = 0, lo2 = 0, ro2 = 0, lo3 = 0, ro3 = 0;
                downmix_stereo(pcm, i, stream_channels, lo0, ro0);
                downmix_stereo(pcm, i + 1, stream_channels, lo1, ro1);
                downmix_stereo(pcm, i + 2, stream_channels, lo2, ro2);
                downmix_stereo(pcm, i + 3, stream_channels, lo3, ro3);
                pcm_output[i * 2] = clip_to_16(lo0);
                pcm_output[i * 2 + 1] = clip_to_16(ro0);
                pcm_output[(i + 1) * 2] = clip_to_16(lo1);
                pcm_output[(i + 1) * 2 + 1] = clip_to_16(ro1);
                pcm_output[(i + 2) * 2] = clip_to_16(lo2);
                pcm_output[(i + 2) * 2 + 1] = clip_to_16(ro2);
                pcm_output[(i + 3) * 2] = clip_to_16(lo3);
                pcm_output[(i + 3) * 2 + 1] = clip_to_16(ro3);
            }
            for (; i < num_samples; i++) {
                ogg_int32_t lo = 0, ro = 0;
                downmix_stereo(pcm, i, stream_channels, lo, ro);
                pcm_output[i * 2] = clip_to_16(lo);
                pcm_output[i * 2 + 1] = clip_to_16(ro);
            }
        }
    }

    // Tell tremor we consumed all available samples
    vorbis_synthesis_read(&this->tremor_->vd, available_samples);

    // Note: start-priming and end-of-stream granule trimming are handled inside
    // Tremor (vorbis_synthesis_blockin), which trims the final block before
    // vorbis_synthesis_pcmout returns it. The wrapper therefore needs no
    // granule-position bookkeeping of its own.

    // Report the total bytes written across all output channels.
    bytes_written = num_samples * output_channels * sizeof(int16_t);
    this->packet_count_++;
    this->has_pending_pcm_ = false;
    return OGG_VORBIS_DECODER_SUCCESS;
}

}  // namespace micro_vorbis
