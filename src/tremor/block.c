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

 function: PCM data vector blocking, windowing and dis/reassembly

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

#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include "ivorbiscodec.h"
#include "codec_internal.h"

#include "window.h"
#include "registry.h"
#include "misc.h"
#include "backends.h"
#include "block.h"
#include "mdct.h"

static int ilog(unsigned int v){
  int ret=0;
  if(v)--v;
  while(v){
    ret++;
    v>>=1;
  }
  return(ret);
}

/* Synthesis buffering: mapping0_inverse writes each channel's floor-applied
   spectrum into vd->work[i] and mdct_backward transforms it in place as a
   half block; the previous block's overlap tail sits in vd->mdctright[i].
   vorbis_synthesis_blockin only opens the out_begin/out_end readout window;
   mdct_unroll_lap windows and overlap-adds on demand in
   vorbis_synthesis_lapout. */

/* block abstraction setup *********************************************/

/* Compute worst-case per-packet arena size from codec_setup_info.
   All values are known after headers are parsed and codebooks initialized.
   Uses blocksizes[1] (long block) as worst case throughout. */
static long _vorbis_arena_compute_size(vorbis_info *vi){
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  int channels=vi->channels;
  long n=ci->blocksizes[1];
  long size=0;
  int i;

  /* Floor allocations (worst case across all floor types) */
  {
    long max_floor=0;
    for(i=0;i<ci->floors;i++){
      long floor_size=0;
      if(ci->floor_type[i]==1){
        /* floor1: fit_value array, max VIF_POSIT+2 posts */
        floor_size=(VIF_POSIT+2) * (long)sizeof(int);
      }else{
        /* floor0: lsp array of (order+1) int32_t */
        vorbis_info_floor0 *f0=(vorbis_info_floor0 *)ci->floor_param[i];
        floor_size=(f0->order+1) * (long)sizeof(ogg_int32_t);
      }
      if(floor_size>max_floor) max_floor=floor_size;
    }
    /* One floor allocation per channel */
    size += channels * max_floor;
  }

  /* Residue partword allocations: res012.c (lowmem classword decode, see
     src/tremor/CHANGES.md)
     _01inverse (types 0,1): ch * sizeof(uchar*) + ch * (partwords*dim) * sizeof(uchar)
     res2_inverse (type 2):  (partwords*dim) * sizeof(uchar)
     partword is now a flat per-channel byte array of partwords*partitions_per_word
     (== dim) class indices, not an array of partwords pointers into a
     setup-arena decodemap, so the per-word cost scales with dim (bytes), not
     with a fixed pointer size. _01inverse caps `end` at pcmend/2; res2_inverse
     caps at pcmend*ch/2, so res2 can have up to `ch` times the partitions of
     res0/res1. Track the per-type worst case separately. */
  {
    long max_bytes_01=0;
    long max_bytes_2=0;
    for(i=0;i<ci->residues;i++){
      vorbis_info_residue0 *ri=(vorbis_info_residue0 *)ci->residue_param[i];
      long end_cap_01=n/2;
      long end_cap_2=(n*(long)channels)/2;
      codebook *phrasebook=ci->book_param+ri->groupbook;
      long dim=phrasebook->dim;

      long end01=ri->end < end_cap_01 ? ri->end : end_cap_01;
      long rn01=end01 - ri->begin;
      if(rn01>0){
        long partvals=rn01/ri->grouping;
        long pw=(partvals+dim-1)/dim;
        long bytes=pw*dim; /* one unsigned char per partition index */
        if(bytes>max_bytes_01) max_bytes_01=bytes;
      }

      long end2=ri->end < end_cap_2 ? ri->end : end_cap_2;
      long rn2=end2 - ri->begin;
      if(rn2>0){
        long partvals=rn2/ri->grouping;
        long pw=(partvals+dim-1)/dim;
        long bytes=pw*dim;
        if(bytes>max_bytes_2) max_bytes_2=bytes;
      }
    }
    /* _01inverse: outer pointer array + per-ch inner byte arrays. Its `end` cap
       is pcmend/2 (channel-independent) and it allocates `ch` inner arrays per
       submap, so summed over all submaps the inner arrays total
       channels*max_bytes_01 (the submaps partition the channels) - already
       covered above. */
    size += channels * (long)sizeof(unsigned char *);
    size += channels * max_bytes_01;
    /* res2: one partword byte array per submap. mapping0_inverse calls the
       residue inverse once per submap (mapping0.c) and res2_inverse allocates
       its partword array unconditionally (res012.c); the block arena is reset
       only between packets, so every submap's array is live at once. Unlike
       res0/1, res2's array size is channel-independent when info->end is the
       binding cap (each submap then allocates the full max_bytes_2, not a
       ch-scaled share), so a single reservation undercounts by the submap
       count. A submap must carry >=1 channel to allocate (ch==0 -> n<=0 -> no
       alloc) and the submaps partition the channels, so at most
       min(channels,submaps) res2 arrays coexist; submaps is a 4-bit field
       (<=16). Reserve that many. */
    {
      long max_res2_submaps = channels < 16 ? channels : 16;
      size += max_res2_submaps * max_bytes_2;
    }
  }

  /* mapping0 ARENA_STACK allocations (4 arrays of channels pointers/ints) */
  size += channels * (long)sizeof(ogg_int32_t *); /* pcmbundle */
  size += channels * (long)sizeof(int);           /* zerobundle */
  size += channels * (long)sizeof(int);           /* nonzero */
  size += channels * (long)sizeof(void *);        /* floormemo */

  /* floor0's ilsp scratch is a bounded alloca in vorbis_lsp_to_curve
     (order <= 255, so <= 1020 bytes), not a block-arena allocation. */

  /* The terms above are raw sizes, but _vorbis_block_alloc rounds every
     allocation up to ARENA_ALIGN, so allow (ARENA_ALIGN-1) of waste per
     allocation. Per packet there are at most 3*channels + 4 allocations
     (PCM no longer lives in the block arena; see ivorbiscodec.h's
     vorbis_dsp_state::work/mdctright):
       4          mapping bundles          (mapping0.c: pcm/zero/nonzero/floormemo)
       channels   floor memos              (floor0/1 inverse1)
       channels   residue inner arrays     (res012.c: res0/1 per-channel inner
                                             arrays, summed over submaps)
       channels   residue outer arrays     (res012.c: res0/1 outer array or res2
                                             partword array - one per submap, and
                                             the submaps partition the channels)
     The slack has to scale with channels (Vorbis allows up to 255): the
     decode path dereferences _vorbis_block_alloc's NULL return unchecked,
     so an undersized arena is a crash, not a clean failure. */
  size += (3L * channels + 4) * (ARENA_ALIGN - 1);

  return size;
}

int vorbis_block_init(vorbis_dsp_state *v, vorbis_block *vb){
  long arena_size;
  memset(vb,0,sizeof(*vb));
  vb->vd=v;
  arena_size=_vorbis_arena_compute_size(v->vi);
  vb->arena_data=_ogg_malloc(arena_size);
  if(!vb->arena_data) return -1;
  vb->arena_capacity=arena_size;
  vb->arena_used=0;
  return 0;
}

int vorbis_block_clear(vorbis_block *vb){
  if(vb->arena_data) _ogg_free(vb->arena_data);
  memset(vb,0,sizeof(*vb));
  return 0;
}

/* Extra bytes added to the computed DSP setup arena size as defensive slack.
   _vorbis_dsp_arena_compute_size() is exact (it mirrors every allocation with
   the same ARENA_ALIGN rounding the bump allocator uses), so this is pure
   insurance against an overlooked site or platform sizeof drift; it is tiny
   next to the arena itself. (Residue, floor1, and mapping0 no longer
   contribute here: their look()s return the info pointer directly and their
   arena_size()s are 0 - floor0 is the only backend whose look() still
   allocates - see res012.c, floor1.c, mapping0.c and
   src/tremor/CHANGES.md.) */
#define DSP_ARENA_SAFETY 256

/* Ceiling on the DSP setup arena. The arena is addressed with `long` offsets
   (setup_arena_used/_capacity) and handed to a single _ogg_malloc, so on the
   ILP32 target (ESP32, 32-bit long) it can never exceed 2^31-1 bytes. A crafted
   header - many modes/submaps fanning out into per-mode floor/mapping lookups -
   can drive _vorbis_dsp_arena_compute_size's mirror total past that. Summed
   into a 32-bit long it would wrap to a small value,
   _ogg_malloc would then succeed undersized, and a backend look()'s unchecked
   arena allocations would scribble past the buffer. The total is computed in
   64 bits and any stream over this cap is rejected before the malloc. */
#define DSP_ARENA_MAX_BYTES 0x7fffffffLL

/* Compute the size of the DSP setup arena from codec_setup_info. Mirrors the
   top-level allocations in _vds_init and, through the per-backend arena_size
   vtable entries, every floor/residue lookup mapping0_look builds. mapping0
   itself no longer owns any arena state (vorbis_info_mapping0 is the "look");
   floor1 and every residue backend are likewise identity look()s that
   contribute 0. floor0 is the only backend whose look() still allocates
   (linearmap/lsp_look, cached per blockflag on vorbis_info_floor0 - see
   mapping0.c and backends.h), so it is the only nonzero term this loop
   actually sums in practice. Each term is rounded to ARENA_ALIGN exactly as
   _vorbis_setup_alloc rounds, so for a correct mirror the return value equals
   the arena's final used watermark.

   The per-channel work[] planes (n1/2 int32s each) are counted for ALL
   channels: residue decode and channel coupling touch every channel, so
   work[] is allocated regardless of `keep`. The per-channel mdctright[]
   overlap tails (n1/4 int32s each) are counted only for channels the caller
   keeps: `keep` is the per-channel mask passed to vorbis_synthesis_init_ex
   (NULL means keep all). Because the mask is fixed before the arena is sized,
   a dropped channel's mdctright is never allocated at all, so there is
   nothing to free later and the whole DSP state collapses to this single
   allocation. */
/* Returns the mirrored arena size in 64-bit so a maliciously large mode/submap
   fan-out (each per-mode term is a valid long, but up to 64 modes * 16 submaps
   can sum past 2^31) cannot wrap; the caller enforces DSP_ARENA_MAX_BYTES. */
static ogg_int64_t _vorbis_dsp_arena_compute_size(vorbis_dsp_state *v,const unsigned char *keep){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  int channels=vi->channels;
  ogg_int64_t size=0;
  int i;

  size+=_vorbis_arena_round(sizeof(private_state));                 /* backend_state */
  size+=_vorbis_arena_round(channels*(long)sizeof(ogg_int32_t *));  /* v->work ptr array */
  size+=_vorbis_arena_round(channels*(long)sizeof(ogg_int32_t *));  /* v->mdctright ptr array */
  size+=_vorbis_arena_round(VORBIS_KEEP_BYTES(channels));           /* v->channel_keep */

  /* v->work[i]: ALL channels, n1/2 int32s each */
  {
    long workbytes=_vorbis_arena_round((ci->blocksizes[1]/2)*(long)sizeof(ogg_int32_t));
    size += channels * workbytes;
  }

  /* v->mdctright[i]: kept channels only, n1/4 int32s each */
  {
    long tailbytes=_vorbis_arena_round((ci->blocksizes[1]/4)*(long)sizeof(ogg_int32_t));
    for(i=0;i<channels;i++)
      if(!keep || vorbis_keep_get(keep,i)) size+=tailbytes;
  }

  size+=_vorbis_arena_round(ci->modes*(long)sizeof(vorbis_look_mapping *)); /* b->mode */

  for(i=0;i<ci->modes;i++){
    int mapnum=ci->mode_param[i]->mapping;
    int maptype=ci->map_type[mapnum];
    size+=_mapping_P[maptype]->arena_size(v,ci->mode_param[i],
					  ci->map_param[mapnum]);
  }

  return size;
}

static int _vds_init(vorbis_dsp_state *v,vorbis_info *vi,const unsigned char *keep){
  int i;
  ogg_int64_t arena_size;
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  private_state *b=NULL;

  if(ci==NULL) return 1;

  memset(v,0,sizeof(*v));
  v->vi=vi;

  /* Codebooks must already be unpacked (done in vorbis_book_unpack during header
     parsing): residue arena sizing reads book dimensions and the lookups
     reference book_param. Nothing is allocated yet, so just bail. */
  if(!ci->book_param)
    return -1;

  /* One-shot allocation of the entire DSP setup arena. `keep` (NULL = keep all)
     is fixed here, before sizing, so dropped channels' history buffers are never
     allocated rather than allocated-then-freed. */
  arena_size=_vorbis_dsp_arena_compute_size(v,keep);
  /* Reject a header whose mirrored arena can't fit a `long` (see
     DSP_ARENA_MAX_BYTES). Leaving room for DSP_ARENA_SAFETY keeps the malloc
     argument and setup_arena_capacity within a positive long on the 32-bit
     target. A genuinely large but representable arena still fails cleanly at the
     _ogg_malloc NULL check below. */
  if(arena_size<0 || arena_size>DSP_ARENA_MAX_BYTES-DSP_ARENA_SAFETY)
    return -1;
  v->setup_arena_data=_ogg_malloc((long)arena_size+DSP_ARENA_SAFETY);
  if(!v->setup_arena_data)
    return -1;
  v->setup_arena_capacity=(long)arena_size+DSP_ARENA_SAFETY;
  v->setup_arena_used=0;

  b=(private_state *)(v->backend_state=_vorbis_setup_calloc(v,1,sizeof(*b)));
  b->modebits=ilog(ci->modes);

  /* Vorbis I uses only window type 0 (returns a static table, not arena mem) */
  b->window[0]=_vorbis_window(0,ci->blocksizes[0]/2);
  b->window[1]=_vorbis_window(0,ci->blocksizes[1]/2);

  v->work=(ogg_int32_t **)_vorbis_setup_alloc(v,vi->channels*(long)sizeof(*v->work));
  v->mdctright=(ogg_int32_t **)_vorbis_setup_alloc(v,vi->channels*(long)sizeof(*v->mdctright));

  /* microVorbis: per-channel decode mask, fixed at init. NULL keep = decode
     every channel (default, behavior unchanged). The mask is immutable for the
     stream's life because the mdctright buffers it gates are arena-owned. */
  v->channel_keep=(unsigned char *)_vorbis_setup_calloc(v,VORBIS_KEEP_BYTES(vi->channels),1);
  for(i=0;i<vi->channels;i++)
    if(!keep || vorbis_keep_get(keep,i))vorbis_keep_set(v->channel_keep,i);

  /* work[i]: ALL channels, n1/2 int32s. Residue decode and channel coupling
     touch every channel even when only some are kept, so every channel needs
     a plane. Zeroed: mapping0_inverse memsets it per-block anyway, but a
     dropped channel's plane is otherwise never written and mdct_shift_right
     reads it unconditionally for kept channels' shift on the next packet. */
  for(i=0;i<vi->channels;i++)
    v->work[i]=(ogg_int32_t *)_vorbis_setup_calloc(v,ci->blocksizes[1]/2,sizeof(*v->work[i]));

  /* mdctright[i]: kept channels only, n1/4 int32s. Dropped channels get a NULL
     pointer (vorbis_synthesis_lapout rejects non-kept channels before
     touching it). Zeroed: mdct_unroll_lap reads the tail before the first
     block writes it (via mdct_shift_right). */
  for(i=0;i<vi->channels;i++)
    v->mdctright[i]=vorbis_keep_get(v->channel_keep,i)
      ?(ogg_int32_t *)_vorbis_setup_calloc(v,ci->blocksizes[1]/4,sizeof(*v->mdctright[i]))
      :NULL;

  /* all 1 (large block) or 0 (small block) */
  /* explicitly set for the sake of clarity */
  v->lW=0; /* previous window size */
  v->W=0;  /* current window size */

  /* initialize all the mapping/backend lookups (all bump from the arena) */
  b->mode=(vorbis_look_mapping **)_vorbis_setup_calloc(v,ci->modes,sizeof(*b->mode));
  for(i=0;i<ci->modes;i++){
    int mapnum=ci->mode_param[i]->mapping;
    int maptype=ci->map_type[mapnum];
    b->mode[i]=_mapping_P[maptype]->look(v,ci->mode_param[i],
					 ci->map_param[mapnum]);
  }

  return 0;
}

int vorbis_synthesis_restart(vorbis_dsp_state *v){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci;

  if(!v->backend_state)return -1;
  if(!vi)return -1;
  ci=vi->codec_setup;
  if(!ci)return -1;

  v->out_begin=-1;
  v->out_end=-1;

  v->granulepos=-1;
  v->sequence=-1;
  ((private_state *)(v->backend_state))->sample_count=-1;

  return(0);
}

int vorbis_synthesis_init_ex(vorbis_dsp_state *v,vorbis_info *vi,
			     const unsigned char *keep,int n){
  /* keep[] selects which channels to allocate/decode and is immutable for the
     stream. n must match the channel count; pass keep=NULL (any n) to keep all. */
  if(!vi || (keep && n!=vi->channels))return 1;
  if(_vds_init(v,vi,keep))return 1;
  vorbis_synthesis_restart(v);

  return 0;
}

void vorbis_dsp_clear(vorbis_dsp_state *v){
  if(v){
    /* The entire DSP state (private_state, the work/mdctright/channel_keep
       pointer arrays, every channel's work[] plane, every kept channel's
       mdctright[] tail, b->mode, and every mode/floor/residue lookup) lives in
       the single setup arena, released in one free. The backend free_look
       hooks are no-ops for this reason. */
    if(v->setup_arena_data)_ogg_free(v->setup_arena_data);

    memset(v,0,sizeof(*v));
  }
}

/* Bookkeeping only: no PCM is touched here. mapping0_inverse (via
   mdct_backward) has already left the current block's half-transform in
   vd->work[]; synthesis.c's mdct_shift_right has already saved the previous
   block's tail into vd->mdctright[] before this ran. This just advances the
   lW/W/granulepos/sample_count state and opens the readout window
   (out_begin/out_end) that vorbis_synthesis_lapout reconstructs from on
   demand. */
int vorbis_synthesis_blockin(vorbis_dsp_state *v,vorbis_block *vb){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  private_state *b=v->backend_state;

  /* Don't accept a new block until the previous one's samples have been fully
     read out. out_begin==-1 is priming (nothing pending yet), not this case. */
  if(v->out_begin>-1 && v->out_begin<v->out_end)return(OV_EINVAL);

  v->lW=v->W;
  v->W=vb->W;

  if((v->sequence==-1)||
     (v->sequence+1 != vb->sequence)){
    v->granulepos=-1; /* out of sequence; lose count */
    b->sample_count=-1;
  }

  v->sequence=vb->sequence;

  if(vb->pcmend){  /* pcmend==0 means vorbis_synthesis_trackonly was called on
                       this block: bookkeeping only, nothing was decoded */
    if(v->out_begin==-1){
      /* first real block: establishes lap state, emits no samples yet */
      v->out_begin=0;
      v->out_end=0;
    }else{
      v->out_begin=0;
      v->out_end=ci->blocksizes[v->lW]/4+ci->blocksizes[v->W]/4;
    }
  }

  /* track the frame number... This is for convenience, but also
     making sure our last packet doesn't end with added padding.  If
     the last packet is partial, the number of samples we'll have to
     return will be past the vb->granulepos.

     This is not foolproof!  It will be confused if we begin
     decoding at the last page after a seek or hole.  In that case,
     we don't have a starting point to judge where the last frame
     is.  For this reason, vorbisfile will always try to make sure
     it reads the last two marked pages in proper sequence */

  if(b->sample_count==-1){
    b->sample_count=0;
  }else{
    b->sample_count+=ci->blocksizes[v->lW]/4+ci->blocksizes[v->W]/4;
  }

  if(v->granulepos==-1){
    if(vb->granulepos!=-1){ /* only set if we have a position to set to */

      v->granulepos=vb->granulepos;

      /* is this a short page? */
      if(b->sample_count>v->granulepos){
	/* corner case; if this is both the first and last audio page,
	   then spec says the end is cut, not beginning */
        /* we use ogg_int64_t for granule positions because a uint64
           isn't universally available.  A 'negative' granpos is really
           a huge unsigned sample number far beyond sample_count, so
           there is nothing to trim; subtracting it directly would
           overflow ogg_int64_t (e.g. a crafted INT64_MIN granpos). */
        ogg_int64_t extra = vb->granulepos<0 ? 0 :
                            b->sample_count-vb->granulepos;

	if(vb->eofflag){
	  /* trim the end */
	  /* no preceding granulepos; assume we started at zero (we'd
	     have to in a short single-page stream) */
	  /* granulepos could be -1 due to a seek, but that would result
	     in a long coun`t, not short count */

          /* Guard against corrupt/malicious frames that set EOP and
             a backdated granpos; don't rewind more samples than we
             actually have */
          if(extra > v->out_end - v->out_begin)
            extra = v->out_end - v->out_begin;

	  v->out_end-=extra;
	}else{
	  /* trim the beginning */
	  v->out_begin+=extra;
	  if(v->out_begin>v->out_end)
	    v->out_begin=v->out_end;
	}

      }

    }
  }else{
    v->granulepos+=ci->blocksizes[v->lW]/4+ci->blocksizes[v->W]/4;
    if(vb->granulepos!=-1 && v->granulepos!=vb->granulepos){

      if(v->granulepos>vb->granulepos){
	/* see the short-page case above: guard the subtraction against a
	   'negative' (huge unsigned) granpos to avoid ogg_int64_t overflow */
	ogg_int64_t extra = vb->granulepos<0 ? 0 :
	                    v->granulepos-vb->granulepos;

	if(extra)
	  if(vb->eofflag){
	    /* partial last frame.  Strip the extra samples off */

            /* Guard against corrupt/malicious frames that set EOP and
               a backdated granpos; don't rewind more samples than we
               actually have */
            if(extra > v->out_end - v->out_begin)
              extra = v->out_end - v->out_begin;

            v->out_end-=extra;

	  } /* else {Shouldn't happen *unless* the bitstream is out of
	       spec.  Either way, believe the bitstream } */
      } /* else {Shouldn't happen *unless* the bitstream is out of
	   spec.  Either way, believe the bitstream } */
      v->granulepos=vb->granulepos;
    }
  }

  /* Update, cleanup */

  if(vb->eofflag)v->eofflag=1;
  return(0);
}

/* Pending finalized samples not yet consumed by vorbis_synthesis_read. */
int vorbis_synthesis_pcmavail(vorbis_dsp_state *v){
  return (v->out_begin>-1 && v->out_begin<v->out_end) ? v->out_end-v->out_begin : 0;
}

/* Reconstruct up to `samples` pending samples of channel ch into out. Runs
   the deferred iMDCT tail + window + overlap-add (mdct_unroll_lap) against
   vd->work[ch] (current block) and vd->mdctright[ch] (previous block's
   tail); non-consuming, so a caller can call this repeatedly (e.g. retrying
   after an undersized output buffer) until vorbis_synthesis_read advances
   out_begin. */
int vorbis_synthesis_lapout(vorbis_dsp_state *v,int ch,ogg_int32_t *out,int samples){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  private_state *b=(private_state *)v->backend_state;
  int avail,n;

  if(ch<0 || ch>=vi->channels)return(OV_EINVAL);
  if(!v->channel_keep || !vorbis_keep_get(v->channel_keep,ch))return(OV_EINVAL);

  avail=(v->out_begin>-1 && v->out_begin<v->out_end) ? v->out_end-v->out_begin : 0;
  n=samples<avail ? samples : avail;
  if(n<=0)return(0);

  mdct_unroll_lap(ci->blocksizes[0],ci->blocksizes[1],
		  (int)v->lW,(int)v->W,
		  v->work[ch],v->mdctright[ch],
		  (const LOOKUP_T *)b->window[0],(const LOOKUP_T *)b->window[1],
		  out,1,
		  v->out_begin,v->out_begin+n);
  return(n);
}

int vorbis_synthesis_read(vorbis_dsp_state *v,int samples){
  if(samples && v->out_begin+samples>v->out_end)return(OV_EINVAL);
  v->out_begin+=samples;
  return(0);
}

