#!/usr/bin/env python3
"""Generate tests/data/modes3_badmode.ogg (see test_out_of_range_mode_rejected).

Crafts an Ogg Vorbis stream with a non-power-of-two mode count (modes=3) and
an audio packet encoding mode==3 (== ci->modes, one past the last valid
index). No real encoder emits more than 2 modes, so this cannot come from
generate_test_data.sh. With codec_setup_info's mode_param right-sized to
`modes`, an unguarded ci->mode_param[mode] access is a heap OOB read; the
synthesis.c bound must reject the packet with OV_EBADPACKET instead, and the
two valid mode-0 packets that follow decode to 128 samples of silence.

Usage: python3 craft_modes3_badmode.py [output.ogg]  (default: data/modes3_badmode.ogg)"""
import os, struct, sys

class BitWriter:  # oggpack: LSB-first
    def __init__(self):
        self.bits = []
    def w(self, value, nbits):
        for i in range(nbits):
            self.bits.append((value >> i) & 1)
    def bytes(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, b in enumerate(self.bits):
            out[i >> 3] |= b << (i & 7)
        return bytes(out)

def id_header():
    w = BitWriter()
    w.w(0x01, 8)
    for c in b"vorbis": w.w(c, 8)
    w.w(0, 32)        # version
    w.w(1, 8)         # channels
    w.w(8000, 32)     # rate
    w.w(0, 32); w.w(0, 32); w.w(0, 32)  # bitrates
    w.w(8, 4); w.w(8, 4)                # blocksizes 256/256
    w.w(1, 1)                           # framing
    return w.bytes()

def comment_header():
    w = BitWriter()
    w.w(0x03, 8)
    for c in b"vorbis": w.w(c, 8)
    w.w(0, 32)  # vendor len
    w.w(0, 32)  # comment count
    w.w(1, 1)   # framing
    return w.bytes()

def setup_header(modes=3):
    w = BitWriter()
    w.w(0x05, 8)
    for c in b"vorbis": w.w(c, 8)

    # --- codebooks: 2 ---
    w.w(1, 8)  # books-1
    # book 0: maptype 0 (residue groupbook), dim 1, entries 2, lens {1,1}
    w.w(0x564342, 24); w.w(1, 16); w.w(2, 24)
    w.w(0, 1)   # unordered
    w.w(0, 1)   # not sparse
    w.w(0, 5); w.w(0, 5)  # len-1 = 0,0
    w.w(0, 4)   # maptype 0
    # book 1: maptype 1 (floor0 book, needs dec_type!=0), dim 1, entries 2
    w.w(0x564342, 24); w.w(1, 16); w.w(2, 24)
    w.w(0, 1); w.w(0, 1)
    w.w(0, 5); w.w(0, 5)
    w.w(1, 4)    # maptype 1
    w.w(0, 32)   # q_min
    w.w(0, 32)   # q_delta
    w.w(0, 4)    # q_bits-1 -> q_bits=1
    w.w(0, 1)    # q_seq
    w.w(0, 1); w.w(0, 1)  # quantvals=2, 1 bit each

    # --- times: 1 ---
    w.w(0, 6)
    w.w(0, 16)  # time type 0

    # --- floors: 1 (floor0) ---
    w.w(0, 6)
    w.w(0, 16)   # floor type 0
    w.w(1, 8)    # order
    w.w(8000, 16)# rate
    w.w(64, 16)  # barkmap
    w.w(6, 6)    # ampbits
    w.w(1, 8)    # ampdB
    w.w(0, 4)    # numbooks-1 -> 1
    w.w(1, 8)    # book 1

    # --- residues: 1 (res0) ---
    w.w(0, 6)
    w.w(0, 16)  # residue type 0
    w.w(0, 24)  # begin
    w.w(0, 24)  # end
    w.w(0, 24)  # grouping-1 -> 1
    w.w(0, 6)   # partitions-1 -> 1
    w.w(0, 8)   # groupbook 0
    w.w(0, 3)   # cascade low
    w.w(0, 1)   # no cascade high flag

    # --- maps: 1 (mapping0) ---
    w.w(0, 6)
    w.w(0, 16)  # map type 0
    w.w(0, 1)   # submaps flag -> 1 submap
    w.w(0, 1)   # coupling flag
    w.w(0, 2)   # reserved
    w.w(0, 8)   # time submap
    w.w(0, 8)   # floor submap
    w.w(0, 8)   # residue submap

    # --- modes ---
    w.w(modes - 1, 6)
    for _ in range(modes):
        w.w(0, 1)   # blockflag
        w.w(0, 16)  # windowtype
        w.w(0, 16)  # transformtype
        w.w(0, 8)   # mapping 0
    w.w(1, 1)  # framing
    return w.bytes()

def audio_packet(mode, modebits, extra_zero_bytes=1):
    w = BitWriter()
    w.w(0, 1)            # audio packet
    w.w(mode, modebits)  # mode index
    b = w.bytes()
    return b + b"\x00" * extra_zero_bytes

CRC_TABLE = []
for i in range(256):
    r = i << 24
    for _ in range(8):
        r = ((r << 1) ^ 0x04C11DB7) if (r & 0x80000000) else (r << 1)
        r &= 0xFFFFFFFF
    CRC_TABLE.append(r)

def ogg_crc(data):
    crc = 0
    for byte in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ CRC_TABLE[((crc >> 24) & 0xFF) ^ byte]
    return crc

def ogg_page(packets, serial, seq, granule, bos=False, eos=False):
    lacing = bytearray()
    body = bytearray()
    for p in packets:
        n = len(p)
        while n >= 255:
            lacing.append(255); n -= 255
        lacing.append(n)
        body += p
    hdr = b"OggS" + bytes([0, (bos and 2) | (eos and 4)])
    hdr += struct.pack("<q", granule) + struct.pack("<I", serial)
    hdr += struct.pack("<I", seq) + b"\x00\x00\x00\x00"
    hdr += bytes([len(lacing)]) + bytes(lacing)
    page = bytearray(hdr + body)
    crc = ogg_crc(page)
    page[22:26] = struct.pack("<I", crc)
    return bytes(page)

def main():
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "data", "modes3_badmode.ogg")
    out = sys.argv[1] if len(sys.argv) > 1 else default
    serial = 0x6D6F6465  # 'mode'
    modebits = 2         # ilog(3)
    pages = [
        ogg_page([id_header()], serial, 0, 0, bos=True),
        ogg_page([comment_header(), setup_header(3)], serial, 1, 0),
        # packet 1: mode==3 (out of range, must be rejected as OV_EBADPACKET)
        # packets 2,3: mode==0, valid; emit 128 samples of silence
        ogg_page([audio_packet(3, modebits),
                  audio_packet(0, modebits),
                  audio_packet(0, modebits)], serial, 2, 128, eos=True),
    ]
    with open(out, "wb") as f:
        for p in pages:
            f.write(p)
    print(f"wrote {out} ({sum(len(p) for p in pages)} bytes)")

if __name__ == "__main__":
    main()
