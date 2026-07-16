/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis 'TREMOR' CODEC SOURCE CODE.   *
 *                                                                  *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis 'TREMOR' SOURCE CODE IS (C) COPYRIGHT 1994-2002    *
 * BY THE Xiph.Org FOUNDATION http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: modified discrete cosine transform prototypes

 ********************************************************************/

#ifndef _OGG_mdct_H_
#define _OGG_mdct_H_

#include "ivorbiscodec.h"
#include "misc.h"

#define DATA_TYPE ogg_int32_t
#define REG_TYPE  register ogg_int32_t

#ifdef _LOW_ACCURACY_
#define cPI3_8 (0x0062)
#define cPI2_8 (0x00b5)
#define cPI1_8 (0x00ed)
#else
#define cPI3_8 (0x30fbc54d)
#define cPI2_8 (0x5a82799a)
#define cPI1_8 (0x7641af3d)
#endif

extern void mdct_forward(int n, DATA_TYPE *in, DATA_TYPE *out);

/* master-shaped (full-block) backward MDCT: transforms n values from
   in into n values in out. Superseded by the lowmem functions below;
   removed once mapping0.c no longer calls it. */
extern void mdct_backward_full(int n, DATA_TYPE *in, DATA_TYPE *out);

/* lowmem half-block backward MDCT: transforms n/2 values in in, in
   place. Does not perform the final deinterleave/expansion; see
   mdct_unroll_lap(). */
extern void mdct_backward(int n, DATA_TYPE *in);

/* saves the n/4 odd-indexed values of in (post mdct_backward) into
   right, for use as the overlap tail by the next block's
   mdct_unroll_lap() call */
extern void mdct_shift_right(int n, DATA_TYPE *in, DATA_TYPE *right);

/* reconstructs, windows, and overlap-adds the current block's half-
   transform (in, right) against the previous block's tail, writing
   samples [start,end) of the current frame to out at stride step.
   lW/W select the previous/current block's window per side; w0/w1
   are the short/long window tables. Emits raw s7.24 fixed-point
   samples; the caller is responsible for rounding and clipping to
   the output sample width. */
extern void mdct_unroll_lap(int n0, int n1,
                             int lW, int W,
                             DATA_TYPE *in,
                             DATA_TYPE *right,
                             const LOOKUP_T *w0,
                             const LOOKUP_T *w1,
                             ogg_int32_t *out,
                             int step,
                             int start,
                             int end);

#endif












