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

 function: libvorbis codec headers

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

#ifndef _V_CODECI_H_
#define _V_CODECI_H_

#include "codebook.h"

#define VI_SETUP_MAX 64  /* modes/maps/times/floors/residues: 6-bit read +1 */

typedef void vorbis_look_mapping;
typedef void vorbis_look_floor;
typedef void vorbis_look_residue;
typedef void vorbis_look_transform;

/* mode ************************************************************/
typedef struct {
  int blockflag;
  int windowtype;
  int transformtype;
  int mapping;
} vorbis_info_mode;

typedef void vorbis_info_floor;
typedef void vorbis_info_residue;
typedef void vorbis_info_mapping;

typedef struct private_state {
  /* local lookup storage */
  const void             *window[2];

  /* backend lookups are tied to the mode, not the backend or naked mapping */
  int                     modebits;
  vorbis_look_mapping   **mode;

  ogg_int64_t sample_count;

} private_state;

/* codec_setup_info contains all the setup information specific to the
   specific compression/decompression mode in progress (eg,
   psychoacoustic settings, channel setup, options, codebook
   etc).  
*********************************************************************/

typedef struct codec_setup_info {

  /* Vorbis supports only short and long blocks, but allows the
     encoder to choose the sizes */

  long blocksizes[2];

  /* modes are the primary means of supporting on-the-fly different
     blocksizes, different channel mappings (LR or M/A),
     different residue backends, etc.  Each mode consists of a
     blocksize flag and a mapping (along with the mapping setup */

  int        modes;
  int        maps;
  int        times;
  int        floors;
  int        residues;
  int        books;

  /* microVorbis: these were fixed [64] tables (the 6-bit-count spec max);
     they are now heap-allocated by _vorbis_unpack_books to the parsed
     counts above. Each count is set only after its table(s) are allocated,
     so vorbis_info_clear's count-bounded loops never index a NULL table.
     VI_SETUP_MAX caps every allocation at the old fixed size. */
  vorbis_info_mode      **mode_param;     /* [modes] */
  int                    *map_type;       /* [maps] */
  vorbis_info_mapping   **map_param;      /* [maps] */
  int                    *time_type;      /* [times] */
  int                    *floor_type;     /* [floors] */
  vorbis_info_floor     **floor_param;    /* [floors] */
  int                    *residue_type;   /* [residues] */
  vorbis_info_residue   **residue_param;  /* [residues] */
  codebook               *book_param;

  int    passlimit[32];     /* iteration limit per couple/quant pass */
  int    coupling_passes;
} codec_setup_info;

#endif
