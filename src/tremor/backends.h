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

 function: backend and mapping structures

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

/* this is exposed up here because we need it for static modes.
   Lookups for each backend aren't exposed because there's no reason
   to do so */

#ifndef _vorbis_backend_h_
#define _vorbis_backend_h_

#include "codec_internal.h"

/* this would all be simpler/shorter with templates, but.... */
/* Transform backend generic *************************************/

/* only mdct right now.  Flesh it out more if we ever transcend mdct
   in the transform domain */

/* Floor backend generic *****************************************/
typedef struct{
  vorbis_info_floor     *(*unpack)(vorbis_info *,oggpack_buffer *);
  vorbis_look_floor     *(*look)  (vorbis_dsp_state *,vorbis_info_mode *,
				   vorbis_info_floor *);
  void (*free_info) (vorbis_info_floor *);
  void (*free_look) (vorbis_look_floor *);
  void *(*inverse1)  (struct vorbis_block *,vorbis_look_floor *);
  int   (*inverse2)  (struct vorbis_block *,vorbis_look_floor *,
		     void *buffer,ogg_int32_t *);
  /* microVorbis: bytes look() will bump from the DSP setup arena, given the
     same args. Mirrors look() and lives beside it so the two cannot drift. */
  long  (*arena_size)(vorbis_dsp_state *,vorbis_info_mode *,
		     vorbis_info_floor *);
} vorbis_func_floor;

typedef struct{
  int   order;
  long  rate;
  long  barkmap;

  int   ampbits;
  int   ampdB;

  int   numbooks; /* <= 16 */
  int   books[16];

} vorbis_info_floor0;

/* microVorbis: partitionclass, the class_ arrays and postlist were fixed
   spec-max arrays (31/16/16x8/65 ints); they are heap-allocated to the
   parsed sizes in floor1_unpack, and these are the hard caps the parsed
   counts are bounded by (partitions: 5-bit read; classes: 4-bit reads;
   posts: count>VIF_POSIT rejected). */
#define VIF_POSIT 63
#define VIF_CLASS 16
#define VIF_PARTS 31
typedef struct{
  int   partitions;      /* 0 to 31 */
  int  *partitionclass;  /* [partitions]; 0 to 15 */

  int  *class_dim;       /* [maxclass+1]; 1 to 8 */
  int  *class_subs;      /* [maxclass+1]; 0,1,2,3 (bits: 1<<n poss) */
  int  *class_book;      /* [maxclass+1]; subs ^ dim entries */
  int  *class_subbook;   /* [maxclass+1][8] flat, row stride 8 */

  int   mult;            /* 1 2 3 or 4 */
  int  *postlist;        /* [count+2]; first two implicit */

} vorbis_info_floor1;

/* Residue backend generic *****************************************/
typedef struct{
  vorbis_info_residue *(*unpack)(vorbis_info *,oggpack_buffer *);
  vorbis_look_residue *(*look)  (vorbis_dsp_state *,vorbis_info_mode *,
				 vorbis_info_residue *);
  void (*free_info)    (vorbis_info_residue *);
  void (*free_look)    (vorbis_look_residue *);
  int  (*inverse)      (struct vorbis_block *,vorbis_look_residue *,
			ogg_int32_t **,int *,int);
  /* microVorbis: see vorbis_func_floor::arena_size */
  long (*arena_size)   (vorbis_dsp_state *,vorbis_info_mode *,
			vorbis_info_residue *);
} vorbis_func_residue;

/* microVorbis: secondstages/booklist were fixed spec-max arrays (64/512);
   they are heap-allocated to the parsed sizes in res0_unpack, and these are
   the hard caps the parsed counts are validated against before allocating. */
#define VIR_PARTS 64          /* partitions: 6-bit read +1 */
#define VIR_BOOKS (VIR_PARTS*8) /* booklist: <=8 cascade bits per partition */
typedef struct vorbis_info_residue0{
/* block-partitioned VQ coded straight residue */
  long  begin;
  long  end;

  /* first stage (lossless partitioning) */
  int    grouping;         /* group n vectors per partition */
  int    partitions;       /* possible codebooks for a partition */
  int    partvals;         /* partitions ^ groupbook dim */
  int    groupbook;        /* huffbook for partitioning */
  int   *secondstages;     /* [partitions] expanded out to pointers in lookup */
  int   *booklist;         /* [acc] list of second stage books */

  /* microVorbis: decode-time precompute, built in res0_unpack from
     secondstages/booklist above (see src/tremor/CHANGES.md). Replaces
     vorbis_look_residue0's partbooks - this is the only lookup residue
     decode needs, so res0_look now just hands back this struct. */
  unsigned char *stagemasks; /* [partitions]; bit s set iff partition p has a stage-s book */
  unsigned char *stagebooks; /* [partitions*8], index (p<<3)+s; valid iff stagemasks[p]&(1<<s) */
  int    stages;            /* max stage count across all partitions */
} vorbis_info_residue0;

/* Mapping backend generic *****************************************/
typedef struct{
  vorbis_info_mapping *(*unpack)(vorbis_info *,oggpack_buffer *);
  vorbis_look_mapping *(*look)  (vorbis_dsp_state *,vorbis_info_mode *,
				 vorbis_info_mapping *);
  void (*free_info)    (vorbis_info_mapping *);
  void (*free_look)    (vorbis_look_mapping *);
  int  (*inverse)      (struct vorbis_block *vb,vorbis_look_mapping *);
  /* microVorbis: see vorbis_func_floor::arena_size */
  long (*arena_size)   (vorbis_dsp_state *,vorbis_info_mode *,
			vorbis_info_mapping *);
} vorbis_func_mapping;

/* microVorbis: chmuxlist/coupling_mag/coupling_ang were fixed spec-max
   arrays (256 each); they are heap-allocated to the parsed sizes in
   mapping0_unpack, and these are the hard caps the parsed counts are
   validated against before allocating. */
#define VIM_CHANNELS 256      /* channels: 8-bit read in the ID header */
#define VIM_COUPLES 256       /* coupling_steps: 8-bit read +1 */
typedef struct vorbis_info_mapping0{
  int   submaps;  /* <= 16 */
  int  *chmuxlist;         /* [channels]; up to 256 channels in a Vorbis stream */

  int   floorsubmap[16];   /* [mux] submap to floors */
  int   residuesubmap[16]; /* [mux] submap to residue */

  int   psy[2]; /* by blocktype; impulse/padding for short,
                   transition/normal for long */

  int   coupling_steps;    /* <= 256 */
  int  *coupling_mag;      /* [coupling_steps] */
  int  *coupling_ang;      /* [coupling_steps] */
} vorbis_info_mapping0;

#endif





