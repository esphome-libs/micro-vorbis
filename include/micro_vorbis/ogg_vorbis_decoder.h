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

/// @file ogg_vorbis_decoder.h
/// @brief Ogg Vorbis streaming decoder wrapper for ESP-IDF using Tremor fixed-point decoding

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Forward declarations to avoid exposing implementation details
namespace micro_ogg {
class OggDemuxer;
struct OggPacket;
}  // namespace micro_ogg

namespace micro_vorbis {

// ============================================================================
// Public Types
// ============================================================================

/// @brief Result codes for OggVorbisDecoder operations
///
/// Non-negative values (>= 0) indicate success/informational states, negative values indicate
/// errors. One error, OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL, is recoverable: resize the
/// output buffer and retry the call. After any other error, call reset() and feed the
/// stream again from its first page; input already consumed cannot be re-parsed.
///
/// Error checking pattern:
/// - Use `result < 0` to check for errors
/// - Use `result >= 0` to check for success/informational states
/// - `OGG_VORBIS_DECODER_SUCCESS` (0) means an audio frame was decoded (check bytes_written)
/// - `OGG_VORBIS_DECODER_STREAM_INFO_READY` (1) means all headers are parsed and the PCM format is
/// available
/// - `OGG_VORBIS_DECODER_NEED_MORE_DATA` (2) means no audio was produced; feed more input data
/// - `OGG_VORBIS_DECODER_END_OF_STREAM` (3) means the stream is fully decoded; no more audio
/// - `OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL` (-1) is a recoverable error: the output
/// buffer can't hold this frame; size it with get_required_output_bytes() and retry
enum OggVorbisResult : int8_t {
    // Success / informational (>= 0)
    OGG_VORBIS_DECODER_SUCCESS = 0,  // Audio frame decoded (check bytes_written output parameter)
    OGG_VORBIS_DECODER_STREAM_INFO_READY =
        1,                                  // All three Vorbis headers parsed; PCM format available
    OGG_VORBIS_DECODER_NEED_MORE_DATA = 2,  // No audio produced this call; feed more input data
    OGG_VORBIS_DECODER_END_OF_STREAM = 3,   // Stream fully decoded; no more audio frames

    // Errors (< 0)
    OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL = -1,  // Output buffer can't hold this frame
    // Also returned for an out-of-memory failure while unpacking the setup header
    // (codebooks): Tremor cannot distinguish that allocation failure from a malformed
    // header, so it is reported here instead of as ERROR_ALLOCATION_FAILED.
    OGG_VORBIS_DECODER_ERROR_INPUT_INVALID = -2,      // Invalid Ogg/Vorbis stream structure
    OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED = -3,  // Memory allocation failed
    OGG_VORBIS_DECODER_ERROR_DECODE_FAILED = -4  // Vorbis decode failed (corrupted/invalid packet)
};

/// @brief Format of the PCM that decode() produces
///
/// Describes the decoder's output, not the source file: num_channels() is the
/// resolved output channel count (after any downmix or role selection), so it can
/// differ from the file's channel count. Guaranteed populated once decode() returns
/// OGG_VORBIS_DECODER_STREAM_INFO_READY (all three Vorbis headers parsed); read it
/// then. The fields come from the identification header. Bit depth is fixed because
/// Tremor always emits 16-bit signed PCM.
class PcmFormat {
    friend class OggVorbisDecoder;

    // 32-bit fields
    uint32_t max_block_size_{0};  // Long block size in samples/channel (1 << blocksize_1); a
                                  // packet outputs at most half this per channel (50% MDCT overlap)
    uint32_t sample_rate_{0};     // Sample rate in Hz (from the identification header)

    // 8-bit fields
    uint8_t num_channels_{0};  // Resolved output channel count

public:
    /// @brief Bits per output sample (always 16; microVorbis emits 16-bit signed PCM)
    /// @return Output bit depth in bits (always 16)
    uint32_t bits_per_sample() const {
        return 16;
    }
    /// @brief Bytes per output sample (always 2; microVorbis emits 16-bit signed PCM)
    /// @return Output bytes per sample (always 2)
    uint32_t bytes_per_sample() const {
        return 2;
    }
    /// @brief Safe output buffer size, in bytes, for any single decode() call
    ///
    /// Allocate this many bytes and decode() never returns
    /// OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL; pass it directly as decode()'s
    /// output_size_bytes.
    /// @return Output buffer size in bytes (all channels), or 0 before headers are parsed
    uint32_t max_output_bytes() const {
        return (this->max_block_size_ / 2) * this->num_channels_ * this->bytes_per_sample();
    }
    /// @brief Number of output channels (1 = mono, 2 = stereo, etc.)
    /// @return Output channel count, or 0 before headers are parsed
    uint32_t num_channels() const {
        return this->num_channels_;
    }
    /// @brief Stream sample rate in Hz (e.g., 44100, 48000)
    /// @return Sample rate in Hz, or 0 before headers are parsed
    uint32_t sample_rate() const {
        return this->sample_rate_;
    }
    /// @brief Whether the PCM format has been populated. Guaranteed true once decode()
    /// has returned OGG_VORBIS_DECODER_STREAM_INFO_READY; treat that return as the
    /// signal that the format is ready to read.
    /// @return true once the PCM format is available (guaranteed by
    /// OGG_VORBIS_DECODER_STREAM_INFO_READY)
    bool is_valid() const {
        return this->sample_rate_ != 0;
    }
};

/// @brief Loudspeaker role for raw channel selection
///
/// Passed to the OggVorbisDecoder channel-selecting constructor to pick specific
/// source channels by their loudspeaker role rather than by raw plane index. The
/// decoder maps each role to a plane following the Vorbis I channel order (spec
/// section 4.3.9), which varies with the file's channel count (e.g. quad has no
/// center; 7.1 uses the side channels SL/SR). A role that is absent from the
/// file's layout emits silence.
///
/// Mono (1-channel) sources route all three front roles (FL, FR, FC) to the single
/// available plane, so requesting a stereo {FL, FR} selection from a mono file
/// duplicates that plane rather than producing silence.
///
/// Spec: https://xiph.org/vorbis/doc/Vorbis_I_spec.html (section 4.3.9, channel order)
enum class SpeakerRole : uint8_t {
    FL,   // Front left
    FR,   // Front right
    FC,   // Front center
    LFE,  // Low-frequency effects
    RL,   // Rear (back) left
    RR,   // Rear (back) right
    SL,   // Side left (7.1 / 6.1 layouts)
    SR,   // Side right (7.1 / 6.1 layouts)
    BC    // Back center (6.1 layout)
};

// ============================================================================
// OggVorbisDecoder
// ============================================================================

/**
 * @brief Streaming Ogg Vorbis Decoder
 *
 * This class provides a high-level interface for decoding Vorbis audio
 * from Ogg container streams using the Tremor fixed-point decoder. It handles:
 * - Ogg container parsing (pages, packets, segments)
 * - Vorbis identification, comment, and setup header parsing
 * - Streaming decode with user-managed buffers
 * - Minimal internal buffering: only when packets span pages or input is incomplete
 *
 * @warning Thread Safety: This class is NOT thread-safe. Each decoder instance
 *          must be accessed from only one thread at a time. If you need to decode
 *          multiple streams concurrently, create separate decoder instances for
 *          each thread. Do not share a single decoder instance between multiple
 *          threads without external synchronization.
 *
 * @note Lazy Allocation: The constructor always succeeds and does not allocate
 *       any resources. The OggDemuxer buffers are allocated on the first call
 *       to decode(); the Tremor decoder state is allocated while the headers
 *       are processed. If any allocation fails (PSRAM or internal RAM),
 *       decode() returns OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED; recover by
 *       freeing memory, calling reset(), and feeding the stream again from its
 *       first page. Once decoding has started, no further allocations occur
 *       until reset() begins a new stream.
 *
 * Usage:
 * 1. Create decoder instance (constructor always succeeds)
 * 2. Call decode() with chunks of Ogg Vorbis data
 * 3. Check the return value: result < 0 means error; result >= 0 is success/informational
 * 4. On OGG_VORBIS_DECODER_SUCCESS, check bytes_written for the decoded audio
 * 5. Advance input pointer by bytes_consumed
 * 6. Repeat until decode() returns OGG_VORBIS_DECODER_END_OF_STREAM
 *
 * Example:
 * @code
 * OggVorbisDecoder decoder;  // Constructor always succeeds
 * uint8_t* pcm_buffer = nullptr;  // Allocated once the PCM format is known
 * size_t pcm_buffer_bytes = 0;
 *
 * while (have_data) {
 *     size_t consumed, bytes;
 *     OggVorbisResult result = decoder.decode(
 *         input_ptr, input_len,
 *         pcm_buffer, pcm_buffer_bytes,
 *         consumed, bytes
 *     );
 *
 *     if (result < 0) {
 *         // Handle error; with the buffer sized below, the recoverable
 *         // OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL never fires
 *         break;
 *     }
 *     if (result == OGG_VORBIS_DECODER_STREAM_INFO_READY) {
 *         // Headers parsed: size the output buffer once, in bytes.
 *         // STREAM_INFO_READY fires again after reset() for a new stream, so free
 *         // any prior buffer first to avoid leaking when reusing the decoder.
 *         delete[] pcm_buffer;
 *         pcm_buffer_bytes = decoder.get_pcm_format().max_output_bytes();
 *         pcm_buffer = new uint8_t[pcm_buffer_bytes];
 *     }
 *     if (result == OGG_VORBIS_DECODER_END_OF_STREAM) {
 *         break;  // Stream fully decoded
 *     }
 *
 *     // OGG_VORBIS_DECODER_SUCCESS yields bytes; STREAM_INFO_READY / NEED_MORE_DATA yield none
 *     if (bytes > 0) {
 *         // Process PCM (bytes is the total written across all channels)
 *         process_audio(reinterpret_cast<int16_t*>(pcm_buffer), bytes / sizeof(int16_t));
 *     }
 *
 *     input_ptr += consumed;
 *     input_len -= consumed;
 * }
 * delete[] pcm_buffer;
 * @endcode
 */
class OggVorbisDecoder {
public:
    // ========================================
    // Lifecycle
    // ========================================

    /// @brief Construct a smart-downmixing Ogg Vorbis decoder
    ///
    /// The constructor always succeeds and does not allocate any resources.
    /// All allocations are deferred to the first call to decode().
    ///
    /// @param channels Output channel count. 0 = use file's channel count (default).
    ///                 1 = mono, 2 = stereo. The decoder smart-downmixes surround
    ///                 input per ITU-R BS.775: center/surround at -3 dB relative to
    ///                 the fronts, LFE dropped, and the mix normalized so a
    ///                 full-scale fold cannot clip (4.6-9.9 dB of attenuation,
    ///                 depending on the source layout). Any value other than 1 or 2
    ///                 falls back to the file's channel count.
    /// @param enable_crc Enable CRC32 validation of Ogg pages (default false)
    ///
    /// @note This constructor is guaranteed not to fail. Resource allocation
    ///       is deferred to the first decode() call, which can return
    ///       OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED if memory allocation fails.
    ///
    /// @note CRC Validation: When enabled, Ogg page checksums are verified per
    ///       RFC 3533. Because packets are handed to the decoder as soon as they
    ///       are parsed, a checksum failure on a page carrying multiple packets is
    ///       reported on that page's final packet; packets earlier on the same
    ///       page have already been decoded. When disabled, the per-page checksum
    ///       computation is skipped and page corruption is not detected.
    explicit OggVorbisDecoder(uint8_t channels = 0, bool enable_crc = false);

    /// @brief Construct a raw channel-selecting Ogg Vorbis decoder
    ///
    /// Selects specific source channels by loudspeaker role: each output channel is
    /// fed, at unity gain and with no mixing, by the source channel matching the
    /// corresponding SpeakerRole. This is distinct from the smart downmix of the
    /// other constructor; no surround folding or attenuation is applied. Use it to
    /// extract a single channel (e.g. one role to a standalone speaker) or a custom
    /// subset (e.g. {FL, FR} for fronts-only stereo from 5.1).
    ///
    /// A requested role that is absent from the file's channel layout emits silence,
    /// except that a mono source routes the front roles (FL, FR, FC) to its single
    /// plane. See SpeakerRole for the per-layout role mapping.
    ///
    /// The selected count becomes the output channel count reported by
    /// get_pcm_format().num_channels().
    ///
    /// @param roles Pointer to an array of SpeakerRole values, one per output channel.
    ///              The array is copied into the decoder and need not outlive the call.
    /// @param count Number of output channels (1 to 8). An invalid count (null roles,
    ///              0, or >8) leaves the selection unset, so the decoder falls back to
    ///              the file's channel count, the same best-effort treatment the
    ///              downmix constructor gives an out-of-range `channels` value.
    /// @param enable_crc Enable CRC32 validation of Ogg pages (default false)
    ///
    /// @note Like the downmix constructor, this always succeeds and defers all
    ///       resource allocation to the first decode() call. See that constructor for
    ///       the CRC-validation note.
    explicit OggVorbisDecoder(const SpeakerRole* roles, uint8_t count, bool enable_crc = false);

    /// @brief Destroy the decoder and free resources
    ~OggVorbisDecoder();

    // Non-copyable and non-movable: the decoder pins Tremor's decode state (vorbis_info/
    // vorbis_dsp_state/vorbis_block), which holds interior pointers back into itself
    // (vd.vi, vb.vd, arena and pcm buffers). That state cannot be relocated, so the
    // decoder is a fixed-in-place resource; construct it once and reset() between streams.
    OggVorbisDecoder(const OggVorbisDecoder&) = delete;
    OggVorbisDecoder& operator=(const OggVorbisDecoder&) = delete;
    OggVorbisDecoder(OggVorbisDecoder&&) = delete;
    OggVorbisDecoder& operator=(OggVorbisDecoder&&) = delete;

    /// @brief Reset the decoder to its initial state, ready to decode a new stream
    ///
    /// Resets all decode state (Tremor block/DSP/info state, Ogg demuxer state, the
    /// header-parse state machine, PCM format, packet counter, end-of-stream and
    /// pending-PCM flags, and the required-output-bytes counter). The Tremor block,
    /// DSP, and codebook arenas are freed and then re-allocated from the next stream's
    /// setup, because their sizes depend on that stream's codebooks, block size, and
    /// channel count. The Ogg demuxer's packet-assembly buffer is kept and reused. The
    /// next decode() can therefore still return OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED
    /// if memory is unavailable.
    ///
    /// User configuration is preserved across the reset:
    /// - CRC checking enable/disable (constructor `enable_crc` parameter)
    /// - Downmix target channel count (constructor `channels` parameter)
    /// - Raw channel selection (constructor `roles`/`count` parameters)
    ///
    /// After this call the decoder behaves like a freshly constructed
    /// OggVorbisDecoder, except that the demuxer buffer and the preserved
    /// configuration above are retained. To fully release memory, destroy
    /// the decoder instance.
    void reset();

    // ========================================
    // Core Decoding API
    // ========================================

    /// @brief Decode Ogg Vorbis data and output PCM samples
    ///
    /// This method processes input data, parsing Ogg pages and packets,
    /// and decoding Vorbis frames to PCM output.
    ///
    /// @param input Pointer to input Ogg Vorbis data (must not be nullptr)
    /// @param input_len Number of bytes available in input
    /// @param output Pointer to output buffer for PCM samples. May be nullptr during
    ///               header parsing; must not be nullptr once decoding begins.
    ///               The buffer must be aligned for int16_t access (2-byte alignment);
    ///               buffers from new/malloc/heap_caps_malloc always satisfy this.
    ///               Currently outputs 16-bit signed PCM samples (int16_t).
    /// @param output_size_bytes Number of bytes available in output buffer
    /// @param[out] bytes_consumed Number of input bytes consumed (may be buffered internally)
    /// @param[out] bytes_written Number of PCM bytes written to output (total across all channels;
    ///                           e.g. a stereo block of 1024 frames => 2048 samples => 4096 bytes)
    ///
    /// @return Informational code (>= 0) or negative error code; see OggVorbisResult
    ///
    /// @note Lazy Allocation: internal resources are allocated as needed while
    ///       the headers are processed (preferring PSRAM on ESP32). If allocation
    ///       fails, returns OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED; recover
    ///       with reset() and re-feed the stream from its first page.
    ///
    /// @note Parameter Validation: input must be non-null (always). output must be
    ///       non-null only after headers are processed (STATE_DECODING); during header
    ///       parsing, a null output pointer is accepted.
    ///
    /// @note The user must advance the input pointer by bytes_consumed before
    ///       calling decode() again.
    /// @note output_size_bytes and bytes_written are both in bytes, so they share the
    ///       output buffer's unit directly. If the buffer is too small, decode() returns
    ///       OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL; call
    ///       get_required_output_bytes() for the exact byte count and retry.
    /// @note Accepts arbitrarily small input chunks (even 1 byte at a time); an internal
    ///       staging buffer reassembles header packets split across chunks.
    OggVorbisResult decode(const uint8_t* input, size_t input_len, uint8_t* output,
                           size_t output_size_bytes, size_t& bytes_consumed, size_t& bytes_written);

    // ========================================
    // PCM Format
    // ========================================

    /// @brief Get the format of the PCM that decode() produces
    ///
    /// Guaranteed available once decode() returns OGG_VORBIS_DECODER_STREAM_INFO_READY;
    /// use that return as the signal to read it. Until the format is populated, accessors
    /// like num_channels() and sample_rate() return 0; check get_pcm_format().is_valid().
    ///
    /// @return Reference to the PcmFormat struct
    const PcmFormat& get_pcm_format() const {
        return this->pcm_format_;
    }

    // ========================================
    // Output Buffer Helpers
    // ========================================

    /// @brief Get the required output buffer size, in bytes, for the last packet
    ///
    /// This method returns the byte count (across all channels) needed to decode the most
    /// recently processed audio packet. Call it after receiving
    /// OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL to size the buffer before retrying;
    /// pass it directly as decode()'s output_size_bytes.
    ///
    /// Unlike max_output_bytes() (a stream-wide upper bound), this is the exact size the last
    /// packet needs, so callers that allocate lazily can grow to fit each packet.
    ///
    /// @return Required size in bytes (all channels), or 0 if no audio packet has been
    ///         processed yet (i.e., still parsing headers)
    ///
    /// @note This value is updated each time an audio packet is processed,
    ///       regardless of whether decoding succeeded or failed.
    size_t get_required_output_bytes() const {
        return this->required_output_bytes_;
    }

private:
    // ========================================
    // Decode Pipeline
    // ========================================

    /// @brief Dispatch a demuxed packet to the appropriate state handler
    OggVorbisResult process_packet(const micro_ogg::OggPacket& packet, uint8_t* output,
                                   size_t output_size_bytes, size_t& bytes_written);

    /// @brief Parse and process the Vorbis identification header packet
    OggVorbisResult handle_identification_packet(const uint8_t* packet_data, size_t packet_len,
                                                 int64_t granule_pos, bool is_bos);

    /// @brief Stream through the Vorbis comment header packet without buffering it
    OggVorbisResult stream_vorbis_comment(const uint8_t* input, size_t input_len,
                                          size_t& bytes_consumed);

    /// @brief Parse and process the Vorbis setup header packet and initialize DSP state
    OggVorbisResult handle_setup_packet(const uint8_t* packet_data, size_t packet_len,
                                        int64_t granule_pos);

    /// @brief Synthesize PCM audio from a Vorbis audio packet
    OggVorbisResult handle_audio_packet(const uint8_t* packet_data, size_t packet_len,
                                        int64_t granule_pos, bool is_eos, uint8_t* output,
                                        size_t output_size_bytes, size_t& bytes_written);

    /// @brief Read pending PCM samples from tremor DSP state into output buffer
    OggVorbisResult output_pcm(uint8_t* output, size_t output_size_bytes, size_t& bytes_written);

    // ========================================
    // Private Types
    // ========================================

    /// @brief Internal state machine states for header and audio decoding
    enum State : uint8_t {
        STATE_EXPECT_IDENTIFICATION,
        STATE_EXPECT_COMMENT,
        STATE_STREAMING_COMMENT,  // Streaming through comment via get_next_data()
        STATE_EXPECT_SETUP,
        STATE_DECODING
    };

    struct TremorState;

    // ========================================
    // Member Variables
    // ========================================

    // Struct fields

    // Output PCM format (populated after parsing the identification header)
    PcmFormat pcm_format_{};

    // Pointer fields

    // Ogg demuxer
    std::unique_ptr<micro_ogg::OggDemuxer> ogg_demuxer_;

    // Tremor decoder state (pimpl pattern to avoid exposing Tremor headers)
    std::unique_ptr<TremorState> tremor_;

    // 64-bit fields

    // Packet counter (for debugging and validation)
    int64_t packet_count_{0};

    // size_t fields

    // Output byte count (all channels) the last audio packet needs
    size_t required_output_bytes_{0};

    // 8-bit fields

    uint8_t comment_magic_buf_[7]{};  // Buffer for 0x03 + "vorbis" magic
    uint8_t comment_magic_len_{0};    // Bytes accumulated in magic buffer
    bool enable_crc_{false};          // CRC validation setting (passed to OggDemuxer)
    bool has_pending_pcm_{false};     // Set when ERROR_OUTPUT_BUFFER_TOO_SMALL is returned
    bool received_eos_{false};        // Set once the end-of-stream page has been seen
    uint8_t requested_channels_{0};   // Output channel count (0 = use file's channel count)
    SpeakerRole sel_[8]{};            // Raw channel selection (role-selecting constructor)
    uint8_t sel_count_{0};            // Roles set in sel_; 0 = none (use requested_channels_)
    State state_{STATE_EXPECT_IDENTIFICATION};  // Position in the header/decode state machine
};

}  // namespace micro_vorbis
