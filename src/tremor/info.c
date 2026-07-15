/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis 'TREMOR' CODEC SOURCE CODE.   *
 *                                                                  *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis 'TREMOR' SOURCE CODE IS (C) COPYRIGHT 1994-2003    *
 * BY THE Xiph.Org FOUNDATION http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: maintain the info structure, info <-> header packets

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

/* general handling of the header and the vorbis_info structure (and
   substructures) */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ogg/ogg.h>
#include "ivorbiscodec.h"
#include "codec_internal.h"
#include "codebook.h"
#include "registry.h"
#include "window.h"
#include "misc.h"

/* helpers */
static void _v_readstring(oggpack_buffer *o,char *buf,int bytes){
  while(bytes--){
    *buf++=oggpack_read(o,8);
  }
}

/* The microVorbis fork drops the public vorbis_comment API entirely. The
   decoder never exposes comment data (the OggVorbisDecoder wrapper streams the
   real comment packet past without buffering and feeds a synthetic empty one),
   so the comment struct, its init/clear, and the tag-lookup API
   (vorbis_comment_query / _query_count and their tagcompare / _v_toupper
   helpers) were all removed. The only state the decoder needs is a single
   "comment header seen" bit, tracked by vi->comment_header_seen so the
   header-sequence check in vorbis_synthesis_headerin still enforces ordering.
   See _vorbis_unpack_comment below, which validates and skips the packet. */

/* used by synthesis, which has a full, alloced vi */
void vorbis_info_init(vorbis_info *vi){
  memset(vi,0,sizeof(*vi));
  vi->codec_setup=(codec_setup_info *)_ogg_calloc(1,sizeof(codec_setup_info));
}

void vorbis_info_clear(vorbis_info *vi){
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;
  int i;

  if(ci){

    /* unpack allocates each section's table(s) before setting its count, so
       a nonzero count implies non-NULL tables; the pointer guards below are
       defensive. _ogg_free(NULL) is a no-op. */
    if(ci->mode_param){
      for(i=0;i<ci->modes;i++)
	if(ci->mode_param[i])_ogg_free(ci->mode_param[i]);
      _ogg_free(ci->mode_param);
    }

    if(ci->map_param){ /* unpack does the range checking */
      for(i=0;i<ci->maps;i++)
	if(ci->map_param[i])
	  _mapping_P[ci->map_type[i]]->free_info(ci->map_param[i]);
      _ogg_free(ci->map_param);
    }
    _ogg_free(ci->map_type);

    if(ci->floor_param){ /* unpack does the range checking */
      for(i=0;i<ci->floors;i++)
	if(ci->floor_param[i])
	  _floor_P[ci->floor_type[i]]->free_info(ci->floor_param[i]);
      _ogg_free(ci->floor_param);
    }
    _ogg_free(ci->floor_type);

    if(ci->residue_param){ /* unpack does the range checking */
      for(i=0;i<ci->residues;i++)
	if(ci->residue_param[i])
	  _residue_P[ci->residue_type[i]]->free_info(ci->residue_param[i]);
      _ogg_free(ci->residue_param);
    }
    _ogg_free(ci->residue_type);

    _ogg_free(ci->time_type);

    if(ci->book_param){
      for(i=0;i<ci->books;i++)
	vorbis_book_clear(ci->book_param+i);
      _ogg_free(ci->book_param);
    }

    _ogg_free(ci);
  }

  memset(vi,0,sizeof(*vi));
}

/* Header packing/unpacking ********************************************/

static int _vorbis_unpack_info(vorbis_info *vi,oggpack_buffer *opb){
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;
  if(!ci)return(OV_EFAULT);

  vi->version=oggpack_read(opb,32);
  if(vi->version!=0)return(OV_EVERSION);

  vi->channels=oggpack_read(opb,8);
  vi->rate=oggpack_read(opb,32);

  vi->bitrate_upper=(ogg_int32_t)oggpack_read(opb,32);
  vi->bitrate_nominal=(ogg_int32_t)oggpack_read(opb,32);
  vi->bitrate_lower=(ogg_int32_t)oggpack_read(opb,32);

  {
    /* Reject a truncated header before shifting: oggpack_read returns the -1
       EOP sentinel on overrun, and 1<<-1 is undefined. Mirrors floor1.c's
       rangebits guard; the EOP check below only fires after the shift. */
    int bs0=oggpack_read(opb,4);
    int bs1=oggpack_read(opb,4);
    if(bs0<0||bs1<0)goto err_out;
    ci->blocksizes[0]=1<<bs0;
    ci->blocksizes[1]=1<<bs1;
  }

  if(vi->rate<1)goto err_out;
  if(vi->channels<1)goto err_out;
  if(ci->blocksizes[0]<64)goto err_out; 
  if(ci->blocksizes[1]<ci->blocksizes[0])goto err_out;
  if(ci->blocksizes[1]>8192)goto err_out;
  
  if(oggpack_read(opb,1)!=1)goto err_out; /* EOP check */

  return(0);
 err_out:
  vorbis_info_clear(vi);
  return(OV_EBADHEADER);
}

/* The decoder discards Vorbis comments entirely, so this validates and skips
   the comment packet without allocating or storing anything. The caller
   (vorbis_synthesis_headerin) records that the comment header was seen. */
static int _vorbis_unpack_comment(oggpack_buffer *opb){
  int i;
  int vendorlen;
  int comments;

  vendorlen=oggpack_read(opb,32);
  if(vendorlen<0)return(OV_EBADHEADER);
  if(vendorlen>opb->storage-oggpack_bytes(opb))return(OV_EBADHEADER);
  /* skip the vendor string; the decoder never exposes it */
  oggpack_adv(opb,(long)vendorlen*8);

  comments=oggpack_read(opb,32);
  if(comments<0||comments>=INT_MAX||comments>(opb->storage-oggpack_bytes(opb))>>2)return(OV_EBADHEADER);

  /* skip each user comment without allocating; the decoder never exposes them */
  for(i=0;i<comments;i++){
    int len=oggpack_read(opb,32);
    if(len<0||len>opb->storage-oggpack_bytes(opb))return(OV_EBADHEADER);
    oggpack_adv(opb,(long)len*8);
  }
  if(oggpack_read(opb,1)!=1)return(OV_EBADHEADER); /* EOP check */

  return(0);
}

/* all of the real encoding details are here.  The modes, books,
   everything */
static int _vorbis_unpack_books(vorbis_info *vi,oggpack_buffer *opb){
  codec_setup_info     *ci=(codec_setup_info *)vi->codec_setup;
  int i;

  /* codebooks */
  ci->books=oggpack_read(opb,8)+1;
  if(ci->books<=0)goto err_out;
  ci->book_param=(codebook *)_ogg_codebook_calloc(ci->books,sizeof(*ci->book_param));
  if(!ci->book_param)goto err_out;
  for(i=0;i<ci->books;i++){
    if(vorbis_book_unpack(opb,ci->book_param+i))goto err_out;
  }

  /* Each section below reads its count into a local, validates it against
     VI_SETUP_MAX (the old fixed-array size; the 6-bit read +1 can't exceed
     it, but don't trust the bit width alone), allocates the section's
     table(s) zeroed, and only then publishes the count. vorbis_info_clear
     iterates the counts and indexes both the type and param tables, so both
     must exist before the count is nonzero. */

  /* time backend settings */
  {
    int n=oggpack_read(opb,6)+1;
    if(n<1 || n>VI_SETUP_MAX)goto err_out;
    ci->time_type=(int *)_ogg_calloc(n,sizeof(*ci->time_type));
    if(!ci->time_type)goto err_out;
    ci->times=n;
  }
  for(i=0;i<ci->times;i++){
    ci->time_type[i]=oggpack_read(opb,16);
    if(ci->time_type[i]<0 || ci->time_type[i]>=VI_TIMEB)goto err_out;
    /* ci->time_param[i]=_time_P[ci->time_type[i]]->unpack(vi,opb);
       Vorbis I has no time backend */
    /*if(!ci->time_param[i])goto err_out;*/
  }

  /* floor backend settings */
  {
    int n=oggpack_read(opb,6)+1;
    if(n<1 || n>VI_SETUP_MAX)goto err_out;
    ci->floor_type=(int *)_ogg_calloc(n,sizeof(*ci->floor_type));
    ci->floor_param=(vorbis_info_floor **)
      _ogg_calloc(n,sizeof(*ci->floor_param));
    if(!ci->floor_type || !ci->floor_param)goto err_out;
    ci->floors=n;
  }
  for(i=0;i<ci->floors;i++){
    ci->floor_type[i]=oggpack_read(opb,16);
    if(ci->floor_type[i]<0 || ci->floor_type[i]>=VI_FLOORB)goto err_out;
    ci->floor_param[i]=_floor_P[ci->floor_type[i]]->unpack(vi,opb);
    if(!ci->floor_param[i])goto err_out;
  }

  /* residue backend settings */
  {
    int n=oggpack_read(opb,6)+1;
    if(n<1 || n>VI_SETUP_MAX)goto err_out;
    ci->residue_type=(int *)_ogg_calloc(n,sizeof(*ci->residue_type));
    ci->residue_param=(vorbis_info_residue **)
      _ogg_calloc(n,sizeof(*ci->residue_param));
    if(!ci->residue_type || !ci->residue_param)goto err_out;
    ci->residues=n;
  }
  for(i=0;i<ci->residues;i++){
    ci->residue_type[i]=oggpack_read(opb,16);
    if(ci->residue_type[i]<0 || ci->residue_type[i]>=VI_RESB)goto err_out;
    ci->residue_param[i]=_residue_P[ci->residue_type[i]]->unpack(vi,opb);
    if(!ci->residue_param[i])goto err_out;
  }

  /* map backend settings */
  {
    int n=oggpack_read(opb,6)+1;
    if(n<1 || n>VI_SETUP_MAX)goto err_out;
    ci->map_type=(int *)_ogg_calloc(n,sizeof(*ci->map_type));
    ci->map_param=(vorbis_info_mapping **)
      _ogg_calloc(n,sizeof(*ci->map_param));
    if(!ci->map_type || !ci->map_param)goto err_out;
    ci->maps=n;
  }
  for(i=0;i<ci->maps;i++){
    ci->map_type[i]=oggpack_read(opb,16);
    if(ci->map_type[i]<0 || ci->map_type[i]>=VI_MAPB)goto err_out;
    ci->map_param[i]=_mapping_P[ci->map_type[i]]->unpack(vi,opb);
    if(!ci->map_param[i])goto err_out;
  }
  
  /* mode settings */
  {
    int n=oggpack_read(opb,6)+1;
    if(n<1 || n>VI_SETUP_MAX)goto err_out;
    ci->mode_param=(vorbis_info_mode **)
      _ogg_calloc(n,sizeof(*ci->mode_param));
    if(!ci->mode_param)goto err_out;
    ci->modes=n;
  }
  for(i=0;i<ci->modes;i++){
    ci->mode_param[i]=(vorbis_info_mode *)_ogg_calloc(1,sizeof(*ci->mode_param[i]));
    if(!ci->mode_param[i])goto err_out;
    ci->mode_param[i]->blockflag=oggpack_read(opb,1);
    ci->mode_param[i]->windowtype=oggpack_read(opb,16);
    ci->mode_param[i]->transformtype=oggpack_read(opb,16);
    ci->mode_param[i]->mapping=oggpack_read(opb,8);

    if(ci->mode_param[i]->windowtype>=VI_WINDOWB)goto err_out;
    if(ci->mode_param[i]->transformtype>=VI_WINDOWB)goto err_out;
    if(ci->mode_param[i]->mapping>=ci->maps)goto err_out;
    if(ci->mode_param[i]->mapping<0)goto err_out;
  }
  
  if(oggpack_read(opb,1)!=1)goto err_out; /* top level EOP check */

  return(0);
 err_out:
  vorbis_info_clear(vi);
  return(OV_EBADHEADER);
}

/* The Vorbis header is in three packets; the initial small packet in
   the first page that identifies basic parameters, a second packet
   with bitstream comments and a third packet that holds the
   codebook. */

int vorbis_synthesis_headerin(vorbis_info *vi,ogg_packet *op){
  oggpack_buffer opb;
  
  if(op){
    oggpack_readinit(&opb,op->packet,op->bytes);

    /* Which of the three types of header is this? */
    /* Also verify header-ness, vorbis */
    {
      char buffer[6];
      int packtype=oggpack_read(&opb,8);
      memset(buffer,0,6);
      _v_readstring(&opb,buffer,6);
      if(memcmp(buffer,"vorbis",6)){
	/* not a vorbis header */
	return(OV_ENOTVORBIS);
      }
      switch(packtype){
      case 0x01: /* least significant *bit* is read first */
	if(!op->b_o_s){
	  /* Not the initial packet */
	  return(OV_EBADHEADER);
	}
	if(vi->rate!=0){
	  /* previously initialized info header */
	  return(OV_EBADHEADER);
	}

	return(_vorbis_unpack_info(vi,&opb));

      case 0x03: /* least significant *bit* is read first */
	if(vi->rate==0){
	  /* um... we didn't get the initial header */
	  return(OV_EBADHEADER);
	}
        if(vi->comment_header_seen){
          /* previously initialized comment header */
          return(OV_EBADHEADER);
        }
        {
          int ret=_vorbis_unpack_comment(&opb);
          if(ret==0)vi->comment_header_seen=1;
          return(ret);
        }

      case 0x05: /* least significant *bit* is read first */
	if(vi->rate==0 || !vi->comment_header_seen){
	  /* um... we didn;t get the initial header or comments yet */
	  return(OV_EBADHEADER);
	}
        if(vi->codec_setup==NULL){
          /* improperly initialized vorbis_info */
          return(OV_EFAULT);
        }
        if(((codec_setup_info *)vi->codec_setup)->books>0){
          /* previously initialized setup header */
          return(OV_EBADHEADER);
        }

	return(_vorbis_unpack_books(vi,&opb));

      default:
	/* Not a valid vorbis header type */
	return(OV_EBADHEADER);
	break;
      }
    }
  }
  return(OV_EBADHEADER);
}

