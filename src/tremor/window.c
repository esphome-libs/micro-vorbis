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

 function: window functions

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
#include "misc.h"
#include "window.h"
#include "window_lookup.h"

const void *_vorbis_window(int type, int left){

  switch(type){
  case 0:

    switch(left){
    case 32:
      return vwin64;
    case 64:
      return vwin128;
    case 128:
      return vwin256;
    case 256:
      return vwin512;
    case 512:
      return vwin1024;
    case 1024:
      return vwin2048;
    case 2048:
      return vwin4096;
    case 4096:
      return vwin8192;
    default:
      return(0);
    }
    break;
  default:
    return(0);
  }
}

void _vorbis_apply_window(ogg_int32_t *d,const void *window_p[2],
			  long *blocksizes,
			  int lW,int W,int nW){
  
  const LOOKUP_T *window[2];
  long n=blocksizes[W];
  long ln=blocksizes[lW];
  long rn=blocksizes[nW];

  long leftbegin=n/4-ln/4;
  long leftend=leftbegin+ln/2;

  long rightbegin=n/2+n/4-rn/4;
  long rightend=rightbegin+rn/2;
  
  int i,p;

  window[0] = (const LOOKUP_T *)window_p[0];
  window[1] = (const LOOKUP_T *)window_p[1];

  for(i=0;i<leftbegin;i++)
    d[i]=0;

  /* Left and right ramps: iterations are independent (d[i] depends only on
     d[i] and the window value), so they unroll cleanly to overlap the
     load/mulsh/store latencies. The hardware zero-overhead loop already
     removes per-iteration branch cost, so this only helps scheduling.
     Blocksizes are powers of two >=64, so the ramp lengths are multiples of
     4; the scalar tails below cover any remainder defensively. */
  {
    const LOOKUP_T *wl=window[lW];
    for(p=0;i+3<leftend;i+=4,p+=4){
      d[i]  =MULT31(d[i],  wl[p]);
      d[i+1]=MULT31(d[i+1],wl[p+1]);
      d[i+2]=MULT31(d[i+2],wl[p+2]);
      d[i+3]=MULT31(d[i+3],wl[p+3]);
    }
    for(;i<leftend;i++,p++)
      d[i]=MULT31(d[i],wl[p]);
  }

  {
    const LOOKUP_T *wn=window[nW];
    for(i=rightbegin,p=rn/2-1;i+3<rightend;i+=4,p-=4){
      d[i]  =MULT31(d[i],  wn[p]);
      d[i+1]=MULT31(d[i+1],wn[p-1]);
      d[i+2]=MULT31(d[i+2],wn[p-2]);
      d[i+3]=MULT31(d[i+3],wn[p-3]);
    }
    for(;i<rightend;i++,p--)
      d[i]=MULT31(d[i],wn[p]);
  }

  for(;i<n;i++)
    d[i]=0;
}
