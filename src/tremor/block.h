/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis 'TREMOR' CODEC SOURCE CODE.   *
 *                                                                  *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis 'TREMOR' SOURCE CODE IS (C) COPYRIGHT 1994-2008    *
 * BY THE Xiph.Org FOUNDATION http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: arena-based block allocation (replaces chain allocator)

 ********************************************************************/

/********************************************************************
 *                                                                  *
 * MODIFIED 2026 BY KEVIN AHRENDT FOR microVorbis.                  *
 *                                                                  *
 * MODIFICATIONS ARE LICENSED UNDER THE SAME BSD-3-CLAUSE TERMS     *
 * AS THE ORIGINAL; SEE 'COPYING'. SEE 'src/tremor/CHANGES.md'      *
 * FOR DETAILS OF THE CHANGES.                                      *
 *                                                                  *
 ********************************************************************/

#ifndef _V_BLOCK_
#define _V_BLOCK_

#include <string.h>
#include "ivorbiscodec.h"

#define ARENA_ALIGN 8

/* Round a byte count up to the arena alignment. Shared by the per-packet block
   arena, the DSP setup arena, and the size-computation helpers so allocation
   and sizing always agree. */
static inline long _vorbis_arena_round(long bytes) {
    return (bytes + (ARENA_ALIGN - 1)) & ~(long)(ARENA_ALIGN - 1);
}

static inline void _vorbis_block_ripcord(vorbis_block *vb) {
    vb->arena_used = 0;
}

static inline void *_vorbis_block_alloc(vorbis_block *vb, long bytes) {
    void *ret;
    bytes = _vorbis_arena_round(bytes);
    if (vb->arena_used + bytes > vb->arena_capacity) {
        return (void *)0;
    }
    ret = (void *)(((char *)vb->arena_data) + vb->arena_used);
    vb->arena_used += bytes;
    return ret;
}

/* Arena allocation macro for decode-path temporaries (replaces alloca) */
#define ARENA_STACK(type, var, size, vb) \
    type *var = ((type *)_vorbis_block_alloc((vb), sizeof(type) * (size)))

/* DSP setup arena: bump allocator for the once-per-stream lookups built during
   _vds_init. Returns NULL on overflow; sizing via _vorbis_dsp_arena_compute_size
   (block.c) guarantees this never happens for a correctly-sized arena. */
static inline void *_vorbis_setup_alloc(vorbis_dsp_state *v, long bytes) {
    void *ret;
    bytes = _vorbis_arena_round(bytes);
    if (v->setup_arena_used + bytes > v->setup_arena_capacity) {
        return (void *)0;
    }
    ret = (void *)(((char *)v->setup_arena_data) + v->setup_arena_used);
    v->setup_arena_used += bytes;
    return ret;
}

/* Zeroing variant matching _ogg_calloc semantics (n * size, memset to 0). */
static inline void *_vorbis_setup_calloc(vorbis_dsp_state *v, long n, long size) {
    long bytes = n * size;
    void *ret = _vorbis_setup_alloc(v, bytes);
    if (ret) memset(ret, 0, (size_t)bytes);
    return ret;
}

#endif
