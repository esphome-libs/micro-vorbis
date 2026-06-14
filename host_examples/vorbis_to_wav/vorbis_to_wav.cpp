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

/* Ogg Vorbis to WAV Converter
 * Converts .ogg files to .wav format using microVorbis
 */

#include "micro_vorbis/ogg_vorbis_decoder.h"
#include "wav_writer.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

// Vorbis decodes multichannel audio in its own native channel order (Vorbis I
// spec section 4.3.9), which differs from the canonical WAV/SMPTE order for 3
// and 5-8 channels. A plain PCM WAV carries no layout info, so a player would
// have to guess; for >2 channels we instead emit WAVE_FORMAT_EXTENSIBLE with a
// speaker mask, and reorder the interleaved samples into WAV order so the mask
// is actually truthful.
struct ChannelLayout {
    uint32_t mask = 0;               // WAVE_FORMAT_EXTENSIBLE speaker mask (0 => plain PCM)
    std::array<uint8_t, 8> order{};  // WAV output channel i <- Vorbis source order[i]
};

// WAVE_FORMAT_EXTENSIBLE speaker-position bits (Microsoft dwChannelMask).
enum Speaker : uint16_t {
    FL = 0x1,    // front left
    FR = 0x2,    // front right
    FC = 0x4,    // front center
    LFE = 0x8,   // low-frequency effects
    BL = 0x10,   // back left
    BR = 0x20,   // back right
    BC = 0x100,  // back center
    SL = 0x200,  // side left
    SR = 0x400,  // side right
};

ChannelLayout vorbis_channel_layout(unsigned channels) {
    // `order` maps each WAV output slot (canonical ascending-bit order) back to
    // the index Vorbis placed that speaker at (Vorbis I spec section 4.3.9).
    switch (channels) {
        case 3:  // Vorbis L C R          -> WAV FL FR FC
            return {FL | FR | FC, {0, 2, 1}};
        case 4:  // Vorbis FL FR BL BR    -> WAV FL FR BL BR (already aligned)
            return {FL | FR | BL | BR, {0, 1, 2, 3}};
        case 5:  // Vorbis FL FC FR BL BR -> WAV FL FR FC BL BR
            return {FL | FR | FC | BL | BR, {0, 2, 1, 3, 4}};
        case 6:  // Vorbis FL FC FR BL BR LFE      -> WAV FL FR FC LFE BL BR
            return {FL | FR | FC | LFE | BL | BR, {0, 2, 1, 5, 3, 4}};
        case 7:  // Vorbis FL FC FR SL SR BC LFE   -> WAV FL FR FC LFE BC SL SR
            return {FL | FR | FC | LFE | BC | SL | SR, {0, 2, 1, 6, 5, 3, 4}};
        case 8:  // Vorbis FL FC FR SL SR BL BR LFE -> WAV FL FR FC LFE BL BR SL SR
            return {FL | FR | FC | LFE | BL | BR | SL | SR, {0, 2, 1, 7, 5, 6, 3, 4}};
        default:  // mono/stereo (unambiguous as plain PCM) or undefined layouts
            return {};
    }
}

void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " <input.ogg> <output.wav>\n";
    std::cerr << "\nConverts an Ogg Vorbis file to WAV format.\n";
}

void print_error_description(micro_vorbis::OggVorbisResult result) {
    switch (result) {
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID:
            std::cerr << " (OGG_VORBIS_DECODER_ERROR_INPUT_INVALID - Invalid Ogg/Vorbis stream)";
            break;
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL:
            std::cerr << " (OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL)";
            break;
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED:
            std::cerr << " (OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED - Memory allocation failed)";
            break;
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_DECODE_FAILED:
            std::cerr << " (OGG_VORBIS_DECODER_ERROR_DECODE_FAILED - Decode failed)";
            break;
        default:
            break;
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }

        const char* input_file = argv[1];
        const char* output_file = argv[2];

        // Open input file
        std::ifstream input(input_file, std::ios::binary);
        if (!input) {
            std::cerr << "Error: Could not open input file: " << input_file << "\n";
            return 1;
        }

        // Create Ogg Vorbis decoder
        micro_vorbis::OggVorbisDecoder decoder;

        // Input buffer - read chunks sequentially, decoder handles buffering internally
        const size_t chunk_size = 4096;
        std::vector<uint8_t> input_buffer(chunk_size);

        // Output PCM buffer. No audio is produced until the headers are parsed,
        // so start small and size it from max_output_bytes() once the decoder
        // reports STREAM_INFO_READY
        std::vector<int16_t> pcm_buffer(2048);

        std::unique_ptr<WavWriter> wav_writer;
        ChannelLayout layout;                 // resolved once the channel count is known
        std::vector<int16_t> reorder_buffer;  // Vorbis -> WAV channel reordering scratch
        bool reached_eos = false;
        size_t total_packets = 0;
        size_t total_bytes_read = 0;
        size_t total_bytes_consumed = 0;
        size_t decode_calls = 0;

        // Process file - read chunks and feed directly to decoder
        while (input && !reached_eos) {
            // Read a chunk from file
            input.read(reinterpret_cast<char*>(input_buffer.data()), chunk_size);
            std::streamsize bytes_read = input.gcount();

            if (bytes_read == 0) {
                break;  // EOF reached
            }

            total_bytes_read += static_cast<size_t>(bytes_read);

            // Decode from this chunk - decoder may need multiple calls per chunk
            size_t chunk_offset = 0;
            while (chunk_offset < static_cast<size_t>(bytes_read)) {
                size_t consumed = 0;
                size_t bytes_written = 0;

                decode_calls++;

                micro_vorbis::OggVorbisResult result =
                    decoder.decode(input_buffer.data() + chunk_offset,
                                   static_cast<size_t>(bytes_read) - chunk_offset,
                                   reinterpret_cast<uint8_t*>(pcm_buffer.data()),
                                   pcm_buffer.size() * sizeof(int16_t), consumed, bytes_written);

                total_bytes_consumed += consumed;
                chunk_offset += consumed;

                // All three Vorbis headers parsed; PCM format is now available
                if (result == micro_vorbis::OGG_VORBIS_DECODER_STREAM_INFO_READY) {
                    const auto& pcm_format = decoder.get_pcm_format();
                    uint32_t sample_rate = pcm_format.sample_rate();
                    uint8_t channels = static_cast<uint8_t>(pcm_format.num_channels());

                    std::cout << "Vorbis PCM format:\n";
                    std::cout << "  Sample rate: " << sample_rate << " Hz\n";
                    std::cout << "  Channels: " << static_cast<int>(channels)
                              << (channels == 1   ? " (mono)"
                                  : channels == 2 ? " (stereo)"
                                                  : "")
                              << "\n";

                    // Size the PCM buffer for the worst-case frame of this stream.
                    // pcm_buffer holds int16_t, so convert the byte bound to elements.
                    pcm_buffer.resize(pcm_format.max_output_bytes() / sizeof(int16_t));

                    // For >2 channels, emit WAVE_FORMAT_EXTENSIBLE with a speaker
                    // mask and reorder samples into WAV order (see helper above).
                    layout = vorbis_channel_layout(channels);
                    if (layout.mask != 0) {
                        reorder_buffer.resize(pcm_buffer.size());
                    }

                    // Create WAV writer with decoder's format
                    wav_writer = std::make_unique<WavWriter>(output_file, sample_rate, channels, 16,
                                                             layout.mask);

                    if (!wav_writer->isOpen()) {
                        std::cerr << "Error: Could not create output file: " << output_file << "\n";
                        return 1;
                    }
                }

                // Error checking: result < 0 means error (informational codes are >= 0)
                if (result < 0) {
                    // Fallback: should not happen once the buffer is sized from
                    // max_output_bytes(), but resize and retry if it ever does
                    if (result == micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL) {
                        size_t required_elements =
                            decoder.get_required_output_bytes() / sizeof(int16_t);
                        if (required_elements == 0) {
                            std::cerr << "Error: decoder reported a zero-byte output requirement\n";
                            return 1;
                        }
                        std::cout << "Resizing PCM buffer from " << pcm_buffer.size() << " to "
                                  << required_elements << " samples\n";
                        pcm_buffer.resize(required_elements);
                        if (layout.mask != 0) {
                            // Keep the reorder scratch in step with the PCM buffer,
                            // or the reorder loop below would write past its end.
                            reorder_buffer.resize(pcm_buffer.size());
                        }
                        continue;  // Retry decode with larger buffer
                    }

                    // Error occurred - provide detailed error information
                    std::cerr << "Error at byte position " << total_bytes_consumed << " in file\n";
                    std::cerr << "Decode call #" << decode_calls << ", consumed=" << consumed
                              << ", bytes=" << bytes_written << "\n";
                    std::cerr << "Error: Decoding failed with error code: "
                              << static_cast<int>(result);
                    print_error_description(result);
                    std::cerr << "\n";
                    return 1;
                }

                // Stream fully decoded - stop both loops
                if (result == micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM) {
                    reached_eos = true;
                    break;
                }

                // OGG_VORBIS_DECODER_SUCCESS yields audio; bytes_written is the total across
                // all channels. WavWriter wants the per-channel sample count.
                if (bytes_written > 0) {
                    total_packets++;

                    // Write decoded samples to WAV file
                    if (wav_writer) {
                        const uint32_t channels = decoder.get_pcm_format().num_channels();
                        size_t samples_per_channel = bytes_written / sizeof(int16_t) / channels;

                        const int16_t* out = pcm_buffer.data();
                        if (layout.mask != 0) {
                            // Reorder each interleaved frame from Vorbis channel
                            // order into the canonical WAV order the mask declares.
                            for (size_t frame = 0; frame < samples_per_channel; frame++) {
                                const int16_t* src = pcm_buffer.data() + frame * channels;
                                int16_t* dst = reorder_buffer.data() + frame * channels;
                                for (uint32_t ch = 0; ch < channels; ch++) {
                                    dst[ch] = src[layout.order[ch]];
                                }
                            }
                            out = reorder_buffer.data();
                        }

                        if (!wav_writer->writeSamples(out, samples_per_channel)) {
                            std::cerr << "Error: Failed to write samples to WAV file\n";
                            return 1;
                        }
                    }
                }

                // If no bytes consumed AND no output, decoder needs more data - read next chunk
                if (consumed == 0 && bytes_written == 0) {
                    break;  // Exit inner loop, read more from file
                }
            }  // End of inner decode loop
        }  // End of outer file read loop

        // Clean up
        if (wav_writer) {
            std::cout << "\nConversion complete!\n";
            std::cout << "Total decode() calls: " << decode_calls << "\n";
            std::cout << "Total bytes read from file: " << total_bytes_read << "\n";
            std::cout << "Total bytes consumed by decoder: " << total_bytes_consumed << "\n";
            std::cout << "Average bytes per packet: "
                      << (total_bytes_consumed / (total_packets > 0 ? total_packets : 1)) << "\n";
            std::cout << "Total packets decoded: " << total_packets << "\n";
            std::cout << "Total samples written: " << wav_writer->getSamplesWritten() << "\n";
            std::cout << "Duration: "
                      << (static_cast<double>(wav_writer->getSamplesWritten()) /
                          static_cast<double>(decoder.get_pcm_format().sample_rate()))
                      << " seconds\n";
            std::cout << "Output file: " << output_file << "\n";
        } else {
            std::cerr << "Error: No Vorbis stream found in input file\n";
            return 1;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
