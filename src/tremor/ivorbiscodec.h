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

#ifndef _vorbis_codec_h_
#define _vorbis_codec_h_

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <ogg/ogg.h>

/* microVorbis: per-channel "keep" mask for the channel-selection optimization.
   A packed bitmask of VORBIS_KEEP_BYTES(channels) bytes, LSB-first: bit ch set
   means channel ch is decoded/kept. This replaces a one-int-per-channel array
   (and a 1 KB caller-side scratch buffer); a 255-channel mask is 32 bytes. The
   casts keep the helpers warning-clean under -Wconversion in the C++ wrapper. */
#define VORBIS_KEEP_BYTES(channels) (((channels)+7)>>3)
static inline int vorbis_keep_get(const unsigned char *m,int ch){
  return (m[ch>>3]>>(ch&7))&1;
}
static inline void vorbis_keep_set(unsigned char *m,int ch){
  m[ch>>3]=(unsigned char)(m[ch>>3]|(1u<<(ch&7)));
}

typedef struct vorbis_info{
  int version;
  int channels;
  long rate;

  /* The below bitrate declarations are *hints*.
     Combinations of the three values carry the following implications:
     
     all three set to the same value: 
       implies a fixed rate bitstream
     only nominal set: 
       implies a VBR stream that averages the nominal bitrate.  No hard 
       upper/lower limit
     upper and or lower set: 
       implies a VBR bitstream that obeys the bitrate limits. nominal 
       may also be set to give a nominal rate.
     none set:
       the coder does not care to speculate.
  */

  long bitrate_upper;
  long bitrate_nominal;
  long bitrate_lower;
  long bitrate_window;

  /* microVorbis: set once the comment header has been validated/skipped, so
     vorbis_synthesis_headerin can enforce the id -> comment -> setup ordering
     without a separate vorbis_comment object (which the fork removed). */
  int comment_header_seen;

  void *codec_setup;
} vorbis_info;

/* vorbis_dsp_state buffers the current vorbis audio
   analysis/synthesis state.  The DSP state belongs to a specific
   logical bitstream ****************************************************/
typedef struct vorbis_dsp_state{
  int analysisp;
  vorbis_info *vi;

  ogg_int32_t **pcm;
  ogg_int32_t **pcmret;
  int      pcm_storage;
  int      pcm_current;
  int      pcm_returned;

  int  preextrapolate;
  int  eofflag;

  long lW;
  long W;
  long nW;
  long centerW;

  ogg_int64_t granulepos;
  ogg_int64_t sequence;

  void       *backend_state;

  /* microVorbis: per-channel decode mask for the channel-selection
     optimization. A VORBIS_KEEP_BYTES(vi->channels) bitmask (see vorbis_keep_*
     above); a clear bit skips that channel's floor-apply / iMDCT / window
     (mapping0_inverse) and overlap-add (vorbis_synthesis_blockin). The entropy
     decode + channel coupling always run for every channel, so kept channels
     stay bit-exact. Fixed at init: defaults to all-set, or to the mask passed to
     vorbis_synthesis_init_ex (dropped channels are never allocated). It is
     immutable for the stream (fixed once via vorbis_synthesis_init_ex) because
     it gates arena-owned history buffers. */
  unsigned char *channel_keep;

  /* microVorbis: single pre-sized arena holding every DSP-setup allocation
     (private_state, the pcm/pcmret/channel_keep pointer arrays, each kept
     channel's PCM history buffer, b->mode, and all mode/floor/residue lookups).
     Sized once by _vorbis_dsp_arena_compute_size() in block.c and filled by bump
     allocation during _vds_init, collapsing dozens-to-thousands of small mallocs
     into one. This is the entire DSP heap footprint: vorbis_dsp_clear releases
     it in a single free. */
  void  *setup_arena_data;      /* Pre-allocated arena buffer */
  long   setup_arena_capacity;  /* Total arena size in bytes */
  long   setup_arena_used;      /* Current bump pointer offset */
} vorbis_dsp_state;

typedef struct vorbis_block{
  /* necessary stream state for linking to the framing abstraction */
  ogg_int32_t  **pcm;       /* this is a pointer into local storage */ 
  oggpack_buffer opb;
  
  long  lW;
  long  W;
  long  nW;
  int   pcmend;
  int   mode;

  int         eofflag;
  ogg_int64_t granulepos;
  ogg_int64_t sequence;
  vorbis_dsp_state *vd; /* For read-only access of configuration */

  /* Pre-allocated arena for per-packet decode allocations */
  void  *arena_data;      /* Pre-allocated arena buffer */
  long   arena_capacity;  /* Total arena size in bytes */
  long   arena_used;      /* Current bump pointer offset */

} vorbis_block;

/* vorbis_info contains all the setup information specific to the
   specific compression/decompression mode in progress (eg,
   psychoacoustic settings, channel setup, options, codebook
   etc). vorbis_info and substructures are in backends.h.
*********************************************************************/

/* microVorbis: the upstream vorbis_comment struct (and its init/clear/query
   API) was removed. The decoder discards comments; vorbis_synthesis_headerin
   tracks "comment header seen" via vorbis_info::comment_header_seen instead. */

/* libvorbis encodes in two abstraction layers; first we perform DSP
   and produce a packet (see docs/analysis.txt).  The packet is then
   coded into a framed OggSquish bitstream by the second layer (see
   docs/framing.txt).  Decode is the reverse process; we sync/frame
   the bitstream and extract individual packets, then decode the
   packet back into PCM audio.

   The extra framing/packetizing is used in streaming formats, such as
   files.  Over the net (such as with UDP), the framing and
   packetization aren't necessary as they're provided by the transport
   and the streaming layer is not used */

/* Vorbis PRIMITIVES: general ***************************************/

extern void     vorbis_info_init(vorbis_info *vi);
extern void     vorbis_info_clear(vorbis_info *vi);

extern int      vorbis_block_init(vorbis_dsp_state *v, vorbis_block *vb);
extern int      vorbis_block_clear(vorbis_block *vb);
extern void     vorbis_dsp_clear(vorbis_dsp_state *v);

/* Vorbis PRIMITIVES: synthesis layer *******************************/
extern int      vorbis_synthesis_headerin(vorbis_info *vi,ogg_packet *op);

/* microVorbis: initialize synthesis state, with `keep` selecting which channels
   to allocate and decode. It is a VORBIS_KEEP_BYTES(n) bitmask (n == vi->channels;
   see vorbis_keep_*); a clear bit drops that channel, so its PCM history buffer
   costs nothing. The mask is fixed here and immutable for the stream (it gates
   arena-owned buffers). Pass keep=NULL (any n) to keep every channel. This is the
   fork's only synthesis-init entry point; upstream's plain vorbis_synthesis_init()
   was removed. */
extern int      vorbis_synthesis_init_ex(vorbis_dsp_state *v,vorbis_info *vi,
                                         const unsigned char *keep,int n);
extern int      vorbis_synthesis_restart(vorbis_dsp_state *v);
extern int      vorbis_synthesis(vorbis_block *vb,ogg_packet *op);
extern int      vorbis_synthesis_trackonly(vorbis_block *vb,ogg_packet *op);
extern int      vorbis_synthesis_blockin(vorbis_dsp_state *v,vorbis_block *vb);
extern int      vorbis_synthesis_pcmout(vorbis_dsp_state *v,ogg_int32_t ***pcm);
extern int      vorbis_synthesis_read(vorbis_dsp_state *v,int samples);
extern long     vorbis_packet_blocksize(vorbis_info *vi,ogg_packet *op);

/* Vorbis ERRORS and return codes ***********************************/

#define OV_FALSE      -1  
#define OV_EOF        -2
#define OV_HOLE       -3

#define OV_EREAD      -128
#define OV_EFAULT     -129
#define OV_EIMPL      -130
#define OV_EINVAL     -131
#define OV_ENOTVORBIS -132
#define OV_EBADHEADER -133
#define OV_EVERSION   -134
#define OV_ENOTAUDIO  -135
#define OV_EBADPACKET -136
#define OV_EBADLINK   -137
#define OV_ENOSEEK    -138

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif

