/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2007             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: toplevel libogg include

 Trimmed to the decode subset Tremor uses: the oggpack_buffer
 bit-unpacker, the read-side oggpack_* readers in bitwise.c, and the
 ogg_packet type. The encode bitpacker, the oggpackB_* family, and the
 ogg_page/ogg_stream/ogg_sync framing API are dropped. Framing comes from
 the external micro-ogg-demuxer.

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
#ifndef _OGG_H
#define _OGG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <ogg/os_types.h>

typedef struct {
  long endbyte;
  int  endbit;

  unsigned char *buffer;
  unsigned char *ptr;
  long storage;
} oggpack_buffer;

/* ogg_packet is used to encapsulate the data and metadata belonging
   to a single raw Ogg/Vorbis packet *************************************/

typedef struct {
  unsigned char *packet;
  long  bytes;
  long  b_o_s;
  long  e_o_s;

  ogg_int64_t  granulepos;

  ogg_int64_t  packetno;     /* sequence number for decode; the framing
                                knows where there's a hole in the data,
                                but we need coupling so that the codec
                                (which is in a separate abstraction
                                layer) also knows about the gap */
} ogg_packet;

/* Ogg BITSTREAM PRIMITIVES: decode-side bit unpacker (bitwise.c) ********/

extern void  oggpack_readinit(oggpack_buffer *b,unsigned char *buf,int bytes);
extern long  oggpack_look(oggpack_buffer *b,int bits);
extern void  oggpack_adv(oggpack_buffer *b,int bits);
extern long  oggpack_read(oggpack_buffer *b,int bits);
extern long  oggpack_bytes(oggpack_buffer *b);

#ifdef __cplusplus
}
#endif

#endif  /* _OGG_H */
