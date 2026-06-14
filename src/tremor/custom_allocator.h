/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF microVorbis.                                *
 *                                                                  *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS SOURCE IS GOVERNED    *
 * BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE IN       *
 * 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.          *
 *                                                                  *
 * Copyright (c) 2026 Kevin Ahrendt                                 *
 *                                                                  *
 ********************************************************************

 function: Override ogg allocation macros with ESP32 PSRAM-aware versions.

 Included from os.h after ogg/os_types.h defines the default _ogg_malloc
 etc. macros. On ESP32, undefines them and replaces with heap_caps-based
 versions controlled by Kconfig memory placement options. On host builds,
 this header is a no-op and the standard malloc/free macros remain.

 ********************************************************************/

#ifndef _CUSTOM_ALLOCATOR_H
#define _CUSTOM_ALLOCATOR_H

#ifdef ESP_PLATFORM
/* ESP-IDF build: Use PSRAM-aware allocation */
#include "esp_heap_caps.h"
#include <string.h> /* for memset */

/* Undefine the standard allocation macros from ogg/os_types.h */
#undef _ogg_malloc
#undef _ogg_calloc
#undef _ogg_realloc
#undef _ogg_free

/* Provide PSRAM-aware replacements as static inline functions.
 * Memory placement is controlled by Kconfig MICRO_VORBIS_STATE_MEMORY_PREFERENCE. */

static inline void* _vorbis_malloc(size_t size) {
#if defined(CONFIG_MICRO_VORBIS_STATE_PREFER_PSRAM)
    /* Try PSRAM first, fall back to internal RAM */
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL)
    /* Try internal RAM first, fall back to PSRAM */
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_PSRAM_ONLY)
    /* PSRAM only - fail if unavailable */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_INTERNAL_ONLY)
    /* Internal RAM only */
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    /* Default: prefer PSRAM, fall back to internal */
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static inline void* _vorbis_calloc(size_t count, size_t size) {
#if defined(CONFIG_MICRO_VORBIS_STATE_PSRAM_ONLY)
    /* PSRAM only - heap_caps_calloc available for strict modes */
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_INTERNAL_ONLY)
    /* Internal RAM only */
    return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL)
    /* Try internal RAM first, fall back to PSRAM */
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    /* Default: prefer PSRAM, fall back to internal */
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static inline void* _vorbis_realloc(void* ptr, size_t size) {
#if defined(CONFIG_MICRO_VORBIS_STATE_PREFER_PSRAM)
    /* Try PSRAM first, fall back to internal RAM */
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_PREFER_INTERNAL)
    /* Try internal RAM first, fall back to PSRAM */
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_PSRAM_ONLY)
    /* PSRAM only */
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_STATE_INTERNAL_ONLY)
    /* Internal RAM only */
    return heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    /* Default: prefer PSRAM, fall back to internal */
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static inline void _vorbis_free(void* ptr) {
    heap_caps_free(ptr);
}

/* Codebook-specific allocation: controlled by MICRO_VORBIS_CODEBOOK_MEMORY_PREFERENCE.
 * Codebook lookup tables are accessed with random-access patterns in the decode
 * hot path, so they benefit significantly from faster internal RAM. */

static inline void* _vorbis_codebook_malloc(size_t size) {
#if defined(CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_INTERNAL)
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_PSRAM)
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_CODEBOOK_PSRAM_ONLY)
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_CODEBOOK_INTERNAL_ONLY)
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    /* Default: prefer internal RAM */
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static inline void* _vorbis_codebook_calloc(size_t count, size_t size) {
#if defined(CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_INTERNAL)
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_CODEBOOK_PREFER_PSRAM)
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_CODEBOOK_PSRAM_ONLY)
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(CONFIG_MICRO_VORBIS_CODEBOOK_INTERNAL_ONLY)
    return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    /* Default: prefer internal RAM */
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

/* Redefine _ogg_* macros to use our PSRAM-aware functions */
#define _ogg_malloc _vorbis_malloc
#define _ogg_calloc _vorbis_calloc
#define _ogg_realloc _vorbis_realloc
#define _ogg_free _vorbis_free
#define _ogg_codebook_malloc _vorbis_codebook_malloc
#define _ogg_codebook_calloc _vorbis_codebook_calloc

#endif /* ESP_PLATFORM */

#endif /* _CUSTOM_ALLOCATOR_H */
