# microVorbis - Internal Architecture

Internal documentation for developers working on the decoder internals. For the public API and usage guide, see the [root README](../README.md). For the full file-by-file changelog of the Tremor fork, see [tremor/CHANGES.md](tremor/CHANGES.md).

## Origins

The decode core is a fork of [Tremor](https://gitlab.xiph.org/xiph/tremor) (`libvorbisidec`), Xiph.Org's fixed-point Vorbis decoder, modified directly in `src/tremor/` (no submodule, no patch step). The fork combines two upstream branches: the tree structure and most files come from **master**, while the codebook subsystem uses the design from the **lowmem** branch, hardened and optimized further (see [Tremor Fork Changes](#tremor-fork-changes)). The decode-only subset of `bitwise.c` plus two headers were folded in from [libogg](https://gitlab.xiph.org/xiph/ogg), so no external libogg dependency exists. The `OggVorbisDecoder` wrapper and Vorbis header parser are original to this project.

## File Organization

### Public API

- `include/micro_vorbis/ogg_vorbis_decoder.h` - Public API: `OggVorbisDecoder` class, `PcmFormat`, `SpeakerRole`, result codes

### Wrapper

- `ogg_vorbis_decoder.cpp` - `OggVorbisDecoder` implementation: header state machine, Ogg demuxing glue, Tremor lifecycle (pimpl in `TremorState`), PCM output conversion (native copy, smart downmix, raw channel selection)
- `vorbis_header.{h,cpp}` - Standalone identification-header parser and packet-type checks (`0x01`/`0x03`/`0x05` + `"vorbis"` magic), used for validation and stream-info extraction before packets are handed to Tremor

### Tremor Fork (`tremor/`)

- `bitwise.c`, `ogg/ogg.h`, `ogg/os_types.h` - Ogg bitstream reader folded in from libogg (decode-only subset; Xtensa funnel-shift fast path)
- `info.c` - Header-packet unpacking (`vorbis_info`, codec setup), comment-packet skip
- `codebook.{h,c}` - Lowmem single-step codebook unpack, compact decode tables, vector decode helpers
- `block.{h,c}` - Block/DSP lifecycle, both arena allocators, `ARENA_STACK`, channel-keep mask, overlap-add state
- `synthesis.c` - Packet-to-block synthesis entry points
- `mapping0.c` - Mapping decode: drives floor/residue/coupling/iMDCT per channel
- `floor0.c`, `floor1.c` - Floor curve reconstruction
- `res012.c` - Residue decode (all three residue types)
- `mdct.c`, `window.c` (+ `*_lookup.h`) - Inverse MDCT and window overlap
- `registry.{c,h}`, `backends.h`, `codec_internal.h` - Backend vtables and internal codec-setup structures
- `custom_allocator.h` - ESP-IDF `heap_caps_*` allocator overrides with Kconfig placement policies
- `os.h`, `misc.h` - Toolchain glue and fixed-point math primitives

## Decode Flow

### Wrapper State Machine

`OggVorbisDecoder::decode()` feeds input through `micro_ogg::OggDemuxer` and dispatches packets by state:

```text
STATE_EXPECT_IDENTIFICATION ──→ STATE_EXPECT_COMMENT ──→ STATE_STREAMING_COMMENT ──→ STATE_EXPECT_SETUP ──→ STATE_DECODING
```

- **STATE_EXPECT_IDENTIFICATION**: Validates BOS placement and granule position, parses the identification header with `parse_vorbis_identification()`, feeds it to Tremor, and resolves the output channel count (selection > downmix request > file's count)
- **STATE_EXPECT_COMMENT**: Waits for the comment header. On the first decode call that supplies comment-packet data, the magic accumulator is reset and the state advances to STATE_STREAMING_COMMENT
- **STATE_STREAMING_COMMENT**: The comment packet (which can carry arbitrarily large embedded data, e.g. cover art) is streamed past via the demuxer's `get_next_data()` without ever buffering it. Only the 7-byte magic is accumulated for validation; a 16-byte synthetic empty comment packet is then fed to Tremor to satisfy its header-ordering check
- **STATE_EXPECT_SETUP**: Feeds the setup header to Tremor, builds the channel-keep mask when raw selection is active, and initializes the DSP state and block. Returns `STREAM_INFO_READY`
- **STATE_DECODING**: Each audio packet runs `vorbis_synthesis()`, `vorbis_synthesis_blockin()`, then `vorbis_synthesis_pcmout()`, and the wrapper converts planar fixed-point PCM to interleaved `int16_t`

All resource allocation is deferred to `decode()` (demuxer buffers on the first call, Tremor state during header processing), so construction never fails; an allocation failure surfaces as `ERROR_ALLOCATION_FAILED` and is recovered with `reset()`. If the output buffer is too small for a decoded block, the PCM stays inside the DSP state, `has_pending_pcm_` is set, and the next call drains it without consuming input. Start priming and end-of-stream granule trimming happen inside Tremor; the wrapper keeps no granule bookkeeping.

### Tremor Synthesis Pipeline

```text
packet ──→ vorbis_synthesis ──────→ vorbis_synthesis_blockin ──→ vorbis_synthesis_pcmout
           (block arena reset,       (overlap-add into DSP        (planar int32 PCM,
            mode/window select,       PCM history, lag/trim)       s7.24 fixed point)
            mapping0_inverse:
            floor ─ residue ─
            coupling ─ iMDCT)
```

## Output Conversion

Tremor emits planar 32-bit PCM in s7.24 fixed point (full scale ±1.0 = ±2^24, so `>> 9` lands in int16 range); the wrapper produces interleaved 16-bit PCM via one of three paths in `output_pcm()`:

- **Native copy** (output channels == stream channels): per-plane 4-sample unrolled loop; adds a half-LSB bias before the truncating `>> 9` so conversion rounds to nearest, then saturates with `clip_to_16()`
- **Smart downmix** (constructor `channels` = 1 or 2): `downmix_stereo()` folds any 1-8 channel Vorbis layout (spec channel order, section 4.3.9) to a stereo pair per ITU-R BS.775: fronts weighted 1.0, center/surround 0.7071 (-3 dB), LFE dropped, then a per-layout normalization gain `1/(sum of one output's weights)` so a full-scale mix cannot clip. The mix runs in two `mulhi()` stages (high word of a 32x32 multiply, a single `MULSH` on Xtensa): Q27 weights produce Q19 terms summed unclamped in int32 (4 guard bits below the output LSB), then a Q28 gain lands the sum in 16-bit range. Mono and stereo sources keep a one-stage Q23 unity form whose truncation matches the native copy's `>> 9`. Mono output is the rounded half-sum of the stereo pair
- **Raw channel selection** (role-array constructor): each output channel copies exactly one source plane at unity gain via `role_to_plane()`, emitting silence for roles absent from the file's layout. Selection also feeds a channel-keep mask into Tremor so unkept planes are never synthesized or allocated (see below)
- **16-bit saturation** (`clip_to_16()`): uses the `clamps` instruction on Xtensa targets (single-cycle), a compare/branch fallback elsewhere

## Tremor Fork Changes

Summary of the fork's changes relative to upstream; [tremor/CHANGES.md](tremor/CHANGES.md) has the complete file-by-file accounting.

### Lowmem Codebook Subsystem

Upstream master unpacks codebooks in two steps: a heap `static_codebook` first, then a `codebook` with a precomputed `valuelist` and first-table acceleration. The fork instead uses the lowmem-branch design, because codebooks dominate the decoder's long-lived heap footprint:

- `vorbis_book_unpack` parses the header and builds the final decode structures in a single pass; `static_codebook` and `sharedbook.c` are gone, and the bitstream `lengthlist` exists only transiently during unpack
- The Huffman tree is a packed `dec_table` with per-book node size (1/2/4 bytes), decoded by a tree walk; there are no first-table acceleration arrays
- There is no precomputed `valuelist`; quantized values are dequantized on the fly during residue decode from `q_min`/`q_del`/`q_bits`/`q_pack` computed once at unpack

### Memory Architecture

Upstream Tremor does hundreds of small heap allocations per stream and more per frame. The fork consolidates the decoder into three memory pools, each with its own ESP-IDF placement policy (`custom_allocator.h` + Kconfig):

1. **Block arena** (`vorbis_block`): a single pre-sized arena (`arena_data`/`arena_capacity`/`arena_used`) replaces upstream's chained per-frame allocator. `_vorbis_arena_compute_size()` in `block.c` sizes it once from the codec setup; `_vorbis_block_ripcord()` resets it each frame by zeroing `arena_used`. The `ARENA_STACK` macro replaces decode-path `alloca` use in `mapping0.c` and `res012.c`. The alloc/ripcord helpers are `static inline` in `block.h` so the hot path inlines them
2. **DSP setup arena** (`vorbis_dsp_state`): upstream's original DSP init performed ~240 separate allocations on a typical stereo stream (private state, pointer arrays, per-channel PCM history, every floor/residue/mapping lookup). The fork computes the total up front through an `arena_size()` callback on each backend vtable that mirrors its `*_look` allocations exactly, then carves everything, including the PCM history buffers, from one allocation. `vorbis_dsp_clear` is a single free; allocation failure is one checkable point
3. **Codebook heap** (`vorbis_info`): the long-lived decode tables go through dedicated `_ogg_codebook_malloc`/`_ogg_codebook_calloc` macros with an independent Kconfig placement policy defaulting to prefer-internal-RAM, because they are read with random-access patterns on the hot path. Everything else defaults to prefer-PSRAM (without `CONFIG_SPIRAM`, all three pools default to internal-only)

Two related decoder-slimming changes:

- **Comment machinery removed**: the `vorbis_comment` struct, its init/clear/query API, and its per-stream allocations are gone. The decoder validates and skips the comment packet; the wrapper streams the real packet past without buffering (see the state machine above). Comment retrieval is intentionally unsupported
- **Per-channel decode mask**: `vorbis_synthesis_init_ex()` fixes a channel-keep mask before the arena is sized, so when the wrapper's raw channel selection is active, unkept channels skip floor-apply, the inverse MDCT (the dominant cost), windowing, and overlap-add, and their PCM history buffers are never allocated. Entropy decode and channel coupling still run for all channels (residue bits are interleaved and coupling mixes channel pairs, so neither is skippable), which keeps kept channels bit-exact versus a full decode

### Bitstream Hardening

The lowmem branch trusts the bitstream in places where a crafted setup packet can smash the stack, overflow, or hang, which would be fatal on a 4-8 KB embedded task stack. The fork (in part porting upstream libvorbis fixes from May 2026):

- Eliminates all setup-path `alloca`s in `vorbis_book_unpack` (the transient `lengthlist` can reach 16 MB from a 24-bit entries field) in favor of checked heap allocations freed on every exit path
- Replaces the decode-path `alloca(4*dim)` vector scratch (up to 256 KB from a crafted `dim`) with a fixed 32-value stack buffer plus checked heap fallback; real encoders emit `dim <= 8`
- NULL-checks every codebook allocation and fails through `vorbis_book_clear`/the header-error path, since a hostile header can legally demand multi-megabyte tables that fail to allocate on embedded targets
- Validates the bitstream up front: `dim < 1` rejected, ordered-codebook length/count plausibility checks, `_make_words` bounds-checked against its work-buffer size, `_book_maptype1_quantvals` saturated against overflow-driven hangs, and floor 0 setup fields range-checked
- Emulates `oggpack_eop()` (which the lowmem codebook code expects from its chained-buffer bitreader) as a `static inline` over the flat-buffer reader's overrun signal (`b->ptr == NULL`); a read landing exactly on the last bit is not flagged as an error, matching upstream semantics

### Undefined-Behavior Cleanups

- Left shifts of negative fixed-point values (well-defined on every supported target, but UB per the C standard and flagged by UBSan) were rewritten as unsigned shifts cast back; the result is bit-identical on two's-complement machines. Fixed in `misc.h` (`MULT31`), `codebook.c`, `floor0.c`, and `floor1.c`
- Bitstream-derived shift counts are bounds-checked before use: dequantization shifts rejected when `|shift| >= 32` (checked once per vector batch, not per vector), `q_pack * dim > 32` rejected at unpack, floor 0 `ampbits` confined to `[1, 31]` at setup, and `vorbis_invsqlook_i` guards its computed exponent shift

### Performance

- **Hoisted dequantization invariants**: upstream lowmem recomputes the add/shift/mask constants for every decoded vector. The fork splits this into `decode_map_ctx_init` (once per `vorbis_book_decodev*` call; also where the shift validation lives) and `decode_map_apply` (the per-vector hot path), with the byte-vs-short branch hoisted out of the inner loop
- **Inlined arena helpers**: block-arena alloc/reset are `static inline` rather than upstream's extern functions

### Portability and Housekeeping

`<math.h>` and float helpers dropped; `ogg/os_types.h`'s legacy-compiler ladder replaced with `<stdint.h>`; `CLIP_TO_15` removed (saturation lives in the wrapper); const-correctness restored on the `LOOKUP_T` tables.

## Xtensa Optimizations

Built on Xtensa targets (ESP32 / ESP32-S2 / ESP32-S3). The assembly paths are guarded by the compiler-defined `#ifdef __XTENSA__`:

- **Funnel-shift bit reads** (`tremor/bitwise.c`): `oggpack_read` and `oggpack_look` use two aligned 32-bit loads plus the `SRC` funnel-shift instruction on the fast path (`endbyte < storage - 7`), replacing upstream's cascading if-chain of byte loads. The `- 7` margin (vs the scalar path's `- 4`) keeps the fast path's read of up to `ptr + 7` within `storage - 1`
- **Saturating clip** (`ogg_vorbis_decoder.cpp`): `clip_to_16()` uses the `clamps` instruction for single-cycle 16-bit saturation

In addition, `mulhi()` in `ogg_vorbis_decoder.cpp` is plain C (`(int64_t)s * m >> 32`) written so the Xtensa compiler lowers it to a single `MULSH` instruction; it needs no guard.

## References

- [Vorbis I Specification](https://xiph.org/vorbis/doc/Vorbis_I_spec.html)
- [Tremor (upstream)](https://gitlab.xiph.org/xiph/tremor)
- [libogg (upstream)](https://gitlab.xiph.org/xiph/ogg)
- [Xtensa ISA Reference](https://www.cadence.com/content/dam/cadence-www/global/en_US/documents/tools/ip/tensilica-ip/isa-summary.pdf)
