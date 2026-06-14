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

static int ilog(unsigned int v){
  int ret=0;
  if(v)--v;
  while(v){
    ret++;
    v>>=1;
  }
  return(ret);
}

/* pcm accumulator examples (not exhaustive):

 <-------------- lW ---------------->
                   <--------------- W ---------------->
:            .....|.....       _______________         |
:        .'''     |     '''_---      |       |\        |
:.....'''         |_____--- '''......|       | \_______|
:.................|__________________|_______|__|______|
                  |<------ Sl ------>|      > Sr <     |endW
                  |beginSl           |endSl  |  |endSr   
                  |beginW            |endlW  |beginSr


                      |< lW >|       
                   <--------------- W ---------------->
                  |   |  ..  ______________            |
                  |   | '  `/        |     ---_        |
                  |___.'___/`.       |         ---_____| 
                  |_______|__|_______|_________________|
                  |      >|Sl|<      |<------ Sr ----->|endW
                  |       |  |endSl  |beginSr          |endSr
                  |beginW |  |endlW                     
                  mult[0] |beginSl                     mult[n]

 <-------------- lW ----------------->
                          |<--W-->|                               
:            ..............  ___  |   |                    
:        .'''             |`/   \ |   |                       
:.....'''                 |/`....\|...|                    
:.........................|___|___|___|                  
                          |Sl |Sr |endW    
                          |   |   |endSr
                          |   |beginSr
                          |   |endSl
			  |beginSl
			  |beginW
*/

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

  /* PCM pointer array: synthesis.c:76 */
  size += channels * (long)sizeof(ogg_int32_t *);

  /* PCM data per channel: synthesis.c:78 */
  size += channels * n * (long)sizeof(ogg_int32_t);

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

  /* Residue partword allocations: res012.c
     _01inverse (types 0,1): ch * sizeof(int**) + ch * partwords * sizeof(int*)
     res2_inverse (type 2):  partwords * sizeof(int*)
     _01inverse caps `end` at pcmend/2; res2_inverse caps at pcmend*ch/2, so
     res2 can have up to `ch` times the partitions of res0/res1. Track the
     per-type worst case separately. */
  {
    long max_partwords_01=0;
    long max_partwords_2=0;
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
        if(pw>max_partwords_01) max_partwords_01=pw;
      }

      long end2=ri->end < end_cap_2 ? ri->end : end_cap_2;
      long rn2=end2 - ri->begin;
      if(rn2>0){
        long partvals=rn2/ri->grouping;
        long pw=(partvals+dim-1)/dim;
        if(pw>max_partwords_2) max_partwords_2=pw;
      }
    }
    /* _01inverse: outer array + per-ch inner arrays */
    size += channels * (long)sizeof(int **);
    size += channels * max_partwords_01 * (long)sizeof(int *);
    /* res2: separate partword array */
    size += max_partwords_2 * (long)sizeof(int *);
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
     allocation. Per packet there are at most 4*channels + 5 allocations:
       1          pcm pointer array        (synthesis.c)
       channels   pcm data buffers         (synthesis.c)
       4          mapping bundles          (mapping0.c: pcm/zero/nonzero/floormemo)
       channels   floor memos              (floor0/1 inverse1)
       channels   residue inner partwords  (res012.c, summed over submaps)
       channels   residue outer partwords  (res012.c, per non-empty submap)
     The slack has to scale with channels (Vorbis allows up to 255): the
     decode path dereferences _vorbis_block_alloc's NULL return unchecked,
     so an undersized arena is a crash, not a clean failure. */
  size += (4L * channels + 5) * (ARENA_ALIGN - 1);

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
   next to the arena itself (which is dominated by residue decodemaps). */
#define DSP_ARENA_SAFETY 256

/* Compute the size of the DSP setup arena from codec_setup_info. Mirrors the
   top-level allocations in _vds_init and, through the per-backend arena_size
   vtable entries, every mode/floor/residue lookup that mapping0_look builds.
   Each term is rounded to ARENA_ALIGN exactly as _vorbis_setup_alloc rounds, so
   for a correct mirror the return value equals the arena's final used watermark.

   The per-channel PCM history buffers (v->pcm[i], pcm_storage int32s each) ARE
   counted, but only for channels the caller keeps: `keep` is the per-channel
   mask passed to vorbis_synthesis_init_ex (NULL means keep all). Because the
   mask is fixed before the arena is sized, dropped channels are never allocated
   at all, so there is nothing to free later and the whole DSP state collapses to
   this single allocation. */
static long _vorbis_dsp_arena_compute_size(vorbis_dsp_state *v,const unsigned char *keep){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  int channels=vi->channels;
  long size=0;
  int i;

  size+=_vorbis_arena_round(sizeof(private_state));                 /* backend_state */
  size+=_vorbis_arena_round(channels*(long)sizeof(ogg_int32_t *));  /* v->pcm */
  size+=_vorbis_arena_round(channels*(long)sizeof(ogg_int32_t *));  /* v->pcmret */
  size+=_vorbis_arena_round(VORBIS_KEEP_BYTES(channels));           /* v->channel_keep */

  /* per-channel PCM history buffers, kept channels only */
  {
    long pcmbytes=_vorbis_arena_round(ci->blocksizes[1]*(long)sizeof(ogg_int32_t));
    for(i=0;i<channels;i++)
      if(!keep || vorbis_keep_get(keep,i)) size+=pcmbytes;
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
  long arena_size;
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
  v->setup_arena_data=_ogg_malloc(arena_size+DSP_ARENA_SAFETY);
  if(!v->setup_arena_data)
    return -1;
  v->setup_arena_capacity=arena_size+DSP_ARENA_SAFETY;
  v->setup_arena_used=0;

  b=(private_state *)(v->backend_state=_vorbis_setup_calloc(v,1,sizeof(*b)));
  b->modebits=ilog(ci->modes);

  /* Vorbis I uses only window type 0 (returns a static table, not arena mem) */
  b->window[0]=_vorbis_window(0,ci->blocksizes[0]/2);
  b->window[1]=_vorbis_window(0,ci->blocksizes[1]/2);

  v->pcm_storage=ci->blocksizes[1];
  v->pcm=(ogg_int32_t **)_vorbis_setup_alloc(v,vi->channels*(long)sizeof(*v->pcm));
  v->pcmret=(ogg_int32_t **)_vorbis_setup_alloc(v,vi->channels*(long)sizeof(*v->pcmret));

  /* microVorbis: per-channel decode mask, fixed at init. NULL keep = decode
     every channel (default, behavior unchanged). The mask is immutable for the
     stream's life because the history buffers it gates are arena-owned. */
  v->channel_keep=(unsigned char *)_vorbis_setup_calloc(v,VORBIS_KEEP_BYTES(vi->channels),1);
  for(i=0;i<vi->channels;i++)
    if(!keep || vorbis_keep_get(keep,i))vorbis_keep_set(v->channel_keep,i);

  /* Per-channel PCM history now lives in the setup arena: only kept channels are
     allocated, dropped channels get a NULL pointer (the blockin/pcmout paths
     already guard on NULL / channel_keep). zeroed because overlap-add reads the
     history before the first block writes it. */
  for(i=0;i<vi->channels;i++)
    v->pcm[i]=vorbis_keep_get(v->channel_keep,i)
      ?(ogg_int32_t *)_vorbis_setup_calloc(v,v->pcm_storage,sizeof(*v->pcm[i]))
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

  v->centerW=ci->blocksizes[1]/2;
  v->pcm_current=v->centerW;
  
  v->pcm_returned=-1;
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
    /* The entire DSP state (private_state, the pcm/pcmret/channel_keep pointer
       arrays, every kept channel's PCM history buffer, b->mode, and every
       mode/floor/residue lookup) lives in the single setup arena, released in
       one free. The backend free_look hooks are no-ops for this reason. */
    if(v->setup_arena_data)_ogg_free(v->setup_arena_data);

    memset(v,0,sizeof(*v));
  }
}

/* Unlike in analysis, the window is only partially applied for each
   block.  The time domain envelope is not yet handled at the point of
   calling (as it relies on the previous block). */

int vorbis_synthesis_blockin(vorbis_dsp_state *v,vorbis_block *vb){
  vorbis_info *vi=v->vi;
  codec_setup_info *ci=(codec_setup_info *)vi->codec_setup;
  private_state *b=v->backend_state;
  int i,j;

  if(v->pcm_current>v->pcm_returned  && v->pcm_returned!=-1)return(OV_EINVAL);

  v->lW=v->W;
  v->W=vb->W;
  v->nW=-1;

  if((v->sequence==-1)||
     (v->sequence+1 != vb->sequence)){
    v->granulepos=-1; /* out of sequence; lose count */
    b->sample_count=-1;
  }

  v->sequence=vb->sequence;
  
  if(vb->pcm){  /* no pcm to process if vorbis_synthesis_trackonly 
                   was called on block */
    int n=ci->blocksizes[v->W]/2;
    int n0=ci->blocksizes[0]/2;
    int n1=ci->blocksizes[1]/2;
    
    int thisCenter;
    int prevCenter;
    
    if(v->centerW){
      thisCenter=n1;
      prevCenter=0;
    }else{
      thisCenter=0;
      prevCenter=n1;
    }
    
    /* v->pcm is now used like a two-stage double buffer.  We don't want
       to have to constantly shift *or* adjust memory usage.  Don't
       accept a new block until the old is shifted out */
    
    /* overlap/add PCM */
    
    for(j=0;j<vi->channels;j++){
      /* microVorbis: skip overlap/add + copy for channels the caller does not
         want. Their per-block spectra were still entropy-decoded and coupled
         (so kept channels are correct), but their PCM history is never built or
         read. */
      if(v->channel_keep && !vorbis_keep_get(v->channel_keep,j))continue;

      /* the overlap/add section */
      if(v->lW){
	if(v->W){
	  /* large/large */
	  ogg_int32_t *pcm=v->pcm[j]+prevCenter;
	  ogg_int32_t *p=vb->pcm[j];
	  for(i=0;i<n1;i++)
	    pcm[i]+=p[i];
	}else{
	  /* large/small */
	  ogg_int32_t *pcm=v->pcm[j]+prevCenter+n1/2-n0/2;
	  ogg_int32_t *p=vb->pcm[j];
	  for(i=0;i<n0;i++)
	    pcm[i]+=p[i];
	}
      }else{
	if(v->W){
	  /* small/large */
	  ogg_int32_t *pcm=v->pcm[j]+prevCenter;
	  ogg_int32_t *p=vb->pcm[j]+n1/2-n0/2;
	  for(i=0;i<n0;i++)
	    pcm[i]+=p[i];
	  for(;i<n1/2+n0/2;i++)
	    pcm[i]=p[i];
	}else{
	  /* small/small */
	  ogg_int32_t *pcm=v->pcm[j]+prevCenter;
	  ogg_int32_t *p=vb->pcm[j];
	  for(i=0;i<n0;i++)
	    pcm[i]+=p[i];
	}
      }
      
      /* the copy section */
      {
	ogg_int32_t *pcm=v->pcm[j]+thisCenter;
	ogg_int32_t *p=vb->pcm[j]+n;
	for(i=0;i<n;i++)
	  pcm[i]=p[i];
      }
    }
    
    if(v->centerW)
      v->centerW=0;
    else
      v->centerW=n1;
    
    /* deal with initial packet state; we do this using the explicit
       pcm_returned==-1 flag otherwise we're sensitive to first block
       being short or long */

    if(v->pcm_returned==-1){
      v->pcm_returned=thisCenter;
      v->pcm_current=thisCenter;
    }else{
      v->pcm_returned=prevCenter;
      v->pcm_current=prevCenter+
	ci->blocksizes[v->lW]/4+
	ci->blocksizes[v->W]/4;
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
	  /* no preceeding granulepos; assume we started at zero (we'd
	     have to in a short single-page stream) */
	  /* granulepos could be -1 due to a seek, but that would result
	     in a long coun`t, not short count */

          /* Guard against corrupt/malicious frames that set EOP and
             a backdated granpos; don't rewind more samples than we
             actually have */
          if(extra > v->pcm_current - v->pcm_returned)
            extra = v->pcm_current - v->pcm_returned;

	  v->pcm_current-=extra;
	}else{
	  /* trim the beginning */
	  v->pcm_returned+=extra;
	  if(v->pcm_returned>v->pcm_current)
	    v->pcm_returned=v->pcm_current;
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
            if(extra > v->pcm_current - v->pcm_returned)
              extra = v->pcm_current - v->pcm_returned;

            v->pcm_current-=extra;

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

/* pcm==NULL indicates we just want the pending samples, no more */
int vorbis_synthesis_pcmout(vorbis_dsp_state *v,ogg_int32_t ***pcm){
  vorbis_info *vi=v->vi;
  if(v->pcm_returned>-1 && v->pcm_returned<v->pcm_current){
    if(pcm){
      int i;
      for(i=0;i<vi->channels;i++)
	/* microVorbis: channels whose history buffer was released (not kept)
	   report a NULL pointer rather than NULL+offset arithmetic. The wrapper
	   never reads these planes in selection mode. */
	v->pcmret[i]=v->pcm[i]?v->pcm[i]+v->pcm_returned:NULL;
      *pcm=v->pcmret;
    }
    return(v->pcm_current-v->pcm_returned);
  }
  return(0);
}

int vorbis_synthesis_read(vorbis_dsp_state *v,int samples){
  if(samples && v->pcm_returned+samples>v->pcm_current)return(OV_EINVAL);
  v->pcm_returned+=samples;
  return(0);
}

