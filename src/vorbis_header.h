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

/// @file vorbis_header.h
/// @brief Vorbis header parsing for Ogg Vorbis streams

#pragma once

#include <cstddef>
#include <cstdint>

namespace micro_vorbis {

// ============================================================================
// Structs
// ============================================================================

/// @brief Vorbis identification header structure
/// Fields populated by parse_vorbis_identification() from the first logical
/// Ogg Vorbis packet; all values are as specified in the Vorbis I spec.
struct VorbisIdentification {
    // 32-bit fields
    int32_t bitrate_maximum;  // Maximum bitrate (0 if unset)
    int32_t bitrate_minimum;  // Minimum bitrate (0 if unset)
    int32_t bitrate_nominal;  // Nominal bitrate
    uint32_t sample_rate;     // Audio sample rate in Hz
    uint32_t vorbis_version;  // Vorbis version (must be 0)

    // 8-bit fields
    uint8_t blocksize_0;    // Log2 of short block size
    uint8_t blocksize_1;    // Log2 of long block size
    uint8_t channel_count;  // Number of audio channels
};

// ============================================================================
// Enums
// ============================================================================

/// @brief Result codes for header parsing
enum VorbisHeaderResult : int8_t {
    // Success / informational (>= 0)
    VORBIS_HEADER_OK = 0,  // Header parsed successfully
    // Errors (< 0)
    VORBIS_HEADER_TOO_SHORT = -1,            // Packet shorter than the required fields
    VORBIS_HEADER_INVALID_MAGIC = -2,        // Missing the packet-type byte + "vorbis" magic
    VORBIS_HEADER_INVALID_VERSION = -3,      // Vorbis version field is nonzero
    VORBIS_HEADER_INVALID_CHANNELS = -4,     // Channel count is zero
    VORBIS_HEADER_INVALID_SAMPLE_RATE = -5,  // Sample rate is zero
    VORBIS_HEADER_INVALID_BLOCKSIZE = -6,    // Block sizes out of range or misordered
    VORBIS_HEADER_INVALID_FRAMING = -7       // Framing bit not set
};

// ============================================================================
// Functions
// ============================================================================

/// @brief Parse Vorbis identification header
/// @param packet Packet data (must start with 0x01 + "vorbis")
/// @param packet_len Packet length in bytes
/// @param[out] info Output VorbisIdentification structure
/// @return VORBIS_HEADER_OK on success, a negative VorbisHeaderResult error code on failure
VorbisHeaderResult parse_vorbis_identification(const uint8_t* packet, size_t packet_len,
                                               VorbisIdentification& info);

/// @brief Check if packet is Vorbis identification header
/// @param packet Packet data
/// @param packet_len Packet length
/// @return true if packet starts with 0x01 + "vorbis"
bool is_vorbis_identification(const uint8_t* packet, size_t packet_len);

/// @brief Check if packet is Vorbis comment header
/// @param packet Packet data
/// @param packet_len Packet length
/// @return true if packet starts with 0x03 + "vorbis"
bool is_vorbis_comment(const uint8_t* packet, size_t packet_len);

/// @brief Check if packet is Vorbis setup header
/// @param packet Packet data
/// @param packet_len Packet length
/// @return true if packet starts with 0x05 + "vorbis"
bool is_vorbis_setup(const uint8_t* packet, size_t packet_len);

}  // namespace micro_vorbis
