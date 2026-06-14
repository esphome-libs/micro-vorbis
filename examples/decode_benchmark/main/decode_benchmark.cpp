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

/* ESP32 Vorbis Decode Benchmark
 *
 * Continuously decodes Ogg Vorbis audio clips and reports timing statistics.
 *
 * Uses OggVorbisDecoder to demux and decode the audio streams.
 *
 * Demonstrates concurrent decoding with independent decoder instances by testing
 * 1-4 tasks for each audio type, with tasks pinned to alternating CPU cores.
 *
 * Each task uses its own OggVorbisDecoder instance. A single instance is not
 * thread-safe; concurrency comes from running separate instances, one per task.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_vorbis/ogg_vorbis_decoder.h"
#include "test_audio.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* const TAG = "DECODE_BENCH";

#ifndef DECODE_BENCH_MAX_CONCURRENT_TASKS
#define DECODE_BENCH_MAX_CONCURRENT_TASKS 4
#endif

static const int MAX_CONCURRENT_TASKS = DECODE_BENCH_MAX_CONCURRENT_TASKS;

// Audio test configurations
enum class AudioType : uint8_t { VORBIS };

struct AudioConfig {
    const char* name;
    const char* codec;
    const uint8_t* data;
    size_t size;
};

static const AudioConfig AUDIO_CONFIGS[] = {
    {"VORBIS", "Vorbis", test_vorbis_music_data, test_vorbis_music_data_size}};

static const int NUM_AUDIO_TYPES = sizeof(AUDIO_CONFIGS) / sizeof(AUDIO_CONFIGS[0]);

// Statistics structure for tracking timing data
struct Stats {
    int64_t min_us;
    int64_t max_us;
    int64_t sum_us;
    int64_t sum_sq_us;  // For standard deviation calculation
    size_t count;
    size_t total_samples;  // Total per-channel frames decoded (drives the audio-duration math)
};

// Results from a decode run
struct DecodeResult {
    Stats frame_stats;
    int64_t total_time_us;
    int64_t setup_time_us;   // Time to process 3 header packets
    int64_t decode_time_us;  // Time for audio decoding only
    uint32_t sample_rate;
    int core_id;
    bool success;
    bool footprint_valid;           // True only when measured alone (no concurrent tasks)
    size_t decoder_heap_bytes;      // Total free heap consumed by this decoder once fully allocated
    size_t decoder_internal_bytes;  // Of that, drawn from internal RAM
    size_t decoder_psram_bytes;     // Of that, drawn from PSRAM
};

// Task parameters
struct TaskParams {
    int task_id;
    DecodeResult* result;
    SemaphoreHandle_t done_semaphore;
    int pinned_core;                  // -1 for no pinning, 0 or 1 for specific core
    const AudioConfig* audio_config;  // Which audio to decode
    bool measure_footprint;           // Only true for single-task runs (see decode_full_file)
};

// Initialize statistics structure
static void init_stats(Stats* s) {
    s->min_us = INT64_MAX;
    s->max_us = 0;
    s->sum_us = 0;
    s->sum_sq_us = 0;
    s->count = 0;
    s->total_samples = 0;
}

// Update statistics with new timing value and sample count
static void update_stats(Stats* s, int64_t time_us, size_t samples) {
    if (time_us < s->min_us) {
        s->min_us = time_us;
    }

    if (time_us > s->max_us) {
        s->max_us = time_us;
    }
    s->sum_us += time_us;
    s->sum_sq_us += time_us * time_us;
    s->count++;
    s->total_samples += samples;
}

// Log statistics
static void log_stats(const char* prefix, const char* name, Stats* s) {
    if (s->count == 0) {
        ESP_LOGI(TAG, "%s%s: no data", prefix, name);
        return;
    }

    double avg = (double)s->sum_us / s->count;
    double variance = (double)s->sum_sq_us / s->count - avg * avg;
    double stddev = sqrt(variance);

    ESP_LOGI(TAG, "%s%s (us): min=%" PRId64 " max=%" PRId64 " avg=%.1f sd=%.1f (n=%zu)", prefix,
             name, s->min_us, s->max_us, avg, stddev, s->count);
}

// Decode the full test audio file and return results.
//
// measure_footprint must only be set when this is the sole running decode task.
// The footprint is a delta of esp_get_free_heap_size(), a single system-wide
// counter; with concurrent tasks the window would also capture their allocations,
// producing a meaningless, timing-dependent number. The decoder's true footprint
// is deterministic for a given stream, so a single-task measurement is sufficient.
static DecodeResult decode_full_file(const uint8_t* audio_data, size_t audio_size,
                                     bool measure_footprint) {
    DecodeResult result{};
    init_stats(&result.frame_stats);
    result.success = true;
    result.sample_rate = 0;
    result.setup_time_us = 0;
    result.decode_time_us = 0;
    result.core_id = xPortGetCoreID();
    result.footprint_valid = false;
    result.decoder_heap_bytes = 0;
    result.decoder_internal_bytes = 0;
    result.decoder_psram_bytes = 0;

    // Baseline free heap before the decoder allocates anything, split by capability
    // so we can see how the footprint (arena + PCM buffer) lands across internal RAM
    // vs PSRAM. The deltas against the post-init snapshots isolate this decoder's
    // draw from task-stack overhead.
    size_t internal_before = measure_footprint ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL) : 0;
    size_t psram_before = measure_footprint ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;

    // Create decoder
    micro_vorbis::OggVorbisDecoder decoder;

    // PCM output buffer - allocated once headers are parsed and we know the format
    std::vector<int16_t> pcm_buffer;

    // Input data pointers
    const uint8_t* input_ptr = audio_data;
    size_t input_remaining = audio_size;

    // Start timing
    int64_t iteration_start = esp_timer_get_time();

    // Decode loop
    while (input_remaining > 0) {
        size_t bytes_consumed = 0;
        size_t bytes_written = 0;

        // Time this decode call
        int64_t frame_start = esp_timer_get_time();

        micro_vorbis::OggVorbisResult decode_result = decoder.decode(
            input_ptr, input_remaining, reinterpret_cast<uint8_t*>(pcm_buffer.data()),
            pcm_buffer.size() * sizeof(int16_t), bytes_consumed, bytes_written);

        int64_t frame_time = esp_timer_get_time() - frame_start;

        // Advance the input before branching on the result. A buffer-too-small retry
        // re-enters decode() to flush pending PCM and reports bytes_consumed == 0, so
        // advancing here keeps the position correct instead of dropping the bytes the
        // failed call already consumed.
        input_ptr += bytes_consumed;
        input_remaining -= bytes_consumed;

        // Once all headers are parsed, record setup time and allocate PCM buffer sized to
        // the stream's long block size (a safe upper bound on output per decode() call).
        // pcm_buffer holds int16_t, so convert the byte bound to an element count.
        if (decode_result == micro_vorbis::OGG_VORBIS_DECODER_STREAM_INFO_READY) {
            result.setup_time_us = esp_timer_get_time() - iteration_start;
            pcm_buffer.resize(decoder.get_pcm_format().max_output_bytes() / sizeof(int16_t));

            // Decoder is now fully allocated (arena sized from codec setup, PCM
            // buffer resized). Capture the footprint while it is still alive, but
            // only when running solo so the global heap counters have no other writers.
            if (measure_footprint) {
                size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                result.decoder_internal_bytes =
                    internal_before > internal_after ? internal_before - internal_after : 0;
                result.decoder_psram_bytes =
                    psram_before > psram_after ? psram_before - psram_after : 0;
                result.decoder_heap_bytes =
                    result.decoder_internal_bytes + result.decoder_psram_bytes;
                result.footprint_valid = true;
            }
        }

        // Update statistics only when audio was decoded. bytes_written is the total
        // across all channels; the duration math wants per-channel frames, so convert
        // bytes to samples and divide by the channel count.
        if (bytes_written > 0) {
            update_stats(&result.frame_stats, frame_time,
                         bytes_written / sizeof(int16_t) / decoder.get_pcm_format().num_channels());
        }

        // Stream fully decoded
        if (decode_result == micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM) {
            break;
        }

        // Check for errors (informational codes are >= 0)
        if (decode_result < 0) {
            // Handle buffer too small by resizing and retrying
            if (decode_result == micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL) {
                size_t required_elements = decoder.get_required_output_bytes() / sizeof(int16_t);
                ESP_LOGI(TAG, "Resizing PCM buffer from %zu to %zu samples", pcm_buffer.size(),
                         required_elements);
                pcm_buffer.resize(required_elements);
                continue;  // Retry decode with larger buffer
            }
            result.success = false;
            break;
        }

        // Stall guard: bail only when the decoder made no progress. A buffer-too-small
        // retry returns bytes_consumed == 0 while flushing pending PCM (bytes_written > 0),
        // so require both to be zero before aborting.
        if (bytes_consumed == 0 && bytes_written == 0 && input_remaining > 0) {
            result.success = false;
            break;
        }

        // Yield to allow other tasks to run (important for concurrent decoding)
        taskYIELD();
    }

    result.total_time_us = esp_timer_get_time() - iteration_start;
    result.decode_time_us = result.total_time_us - result.setup_time_us;
    result.sample_rate = decoder.get_pcm_format().sample_rate();

    return result;
}

// Log decode results with optional prefix
static void log_decode_result(const char* prefix, DecodeResult* result) {
    if (!result->success) {
        ESP_LOGE(TAG, "%sDecode failed", prefix);
        return;
    }

    log_stats(prefix, "Frame", &result->frame_stats);

    // Calculate real-time factor
    double audio_duration_us =
        (double)result->frame_stats.total_samples / result->sample_rate * 1000000.0;
    double rtf = (double)result->total_time_us / audio_duration_us;

    double decode_rtf = (double)result->decode_time_us / audio_duration_us;

    ESP_LOGI(TAG,
             "%sTotal: %" PRId64 " ms (setup: %" PRId64 " ms, decode: %" PRId64
             " ms), %.1fs audio, RTF: %.3f (%.1fx), decode RTF: %.3f (%.1fx), core %d",
             prefix, result->total_time_us / 1000, result->setup_time_us / 1000,
             result->decode_time_us / 1000, audio_duration_us / 1000000.0, rtf, 1.0 / rtf,
             decode_rtf, 1.0 / decode_rtf, result->core_id);

    // Only logged for single-task runs; under concurrency the measurement is
    // unreliable (see decode_full_file) and is skipped entirely.
    if (result->footprint_valid) {
        ESP_LOGI(TAG,
                 "%sDecoder footprint: %zu bytes (internal: %zu, PSRAM: %zu) (arena + PCM buffer)",
                 prefix, result->decoder_heap_bytes, result->decoder_internal_bytes,
                 result->decoder_psram_bytes);
    }
}

// FreeRTOS task function for concurrent decoding
static void decode_task(void* params) {
    TaskParams* task_params = (TaskParams*)params;
    const AudioConfig* config = task_params->audio_config;

    ESP_LOGI(TAG, "Task %d starting %s decode...", task_params->task_id, config->name);

    // Decode the full file
    *task_params->result =
        decode_full_file(config->data, config->size, task_params->measure_footprint);

    ESP_LOGI(TAG, "Task %d finished (%" PRId64 " ms)", task_params->task_id,
             task_params->result->total_time_us / 1000);

    // Signal completion
    xSemaphoreGive(task_params->done_semaphore);

    // Delete this task
    vTaskDelete(NULL);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 Vorbis Decode Benchmark ===");
    ESP_LOGI(TAG, "Audio types: %d", NUM_AUDIO_TYPES);
    for (int a = 0; a < NUM_AUDIO_TYPES; a++) {
        ESP_LOGI(TAG, "  %s (%s): %zu bytes", AUDIO_CONFIGS[a].name, AUDIO_CONFIGS[a].codec,
                 AUDIO_CONFIGS[a].size);
    }
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free Internal: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Concurrent decode test: up to %d independent tasks", MAX_CONCURRENT_TASKS);

    // Create semaphore for task synchronization
    SemaphoreHandle_t done_semaphore = xSemaphoreCreateCounting(MAX_CONCURRENT_TASKS, 0);
    if (done_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    uint32_t iteration = 0;

    while (true) {
        iteration++;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "=== Iteration %lu ===", iteration);

        // Track times for each audio type and task count
        int64_t times[NUM_AUDIO_TYPES][MAX_CONCURRENT_TASKS] = {{0}};
        bool all_success = true;

        // Interleave audio types: for each task count, test each audio type
        for (int num_tasks = 1; num_tasks <= MAX_CONCURRENT_TASKS; num_tasks++) {
            for (int audio_idx = 0; audio_idx < NUM_AUDIO_TYPES; audio_idx++) {
                const AudioConfig* config = &AUDIO_CONFIGS[audio_idx];

                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "--- %s (%s) - %d concurrent task%s ---", config->name, config->codec,
                         num_tasks, num_tasks == 1 ? "" : "s");

                DecodeResult results[MAX_CONCURRENT_TASKS];
                TaskParams params[MAX_CONCURRENT_TASKS];

                // Set up task parameters
                for (int i = 0; i < num_tasks; i++) {
                    params[i].task_id = i;
                    params[i].result = &results[i];
                    params[i].done_semaphore = done_semaphore;
                    params[i].pinned_core = i % 2;
                    params[i].audio_config = config;
                    // Only trust the heap-diff footprint when this decoder runs
                    // alone; concurrent tasks share the global free-heap counter.
                    params[i].measure_footprint = (num_tasks == 1);
                }

                int64_t start_time = esp_timer_get_time();

                // Create all tasks pinned to alternating cores
                int tasks_created = 0;
                for (int i = 0; i < num_tasks; i++) {
                    char task_name[16];
                    snprintf(task_name, sizeof(task_name), "decode_%d", i);

                    BaseType_t ret =
                        xTaskCreatePinnedToCore(decode_task, task_name, 32768, &params[i],
                                                1,  // Priority
                                                NULL,
                                                i % 2  // Core ID: alternates 0, 1, 0, 1
                        );

                    if (ret == pdPASS) {
                        tasks_created++;
                    } else {
                        ESP_LOGE(TAG, "Failed to create task %d", i);
                    }
                }

                // Wait for all successfully created tasks to complete
                for (int i = 0; i < tasks_created; i++) {
                    xSemaphoreTake(done_semaphore, portMAX_DELAY);
                }

                times[audio_idx][num_tasks - 1] = esp_timer_get_time() - start_time;

                // Log per-task results
                for (int i = 0; i < num_tasks; i++) {
                    char prefix[16];
                    snprintf(prefix, sizeof(prefix), "Task %d: ", i);
                    log_decode_result(prefix, &results[i]);
                    all_success = all_success && results[i].success;
                }
            }
        }

        // --- Summary ---
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "--- Summary ---");
        for (int audio_idx = 0; audio_idx < NUM_AUDIO_TYPES; audio_idx++) {
            const AudioConfig* config = &AUDIO_CONFIGS[audio_idx];
            ESP_LOGI(TAG, "%s (%s):", config->name, config->codec);
            for (int i = 0; i < MAX_CONCURRENT_TASKS; i++) {
                ESP_LOGI(TAG, "  %d task%s  %6" PRId64 " ms", i + 1,
                         i == 0 ? ": " : "s:", times[audio_idx][i] / 1000);
            }
        }
        ESP_LOGI(TAG, "All decodes successful: %s", all_success ? "YES" : "NO");
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

        // Low-water marks recorded automatically since boot. These survive task
        // teardown, so they capture the trough during the peak-concurrency phase
        // (4 decoders + PCM buffers + task stacks alive at once) even though every
        // task has since exited and freed its memory.
        ESP_LOGI(TAG, "Min free heap ever:     %lu bytes", esp_get_minimum_free_heap_size());
        ESP_LOGI(TAG, "Min free internal ever: %zu bytes",
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Min free PSRAM ever:    %zu bytes",
                 heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "---");

        // Small delay between iterations
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
