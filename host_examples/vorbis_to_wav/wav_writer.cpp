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

/* Simple WAV File Writer Implementation */

#include "wav_writer.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

// WAV format tags
constexpr uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

// cbSize for WAVE_FORMAT_EXTENSIBLE: 22 bytes of extension follow the base
// fmt fields (valid-bits + channel mask + 16-byte SubFormat GUID).
constexpr uint16_t FMT_EXTENSION_CB_SIZE = 22;

// KSDATAFORMAT_SUBTYPE_PCM GUID, the SubFormat written for EXTENSIBLE PCM:
// 00000001-0000-0010-8000-00aa00389b71, serialized little-endian.
constexpr uint8_t KSDATAFORMAT_SUBTYPE_PCM[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                  0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// WAV file header structures (little-endian)
#pragma pack(push, 1)
struct RIFFHeader {
    char chunk_id[4];     // "RIFF"
    uint32_t chunk_size;  // File size - 8
    char format[4];       // "WAVE"
};

struct FmtChunk {
    char chunk_id[4];       // "fmt "
    uint32_t chunk_size;    // 16 for PCM
    uint16_t audio_format;  // 1 for PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;    // sample_rate * num_channels * bits_per_sample/8
    uint16_t block_align;  // num_channels * bits_per_sample/8
    uint16_t bits_per_sample;
};

// WAVE_FORMAT_EXTENSIBLE tail, written after FmtChunk when a channel mask is
// supplied. Needed for >2 channels so players don't guess the speaker layout.
struct FmtExtension {
    uint16_t cb_size;                // 22
    uint16_t valid_bits_per_sample;  // 16
    uint32_t channel_mask;           // speaker-position bitfield
    uint8_t sub_format[16];          // KSDATAFORMAT_SUBTYPE_PCM
};

struct DataChunk {
    char chunk_id[4];     // "data"
    uint32_t chunk_size;  // Number of bytes in data
};
#pragma pack(pop)

WavWriter::WavWriter(const std::string& filename, uint32_t sample_rate, uint16_t num_channels,
                     uint16_t bits_per_sample, uint32_t channel_mask)
    : file_(fopen(filename.c_str(), "wb")),
      sample_rate_(sample_rate),
      num_channels_(num_channels),
      bits_per_sample_(bits_per_sample),
      channel_mask_(channel_mask),
      samples_written_(0) {
    if (file_) {
        writeHeader();
    }
}

WavWriter::~WavWriter() {
    if (file_) {
        updateHeader();
        fclose(file_);
        file_ = nullptr;
    }
}

void WavWriter::writeHeader() {
    const bool extensible = channel_mask_ != 0;

    RIFFHeader riff{};
    memcpy(riff.chunk_id, "RIFF", 4);
    riff.chunk_size = 0;  // Will update later
    memcpy(riff.format, "WAVE", 4);

    FmtChunk fmt{};
    memcpy(fmt.chunk_id, "fmt ", 4);
    fmt.chunk_size = fmtBodySize();
    fmt.audio_format = extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
    fmt.num_channels = num_channels_;
    fmt.sample_rate = sample_rate_;
    fmt.byte_rate = sample_rate_ * num_channels_ * (bits_per_sample_ / 8);
    fmt.block_align = num_channels_ * (bits_per_sample_ / 8);
    fmt.bits_per_sample = bits_per_sample_;

    DataChunk data{};
    memcpy(data.chunk_id, "data", 4);
    data.chunk_size = 0;  // Will update later

    fwrite(&riff, sizeof(riff), 1, file_);
    fwrite(&fmt, sizeof(fmt), 1, file_);
    if (extensible) {
        FmtExtension ext{};
        ext.cb_size = FMT_EXTENSION_CB_SIZE;
        ext.valid_bits_per_sample = bits_per_sample_;
        ext.channel_mask = channel_mask_;
        memcpy(ext.sub_format, KSDATAFORMAT_SUBTYPE_PCM, sizeof(ext.sub_format));
        fwrite(&ext, sizeof(ext), 1, file_);
    }
    fwrite(&data, sizeof(data), 1, file_);
}

void WavWriter::updateHeader() {
    if (!file_) {
        return;
    }

    // The WAV container caps chunk sizes at 32 bits, so the 64-bit sample
    // count is intentionally truncated to the format's field width.
    uint32_t data_size =
        static_cast<uint32_t>(samples_written_ * num_channels_ * (bits_per_sample_ / 8));

    // Bytes the "fmt " chunk occupies, header included, plus the WAVE_FORMAT_
    // EXTENSIBLE tail when present. Derived from the structs so the offsets
    // below stay correct if a field is ever added.
    const size_t fmt_chunk_bytes =
        sizeof(FmtChunk) + (channel_mask_ != 0 ? sizeof(FmtExtension) : 0);

    // RIFF chunk size = total file size minus the leading "RIFF<size>" (8 bytes).
    uint32_t file_size = data_size + static_cast<uint32_t>(sizeof(RIFFHeader) - 8 +
                                                           fmt_chunk_bytes + sizeof(DataChunk));

    // Patching the sizes needs a seekable file. On a pipe or FIFO the seek
    // fails; bail before the matching fwrite so the size value is not appended
    // to the audio data. The header keeps the placeholder zeros writeHeader() wrote.
    if (fseek(file_, 4, SEEK_SET) != 0 || fwrite(&file_size, 4, 1, file_) != 1) {
        fprintf(stderr, "WavWriter: cannot patch RIFF size (non-seekable output?); "
                        "WAV header left with placeholder sizes\n");
        return;
    }

    // Data chunk size: skip the RIFF and fmt chunks, then the data chunk's id
    // to land on its 4-byte size field.
    const size_t data_size_offset =
        sizeof(RIFFHeader) + fmt_chunk_bytes + offsetof(DataChunk, chunk_size);
    if (fseek(file_, static_cast<long>(data_size_offset), SEEK_SET) != 0 ||
        fwrite(&data_size, 4, 1, file_) != 1) {
        fprintf(stderr, "WavWriter: cannot patch data-chunk size\n");
        return;
    }

    // Leave the file position at end of file.
    fseek(file_, 0, SEEK_END);
}

bool WavWriter::writeSamples(const int16_t* samples, size_t num_samples) {
    if (!file_ || !samples || num_samples == 0) {
        return false;
    }

    size_t bytes_to_write = num_samples * num_channels_ * sizeof(int16_t);
    size_t bytes_written = fwrite(samples, 1, bytes_to_write, file_);

    if (bytes_written == bytes_to_write) {
        samples_written_ += num_samples;
        return true;
    }

    return false;
}
