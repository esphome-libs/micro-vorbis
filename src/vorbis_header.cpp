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

// Implements Vorbis I specification header packet parsing

#include "vorbis_header.h"

#include <cstring>

namespace micro_vorbis {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/// @brief Read a 32-bit little-endian integer from a byte buffer
inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr size_t VORBIS_HEADER_TYPE_SIZE = 1;
constexpr size_t VORBIS_MAGIC_SIZE = 6;
constexpr size_t VORBIS_HEADER_MAGIC_SIZE = VORBIS_HEADER_TYPE_SIZE + VORBIS_MAGIC_SIZE;

constexpr size_t VORBIS_IDENTIFICATION_SIZE = 30;

constexpr uint8_t VORBIS_PACKET_TYPE_IDENTIFICATION = 0x01;
constexpr uint8_t VORBIS_PACKET_TYPE_COMMENT = 0x03;
constexpr uint8_t VORBIS_PACKET_TYPE_SETUP = 0x05;

constexpr size_t VORBIS_VERSION_OFFSET = 7;
constexpr size_t VORBIS_CHANNELS_OFFSET = 11;
constexpr size_t VORBIS_SAMPLE_RATE_OFFSET = 12;
constexpr size_t VORBIS_BITRATE_MAX_OFFSET = 16;
constexpr size_t VORBIS_BITRATE_NOM_OFFSET = 20;
constexpr size_t VORBIS_BITRATE_MIN_OFFSET = 24;
constexpr size_t VORBIS_BLOCKSIZES_OFFSET = 28;
constexpr size_t VORBIS_FRAMING_OFFSET = 29;

constexpr uint8_t VORBIS_MIN_BLOCKSIZE = 6;
constexpr uint8_t VORBIS_MAX_BLOCKSIZE = 13;

/// @brief Check that the packet starts with the expected Vorbis packet-type byte followed by the
/// "vorbis" magic string
inline bool check_vorbis_magic(const uint8_t* packet, size_t packet_len, uint8_t packet_type) {
    if (packet_len < VORBIS_HEADER_MAGIC_SIZE) {
        return false;
    }
    return packet[0] == packet_type && memcmp(packet + 1, "vorbis", VORBIS_MAGIC_SIZE) == 0;
}

}  // namespace

// ============================================================================
// Functions
// ============================================================================

VorbisHeaderResult parse_vorbis_identification(const uint8_t* packet, size_t packet_len,
                                               VorbisIdentification& info) {
    if (!is_vorbis_identification(packet, packet_len)) {
        return VORBIS_HEADER_INVALID_MAGIC;
    }

    if (packet_len < VORBIS_IDENTIFICATION_SIZE) {
        return VORBIS_HEADER_TOO_SHORT;
    }

    info.vorbis_version = read_le32(packet + VORBIS_VERSION_OFFSET);
    info.channel_count = packet[VORBIS_CHANNELS_OFFSET];
    info.sample_rate = read_le32(packet + VORBIS_SAMPLE_RATE_OFFSET);
    info.bitrate_maximum = (int32_t)read_le32(packet + VORBIS_BITRATE_MAX_OFFSET);
    info.bitrate_nominal = (int32_t)read_le32(packet + VORBIS_BITRATE_NOM_OFFSET);
    info.bitrate_minimum = (int32_t)read_le32(packet + VORBIS_BITRATE_MIN_OFFSET);

    constexpr uint8_t BLOCKSIZE_MASK = 0x0F;
    uint8_t blocksizes = packet[VORBIS_BLOCKSIZES_OFFSET];
    info.blocksize_0 = blocksizes & BLOCKSIZE_MASK;
    info.blocksize_1 = (blocksizes >> 4) & BLOCKSIZE_MASK;

    uint8_t framing = packet[VORBIS_FRAMING_OFFSET];

    if (info.vorbis_version != 0) {
        return VORBIS_HEADER_INVALID_VERSION;
    }

    if (info.channel_count == 0) {
        return VORBIS_HEADER_INVALID_CHANNELS;
    }

    if (info.sample_rate == 0) {
        return VORBIS_HEADER_INVALID_SAMPLE_RATE;
    }

    if (info.blocksize_0 < VORBIS_MIN_BLOCKSIZE || info.blocksize_0 > VORBIS_MAX_BLOCKSIZE ||
        info.blocksize_1 < VORBIS_MIN_BLOCKSIZE || info.blocksize_1 > VORBIS_MAX_BLOCKSIZE) {
        return VORBIS_HEADER_INVALID_BLOCKSIZE;
    }

    if (info.blocksize_0 > info.blocksize_1) {
        return VORBIS_HEADER_INVALID_BLOCKSIZE;
    }

    if ((framing & 0x01) == 0) {
        return VORBIS_HEADER_INVALID_FRAMING;
    }

    return VORBIS_HEADER_OK;
}

bool is_vorbis_identification(const uint8_t* packet, size_t packet_len) {
    return check_vorbis_magic(packet, packet_len, VORBIS_PACKET_TYPE_IDENTIFICATION);
}

bool is_vorbis_comment(const uint8_t* packet, size_t packet_len) {
    return check_vorbis_magic(packet, packet_len, VORBIS_PACKET_TYPE_COMMENT);
}

bool is_vorbis_setup(const uint8_t* packet, size_t packet_len) {
    return check_vorbis_magic(packet, packet_len, VORBIS_PACKET_TYPE_SETUP);
}

}  // namespace micro_vorbis
