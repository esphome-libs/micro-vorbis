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

 function: residue backend 0, 1 and 2 implementation

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
#include "registry.h"
#include "codebook.h"
#include "misc.h"
#include "os.h"
#include "block.h"

void res0_free_info(vorbis_info_residue *i){
  vorbis_info_residue0 *info=(vorbis_info_residue0 *)i;
  if(info){
    /* the struct is calloc'd, so these are NULL until res0_unpack
       allocates them; safe on partially-initialized structs */
    _ogg_free(info->secondstages);
    _ogg_free(info->booklist);
    _ogg_free(info->stagemasks);
    _ogg_free(info->stagebooks);
    memset(info,0,sizeof(*info));
    _ogg_free(info);
  }
}

void res0_free_look(vorbis_look_residue *i){
  /* microVorbis: no look struct - res0_look() returns the vorbis_info_residue0
     pointer directly (its stagemasks/stagebooks/stages, built in res0_unpack,
     are the only decode-time state residue needs). Nothing to free here; the
     info struct is owned by res0_free_info via codec_setup_info. */
  (void)i;
}

static int icount(unsigned int v){
  int ret=0;
  while(v){
    ret+=v&1;
    v>>=1;
  }
  return(ret);
}

/* vorbis_info is for range checking */
vorbis_info_residue *res0_unpack(vorbis_info *vi,oggpack_buffer *opb){
  int j,acc=0;
  vorbis_info_residue0 *info=(vorbis_info_residue0 *)_ogg_calloc(1,sizeof(*info));
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;

  if(!info)goto errout;
  info->begin=oggpack_read(opb,24);
  info->end=oggpack_read(opb,24);
  info->grouping=oggpack_read(opb,24)+1;
  info->partitions=oggpack_read(opb,6)+1;
  info->groupbook=oggpack_read(opb,8);

  /* check for premature EOP */
  if(info->groupbook<0)goto errout;

  if(info->partitions<1 || info->partitions>VIR_PARTS)goto errout;
  info->secondstages=(int *)
    _ogg_malloc(info->partitions*sizeof(*info->secondstages));
  if(!info->secondstages)goto errout;

  for(j=0;j<info->partitions;j++){
    int cascade=oggpack_read(opb,3);
    int cflag=oggpack_read(opb,1);
    if(cflag<0) goto errout;
    if(cflag){
      int c=oggpack_read(opb,5);
      if(c<0) goto errout;
      cascade|=(c<<3);
    }
    info->secondstages[j]=cascade;

    acc+=icount(cascade);
  }

  /* acc is bounded by construction (cascade is 8 bits, so <=8 set bits per
     partition, partitions<=VIR_PARTS); check anyway so the allocation size
     can never outrun the cap */
  if(acc<0 || acc>VIR_BOOKS)goto errout;
  info->booklist=(int *)_ogg_malloc((acc?acc:1)*sizeof(*info->booklist));
  if(!info->booklist)goto errout;
  for(j=0;j<acc;j++){
    int book=oggpack_read(opb,8);
    if(book<0) goto errout;
    info->booklist[j]=book;
  }

  if(info->groupbook>=ci->books)goto errout;
  for(j=0;j<acc;j++){
    if(info->booklist[j]>=ci->books)goto errout;
    if(ci->book_param[info->booklist[j]].dec_type==0)goto errout;
  }

  /* microVorbis: precompute the decode-time stagemasks/stagebooks (replaces
     vorbis_look_residue0's partbooks). Same partition-major, stage-minor
     order res0_look used to walk booklist in, so this consumes it
     identically; book numbers fit unsigned char because ci->books<=256 (8-bit
     read +1) and every booklist[] entry above is already validated <ci->books. */
  {
    int k,bookidx=0;
    info->stagemasks=(unsigned char *)
      _ogg_malloc(info->partitions*sizeof(*info->stagemasks));
    if(!info->stagemasks)goto errout;
    info->stagebooks=(unsigned char *)
      _ogg_malloc(info->partitions*8*sizeof(*info->stagebooks));
    if(!info->stagebooks)goto errout;

    for(j=0;j<info->partitions;j++){
      info->stagemasks[j]=(unsigned char)info->secondstages[j];
      for(k=0;k<8;k++){
        if(info->secondstages[j]&(1<<k)){
          info->stagebooks[(j<<3)+k]=(unsigned char)info->booklist[bookidx++];
          if(k+1>info->stages)info->stages=k+1;
        }else{
          info->stagebooks[(j<<3)+k]=0xff; /* unused; never read (mask bit clear) */
        }
      }
    }
  }

  /* verify the phrasebook is not specifying an impossible or
     inconsistent partitioning scheme. */
  /* modify the phrasebook ranging check from r16327; an early beta
     encoder had a bug where it used an oversized phrasebook by
     accident.  These files should continue to be playable, but don't
     allow an exploit */
  {
    int entries = ci->book_param[info->groupbook].entries;
    int dim = ci->book_param[info->groupbook].dim;
    int partvals = 1;
    if (dim<1) goto errout;
    while(dim>0){
      partvals *= info->partitions;
      if(partvals > entries) goto errout;
      dim--;
    }
    info->partvals = partvals;
  }

  return(info);
 errout:
  res0_free_info(info);
  return(NULL);
}

/* microVorbis: no look layer - res0_unpack already built everything decode
   needs (stagemasks/stagebooks/stages) into the vorbis_info_residue0 struct,
   so look is just the info pointer handed back. Matches the identity-look
   pattern the other backends move to (steps 2-3). */
vorbis_look_residue *res0_look(vorbis_dsp_state *vd,vorbis_info_mode *vm,
			  vorbis_info_residue *vr){
  (void)vd;(void)vm;
  return (vorbis_look_residue *)vr;
}

/* microVorbis: res0_look() no longer touches the DSP setup arena. */
static long res0_arena_size(vorbis_dsp_state *vd,vorbis_info_mode *vm,
			    vorbis_info_residue *vr){
  (void)vd;(void)vm;(void)vr;
  return 0;
}


/* a truncated packet here just means 'stop working'; it's not an error */
static int _01inverse(vorbis_block *vb,vorbis_look_residue *vl,
		      ogg_int32_t **in,int ch,
		      long (*decodepart)(codebook *, ogg_int32_t *,
					 oggpack_buffer *,int,int)){

  long i,j,k,l,s;
  vorbis_info_residue0 *info=(vorbis_info_residue0 *)vl;
  codec_setup_info     *ci=(codec_setup_info *)vb->vd->vi->codec_setup;
  codebook             *phrasebook=ci->book_param+info->groupbook;

  /* move all this setup out later */
  int samples_per_partition=info->grouping;
  int partitions_per_word=(int)phrasebook->dim;
  int max=vb->pcmend>>1;
  int end=(info->end<max?info->end:max);
  int n=end-info->begin;

  if(n>0){
    int partvals=n/samples_per_partition;
    int partwords=(partvals+partitions_per_word-1)/partitions_per_word;
    ARENA_STACK(unsigned char *, partword, ch, vb);

    for(j=0;j<ch;j++)
      partword[j]=(unsigned char *)_vorbis_block_alloc(vb,(long)partwords*partitions_per_word*sizeof(*partword[j]));

    for(s=0;s<info->stages;s++){

      /* each loop decodes on partition codeword containing
	 partitions_pre_word partitions */
      for(i=0,l=0;i<partvals;l++){
	long base=l*(long)partitions_per_word;
	if(s==0){
	  /* fetch the partition word for each channel and unpack it into
	     partitions_per_word class indices, most-significant digit first
	     (base info->partitions) - lowmem's classword arithmetic, replacing
	     the old decodemap[] lookup table. temp<info->partvals (checked
	     below) guarantees every digit lands in [0,info->partitions), so
	     the stagemasks/stagebooks index below is always in range. */
	  for(j=0;j<ch;j++){
	    int temp=vorbis_book_decode(phrasebook,&vb->opb);
	    long div;
	    if(temp==-1 || temp>=info->partvals)goto eopbreak;

	    div=1;
	    for(k=partitions_per_word-2;k>=0;k--)div*=info->partitions;
	    for(k=0;k<partitions_per_word;k++){
	      long q=temp/div;
	      partword[j][base+k]=(unsigned char)q;
	      temp-=(int)(q*div);
	      div/=info->partitions;
	    }
	  }
	}

	/* now we decode residual values for the partitions */
	for(k=0;k<partitions_per_word && i<partvals;k++,i++)
	  for(j=0;j<ch;j++){
	    long offset=info->begin+i*samples_per_partition;
	    int idx=partword[j][base+k];
	    if(info->stagemasks[idx]&(1<<s)){
	      codebook *stagebook=ci->book_param+info->stagebooks[(idx<<3)+s];
	      if(decodepart(stagebook,in[j]+offset,&vb->opb,
			    samples_per_partition,-8)==-1)goto eopbreak;
	    }
	  }
      }
    }
  }
 eopbreak:
  return(0);
}

int res0_inverse(vorbis_block *vb,vorbis_look_residue *vl,
		 ogg_int32_t **in,int *nonzero,int ch){
  int i,used=0;
  for(i=0;i<ch;i++)
    if(nonzero[i])
      in[used++]=in[i];
  if(used)
    return(_01inverse(vb,vl,in,used,vorbis_book_decodevs_add));
  else
    return(0);
}

int res1_inverse(vorbis_block *vb,vorbis_look_residue *vl,
		 ogg_int32_t **in,int *nonzero,int ch){
  int i,used=0;
  for(i=0;i<ch;i++)
    if(nonzero[i])
      in[used++]=in[i];
  if(used)
    return(_01inverse(vb,vl,in,used,vorbis_book_decodev_add));
  else
    return(0);
}

/* duplicate code here as speed is somewhat more important */
int res2_inverse(vorbis_block *vb,vorbis_look_residue *vl,
		 ogg_int32_t **in,int *nonzero,int ch){
  long i,k,l,s;
  vorbis_info_residue0 *info=(vorbis_info_residue0 *)vl;
  codec_setup_info     *ci=(codec_setup_info *)vb->vd->vi->codec_setup;
  codebook             *phrasebook=ci->book_param+info->groupbook;

  /* move all this setup out later */
  int samples_per_partition=info->grouping;
  int partitions_per_word=(int)phrasebook->dim;
  int max=(vb->pcmend*ch)>>1;
  int end=(info->end<max?info->end:max);
  int n=end-info->begin;

  if(n>0){

    int partvals=n/samples_per_partition;
    int partwords=(partvals+partitions_per_word-1)/partitions_per_word;
    unsigned char *partword=(unsigned char *)
      _vorbis_block_alloc(vb,(long)partwords*partitions_per_word*sizeof(*partword));
    int beginoff=info->begin/ch;

    for(i=0;i<ch;i++)if(nonzero[i])break;
    if(i==ch)return(0); /* no nonzero vectors */

    samples_per_partition/=ch;

    for(s=0;s<info->stages;s++){
      for(i=0,l=0;i<partvals;l++){
	long base=l*(long)partitions_per_word;

	if(s==0){
	  /* fetch the partition word and unpack it into partitions_per_word
	     class indices, most-significant digit first (base info->partitions)
	     - see _01inverse for the digit-decomposition rationale */
	  int temp=vorbis_book_decode(phrasebook,&vb->opb);
	  long div;
	  if(temp==-1 || temp>=info->partvals)goto eopbreak;

	  div=1;
	  for(k=partitions_per_word-2;k>=0;k--)div*=info->partitions;
	  for(k=0;k<partitions_per_word;k++){
	    long q=temp/div;
	    partword[base+k]=(unsigned char)q;
	    temp-=(int)(q*div);
	    div/=info->partitions;
	  }
	}

	/* now we decode residual values for the partitions */
	for(k=0;k<partitions_per_word && i<partvals;k++,i++){
	  int idx=partword[base+k];
	  if(info->stagemasks[idx]&(1<<s)){
	    codebook *stagebook=ci->book_param+info->stagebooks[(idx<<3)+s];

	    if(vorbis_book_decodevv_add(stagebook,in,
					i*samples_per_partition+beginoff,ch,
					&vb->opb,
					samples_per_partition,-8)==-1)
	      goto eopbreak;
	  }
	}
      }
    }
  }
 eopbreak:
  return(0);
}


vorbis_func_residue residue0_exportbundle={
  &res0_unpack,
  &res0_look,
  &res0_free_info,
  &res0_free_look,
  &res0_inverse,
  &res0_arena_size
};

vorbis_func_residue residue1_exportbundle={
  &res0_unpack,
  &res0_look,
  &res0_free_info,
  &res0_free_look,
  &res1_inverse,
  &res0_arena_size
};

vorbis_func_residue residue2_exportbundle={
  &res0_unpack,
  &res0_look,
  &res0_free_info,
  &res0_free_look,
  &res2_inverse,
  &res0_arena_size
};
