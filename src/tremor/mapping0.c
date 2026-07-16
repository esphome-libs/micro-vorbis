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

 function: channel mapping 0 implementation

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
#include "mdct.h"
#include "codec_internal.h"
#include "codebook.h"
#include "registry.h"
#include "misc.h"
#include "block.h"

/* microVorbis: floor1 and every residue backend now have identity look()
   functions - they just hand back the info pointer built by unpack() (see
   src/tremor/CHANGES.md) - so mapping0_inverse can dispatch straight off
   ci->floor_param/ci->residue_param at decode time with no per-mapping
   lookup struct. floor0 is the sole exception (see vorbis_info_floor0's
   look_cache in backends.h); mapping0_look()'s only remaining job is
   building and caching that. There is therefore no vorbis_look_mapping0
   struct anymore - vorbis_info_mapping0 itself is the "look". */

static void mapping0_free_info(vorbis_info_mapping *i){
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)i;
  if(info){
    /* the struct is calloc'd, so these are NULL until mapping0_unpack
       allocates them; safe on partially-initialized structs */
    _ogg_free(info->chmuxlist);
    _ogg_free(info->coupling_mag);
    _ogg_free(info->coupling_ang);
    memset(info,0,sizeof(*info));
    _ogg_free(info);
  }
}

static void mapping0_free_look(vorbis_look_mapping *look){
  /* microVorbis: mapping0_look() no longer allocates a mapping-owned struct -
     vorbis_info_mapping0 itself is the "look" (see mapping0_look). The one
     real allocation it triggers, floor0's cached look, lives in the DSP
     setup arena (see vorbis_info_floor0::look_cache in backends.h) and is
     freed in one shot by vorbis_dsp_clear. Nothing to free here. */
  (void)look;
}

static vorbis_look_mapping *mapping0_look(vorbis_dsp_state *vd,vorbis_info_mode *vm,
			  vorbis_info_mapping *m){
  int i;
  vorbis_info          *vi=vd->vi;
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)m;

  /* microVorbis: floor1/residue look()s are identity (mapping0_inverse reads
     ci->floor_param/ci->residue_param directly), so the only backend that
     still needs building here is floor0 - see vorbis_info_floor0::look_cache
     in backends.h for why it can't be collapsed the same way, and why caching
     it there (rather than in a mapping0-owned array) is safe. Called once per
     mode from _vds_init's b->mode[] loop (block.c), matching exactly what
     mapping0_arena_size counts below. */
  for(i=0;i<info->submaps;i++){
    int floornum=info->floorsubmap[i];
    if(ci->floor_type[floornum]==0){
      vorbis_info_floor0 *f0=(vorbis_info_floor0 *)ci->floor_param[floornum];
      f0->look_cache[vm->blockflag]=_floor_P[0]->look(vd,vm,ci->floor_param[floornum]);
    }
  }

  return (vorbis_look_mapping *)m;
}

/* microVorbis: bytes mapping0_look() bumps from the DSP setup arena. Mapping0
   itself owns no struct/array of its own anymore (see mapping0_look): the sum
   below is exactly the per-submap floor/residue arena_size entries, which is
   also exactly what floor0_arena_size/residue arena_size charge for (floor1
   and every residue backend return 0 - their look()s are identity and
   mapping0_look never calls them). Mirrors mapping0_look 1:1; keep in sync. */
static long mapping0_arena_size(vorbis_dsp_state *vd,vorbis_info_mode *vm,
				vorbis_info_mapping *m){
  int i;
  vorbis_info          *vi=vd->vi;
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)m;
  long size=0;

  for(i=0;i<info->submaps;i++){
    int floornum=info->floorsubmap[i];
    int resnum=info->residuesubmap[i];
    size+=_floor_P[ci->floor_type[floornum]]->
      arena_size(vd,vm,ci->floor_param[floornum]);
    size+=_residue_P[ci->residue_type[resnum]]->
      arena_size(vd,vm,ci->residue_param[resnum]);
  }

  return size;
}

static int ilog(unsigned int v){
  int ret=0;
  if(v)--v;
  while(v){
    ret++;
    v>>=1;
  }
  return(ret);
}

/* also responsible for range checking */
static vorbis_info_mapping *mapping0_unpack(vorbis_info *vi,oggpack_buffer *opb){
  int i,b;
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)_ogg_calloc(1,sizeof(*info));
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;

  if(!info)goto err_out;
  b=oggpack_read(opb,1);
  if(b<0)goto err_out;
  if(b){
    info->submaps=oggpack_read(opb,4)+1;
    if(info->submaps<=0)goto err_out;
  }else
    info->submaps=1;

  /* mapping0_inverse indexes chmuxlist for every channel even when the
     submap loop below never fills it (submaps==1), so always allocate it
     zeroed and sized to the channel count */
  if(vi->channels<1 || vi->channels>VIM_CHANNELS)goto err_out;
  info->chmuxlist=(int *)_ogg_calloc(vi->channels,sizeof(*info->chmuxlist));
  if(!info->chmuxlist)goto err_out;

  b=oggpack_read(opb,1);
  if(b<0)goto err_out;
  if(b){
    info->coupling_steps=oggpack_read(opb,8)+1;
    if(info->coupling_steps<=0 || info->coupling_steps>VIM_COUPLES)
      goto err_out;
    info->coupling_mag=(int *)
      _ogg_malloc(info->coupling_steps*sizeof(*info->coupling_mag));
    info->coupling_ang=(int *)
      _ogg_malloc(info->coupling_steps*sizeof(*info->coupling_ang));
    if(!info->coupling_mag || !info->coupling_ang)goto err_out;
    for(i=0;i<info->coupling_steps;i++){
      int testM=info->coupling_mag[i]=oggpack_read(opb,ilog(vi->channels));
      int testA=info->coupling_ang[i]=oggpack_read(opb,ilog(vi->channels));

      if(testM<0 || 
	 testA<0 || 
	 testM==testA || 
	 testM>=vi->channels ||
	 testA>=vi->channels) goto err_out;
    }

  }

  if(oggpack_read(opb,2)!=0)goto err_out; /* 2,3:reserved */
    
  if(info->submaps>1){
    for(i=0;i<vi->channels;i++){
      info->chmuxlist[i]=oggpack_read(opb,4);
      if(info->chmuxlist[i]>=info->submaps || info->chmuxlist[i]<0)goto err_out;
    }
  }
  for(i=0;i<info->submaps;i++){
    int temp=oggpack_read(opb,8);
    if(temp>=ci->times)goto err_out;
    info->floorsubmap[i]=oggpack_read(opb,8);
    if(info->floorsubmap[i]>=ci->floors || info->floorsubmap[i]<0)goto err_out;
    info->residuesubmap[i]=oggpack_read(opb,8);
    if(info->residuesubmap[i]>=ci->residues || info->residuesubmap[i]<0)
      goto err_out;
  }

  return info;

 err_out:
  mapping0_free_info(info);
  return(NULL);
}

/* microVorbis: floor look for a submap at decode time. Identity (the info
   pointer, i.e. exactly what floor1_look would have returned) for every
   backend except floor0, whose real per-blockflag look mapping0_look built
   and cached on the floor definition itself - see vorbis_info_floor0's
   look_cache in backends.h. */
static vorbis_look_floor *mapping0_floor_look(codec_setup_info *ci,int floornum,
					      int blockflag){
  if(ci->floor_type[floornum]==0)
    return (vorbis_look_floor *)
      ((vorbis_info_floor0 *)ci->floor_param[floornum])->look_cache[blockflag];
  return (vorbis_look_floor *)ci->floor_param[floornum];
}

static int mapping0_inverse(vorbis_block *vb,vorbis_look_mapping *l){
  vorbis_dsp_state     *vd=vb->vd;
  vorbis_info          *vi=vd->vi;
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)l;

  int                   i,j;
  long                  n=vb->pcmend=ci->blocksizes[vb->W];

  /* microVorbis: per-channel decode mask. NULL => keep everything. Only the
     post-coupling synthesis (floor-apply / iMDCT) is gated; floor decode,
     residue, and coupling above run for every channel so kept channels stay
     bit-exact. Windowing/overlap-add is deferred to readout time
     (vorbis_synthesis_lapout), which the same mask gates via mdctright. */
  const unsigned char  *keep=vd->channel_keep;

  ARENA_STACK(ogg_int32_t *, pcmbundle, vi->channels, vb);
  ARENA_STACK(int, zerobundle, vi->channels, vb);

  ARENA_STACK(int, nonzero, vi->channels, vb);
  ARENA_STACK(void *, floormemo, vi->channels, vb);

  /* time domain information decode (note that applying the
     information would have to happen later; we'll probably add a
     function entry to the harness for that later */
  /* NOT IMPLEMENTED */

  /* recover the spectral envelope; store it in the working vector for now */
  for(i=0;i<vi->channels;i++){
    int submap=info->chmuxlist[i];
    int floornum=info->floorsubmap[submap];
    vorbis_func_floor *floor_func=_floor_P[ci->floor_type[floornum]];
    vorbis_look_floor *floor_look=mapping0_floor_look(ci,floornum,(int)vb->W);

    floormemo[i]=floor_func->inverse1(vb,floor_look);
    if(floormemo[i])
      nonzero[i]=1;
    else
      nonzero[i]=0;
    memset(vd->work[i],0,sizeof(*vd->work[i])*n/2);
  }

  /* channel coupling can 'dirty' the nonzero listing */
  for(i=0;i<info->coupling_steps;i++){
    if(nonzero[info->coupling_mag[i]] ||
       nonzero[info->coupling_ang[i]]){
      nonzero[info->coupling_mag[i]]=1;
      nonzero[info->coupling_ang[i]]=1;
    }
  }

  /* recover the residue into our working vectors */
  for(i=0;i<info->submaps;i++){
    int ch_in_bundle=0;
    int resnum=info->residuesubmap[i];
    vorbis_func_residue *residue_func=_residue_P[ci->residue_type[resnum]];
    vorbis_look_residue *residue_look=(vorbis_look_residue *)ci->residue_param[resnum];

    for(j=0;j<vi->channels;j++){
      if(info->chmuxlist[j]==i){
	if(nonzero[j])
	  zerobundle[ch_in_bundle]=1;
	else
	  zerobundle[ch_in_bundle]=0;
	pcmbundle[ch_in_bundle++]=vd->work[j];
      }
    }

    residue_func->inverse(vb,residue_look,pcmbundle,zerobundle,ch_in_bundle);
  }

  /* channel coupling */
  for(i=info->coupling_steps-1;i>=0;i--){
    ogg_int32_t *pcmM=vd->work[info->coupling_mag[i]];
    ogg_int32_t *pcmA=vd->work[info->coupling_ang[i]];

    for(j=0;j<n/2;j++){
      ogg_int32_t mag=pcmM[j];
      ogg_int32_t ang=pcmA[j];

      if(mag>0)
	if(ang>0){
	  pcmM[j]=mag;
	  pcmA[j]=mag-ang;
	}else{
	  pcmA[j]=mag;
	  pcmM[j]=mag+ang;
	}
      else
	if(ang>0){
	  pcmM[j]=mag;
	  pcmA[j]=mag+ang;
	}else{
	  pcmA[j]=mag;
	  pcmM[j]=mag-ang;
	}
    }
  }

  /* compute and apply spectral envelope */
  for(i=0;i<vi->channels;i++){
    ogg_int32_t *pcm=vd->work[i];
    int submap=info->chmuxlist[i];
    int floornum;
    vorbis_func_floor *floor_func;
    vorbis_look_floor *floor_look;
    if(keep && !vorbis_keep_get(keep,i))continue;

    floornum=info->floorsubmap[submap];
    floor_func=_floor_P[ci->floor_type[floornum]];
    floor_look=mapping0_floor_look(ci,floornum,(int)vb->W);
    floor_func->inverse2(vb,floor_look,floormemo[i],pcm);
  }

  /* transform the working vector in place (half-block iMDCT; the final
     deinterleave/expansion + windowing/overlap-add happens later, at PCM
     readout - see mdct_unroll_lap). A !nonzero channel's plane is already
     all-zero (memset above, residue skipped), and the MDCT of an all-zero
     input is all-zero, so no separate zero-fill is needed here. */
  for(i=0;i<vi->channels;i++){
    if(keep && !vorbis_keep_get(keep,i))continue;
    mdct_backward(n,vd->work[i]);
  }

  /* all done! */
  return(0);
}

/* export hooks */
vorbis_func_mapping mapping0_exportbundle={
  &mapping0_unpack,
  &mapping0_look,
  &mapping0_free_info,
  &mapping0_free_look,
  &mapping0_inverse,
  &mapping0_arena_size
};
