# Modifications to Upstream Tremor

This directory is a fork of [Tremor](https://gitlab.xiph.org/xiph/tremor)
(aka `libvorbisidec`), Xiph.Org's fixed-point Vorbis decoder. The original
source is licensed under a BSD-3-Clause-style license; see [COPYING](COPYING).

The fork draws from two upstream branches. The **codebook subsystem** and
the **synthesis/MDCT subsystem** (`mdct.c`, and the buffer/readout logic in
`block.c`, `synthesis.c`, and `mapping0.c`) were replaced with designs from
upstream's **lowmem** branch - single-step `vorbis_book_unpack` and compact
decode tables (no `static_codebook`) for codebooks; the half-block backward
transform and `work`/`mdctright` readout buffers for synthesis - and then
each was modified further. The remaining tree structure and most other
files (`registry.c`, the backend vtables, `info.c`, the floor/residue
backends) come from the **master** branch. The sections below describe the
fork's changes relative to upstream.

## Dead-Code Policy

Files here are edited in place, and upstream security fixes are ported by
diffing against the original Tremor / libvorbis sources (e.g. libvorbis
`28965ede` / `a629068d`). A file's diffability against upstream is therefore a
maintenance asset, and it decides how aggressively unreachable code is pruned:

- **Files kept byte-identical to upstream** (`registry.{c,h}`, the
  `*_lookup.h` tables) are left verbatim, dead code and all. `synthesis.c`
  sat in this set until the `codec_setup_info` right-sizing forced a
  packet-supplied mode-index bound into it (see Memory Management), and it
  briefly diverged by only those two guard lines. The lowmem synthesis port
  (see Synthesis / MDCT Subsystem) moved it further still - `_vorbis_synthesis1`
  now also saves each kept channel's previous-block iMDCT tail
  (`mdct_shift_right`) before the mapping inverse - so the diff against
  upstream master is no longer near-zero and the "near-zero diff keeps
  porting cheap" rationale for keeping dead code no longer holds.
  `vorbis_synthesis_trackonly`, `vorbis_packet_blocksize`, and the
  `decodep == 0` arm of `_vorbis_synthesis1` that only `trackonly` reaches
  stay in the file regardless: they are kept as documented API surface
  (fast-forward without decode; blocksize lookup for seeking) rather than
  for diff-cheapness, even though nothing in-tree calls them. That code
  still reports 0% in the fuzzer coverage report by design; it is not a
  coverage gap to chase.
- **Files already forked in place** (`info.c`, `block.c`, the floor/residue/
  mapping backends, `window.c`, ...) have no clean upstream diff left to
  protect, so unreachable code in them is removed. This is why the
  comment-query API, `sharedbook.c`, the encode-only entry points, the
  standalone unused primitives `vorbis_synthesis_init`,
  `vorbis_synthesis_idheader`, and `vorbis_info_blocksize`, and (once the
  lowmem synthesis port left it with no callers) `_vorbis_apply_window` are
  gone.
- **Upstream-defined shared interfaces** are the exception inside forked files.
  The backend vtables in `backends.h` (wired by `registry.c`) keep a slot for
  every callback, so the `*_free_look` hooks survive as documented no-ops (the
  DSP arena frees everything in one shot) rather than NULL slots that would need
  a call-site guard upstream does not have.

## Structural Changes

- **Folded in from libogg**: `bitwise.c` and the minimal headers `ogg/ogg.h`
  and `ogg/os_types.h` were copied from
  [libogg](https://gitlab.xiph.org/xiph/ogg) into this fork.
  - `bitwise.c` was stripped to the decode-only subset Tremor uses. All
    write-side functions (`oggpack_writeinit`, `oggpack_write`,
    `oggpackB_write`, etc.) and the `_V_SELFTEST` block were removed.
  - `ogg/os_types.h` was simplified. The ~130-line platform-specific
    `#ifdef` ladder covering legacy compilers was replaced with a plain
    `#include <stdint.h>` and direct `typedef`s for the embedded
    toolchains this project supports.
  - `ogg/ogg.h` was stripped to the decode-only subset Tremor uses: the
    `oggpack_buffer` bit-unpacker, the five read-side `oggpack_*` readers
    implemented in `bitwise.c` (`readinit`, `look`, `adv`, `read`, `bytes`),
    and the `ogg_packet` type. The write-side bitpacker, the `oggpackB_*`
    family, and the Ogg page/stream/sync framing API were removed (framing
    is handled by the external micro-ogg-demuxer).

## Codebook Subsystem (from the lowmem branch)

The master branch unpacks codebooks in two steps: `vorbis_staticbook_unpack`
builds a heap `static_codebook`, then `vorbis_book_init_decode` in
`sharedbook.c` builds a `codebook` with `valuelist` and first-table
acceleration. The fork replaces this with the upstream **lowmem** branch
design:

- **`sharedbook.c` is deleted.** `vorbis_book_unpack` in `codebook.c` parses
  the codebook header and builds the final decode structures in a single
  pass; no `static_codebook` ever exists, and the bitstream `lengthlist` is
  only needed transiently during unpack.
- **Compact decode tables.** The Huffman tree is stored in a packed
  `dec_table` with per-book node size (`dec_nodeb`: 1/2/4 bytes) and leaf
  width (`dec_leafw`), decoded by a tree-walk in
  `decode_packed_entry_number` (no first-table acceleration, no
  `dec_firsttablen`). Quantized values are dequantized on the fly during
  residue decode (`dec_type` 1/2/3) from `q_min`/`q_del`/`q_bits`/`q_pack`
  fields computed once at unpack; there is no precomputed `valuelist`.
- **`codec_internal.h`**: `static_codebook *book_param[256]` and
  `codebook *fullbooks` are replaced by a single `codebook *book_param`
  flat array, heap-allocated in `_vorbis_unpack_books` (`info.c`). The rest
  of `codec_setup_info` keeps its master-branch shape (fixed-size
  mode/map/floor/residue param arrays, `passlimit`, `coupling_passes`).
- **`oggpack_eop` emulation** (`codebook.c`): the lowmem branch's codebook
  code calls `oggpack_eop()` from its chained-buffer bitreader, which our
  libogg flat-buffer `bitwise.c` does not provide. It is emulated as a
  `static inline` in `codebook.c` that reports end-of-packet when
  `b->ptr == NULL`, the flat-buffer overrun signal set by `oggpack_read`
  and `oggpack_adv` when a read goes *past* the end. A read that lands
  exactly on the last bit is not an error, matching the upstream
  `headend < 0` semantics. Used at three header-parse sites in
  `vorbis_book_unpack` and once on the residue decode path in
  `decode_map_apply`.

### Hardening relative to the lowmem branch

The lowmem branch trusts the bitstream in several places where a crafted
setup packet can smash the stack, overflow, or hang. The fork adds the
following checks (in part porting upstream libvorbis commits `28965ede`
and `a629068d`, May 2026):

- **Setup-path `alloca` eliminated in `vorbis_book_unpack`**: upstream
  lowmem `alloca`s the transient `lengthlist` (`entries` is a 24-bit field,
  so up to 16 MB), the maptype-1 `q_val` scratch (`quantvals` can reach
  `2^23` with `dim==1` from a tiny crafted ordered-codebook header), and the
  `work` array in `_make_decode_table` (up to ~64 KB). Each is a stack-clash
  well past any guard page and far beyond an ESP32 task stack. All three are
  now heap-allocated with NULL checks and freed on every exit path
  (`lengthlist` explicitly at the error labels; the `q_val` scratch either
  explicitly after table construction or via `vorbis_book_clear` on error).
- **Allocation failures fail cleanly**: the previously unchecked `dec_table`
  allocations (both the `nodeb==4` and packed paths), both long-lived
  `q_val` allocations, and `ci->book_param` in `info.c` are NULL-checked and
  fail through `vorbis_book_clear` / the header-error path instead of
  dereferencing NULL. The setup-struct allocations on the same header-parse
  path are checked the same way: the `vorbis_info_floor0`/`floor1` structs in
  `floor0_unpack`/`floor1_unpack`, the `vorbis_info_residue0` struct in
  `res0_unpack`, the `vorbis_info_mapping0` struct in `mapping0_unpack`, and
  each `vorbis_info_mode` in `info.c`'s mode loop now test the
  `_ogg_malloc`/`_ogg_calloc` result and bail out through the existing
  NULL-tolerant error paths instead of immediately dereferencing it, so the
  unpacker returns NULL and `vorbis_synthesis_headerin` returns
  `OV_EBADHEADER`. Relevant for embedded where a malicious setup header can
  legally demand multi-megabyte tables that fail to allocate, or where the
  decoder is set up under memory pressure.
- **`dim < 1` rejected at unpack**: no real encoder emits it, and it would
  otherwise reach `n/dim` divisions on the decode path (`res012.c`
  range-checks the groupbook's dim but not the stage books').
- **`_book_maptype1_quantvals` overflow/hang fix**: the polishing loop's
  `acc *= vals` / `acc1 *= vals+1` products are saturated against
  `b->entries` instead of being allowed to overflow `long` (signed-overflow
  UB that could also make the loop never terminate); degenerate
  `dim < 1`/`entries < 1` inputs return early and the decrement branch
  guards `vals <= 1`. The function was also made `static`.
- **`_make_words` bounds-checked**: gained an `rn` parameter (the allocated
  size of the work buffer) and rejects malformed lengthlists whose tree
  chase would write past it.
- **Ordered-case validation in `vorbis_book_unpack`**: rejects `length == 0`,
  codeword counts that cannot fit in `length` bits
  (`num-1 >= 2^length`), and `num` exceeding the remaining entries; the
  unordered case adds an up-front plausibility check that the claimed entry
  count fits in the bytes remaining in the packet.
- **Shift-count guards**: dequantization shift counts derived from
  bitstream-supplied `q_minp`/`q_delp` are rejected once per vector batch in
  `decode_map_ctx_init` when `|shift| >= 32` (see the UB section), and the
  maptype-2/`dec_type 2` path rejects `q_pack * dim > 32`, which would
  otherwise produce out-of-range shifts in `decpack`.
- **Decode-path vector scratch off the stack**: the per-vector `v` scratch
  in `vorbis_book_decodevs_add`, `vorbis_book_decodev_add`,
  `vorbis_book_decodev_set`, and `vorbis_book_decodevv_add` was an
  `alloca(4*dim)`. `dim` is bounded only by
  `_ilog(dim)+_ilog(entries) <= 24`, so a crafted codebook (e.g. with
  `entries < 256`) can claim `dim` up to 65535 resulting in a 256 KB `alloca` per
  call, fatal on typical 4 to 8 KB embedded task stacks. Replaced with a fixed
  32-value on-stack buffer (`DECODE_VEC_STACK`; real encoders emit
  `dim <= 8`) and a checked heap fallback for larger dims, freed on all
  paths (errors break out of the loop instead of returning mid-function).
- **Floor 0 book validation** (`floor0.c`): `floor0_unpack` also
  rejects referenced codebooks with no value mapping (`dec_type == 0`,
  i.e. maptype-0 books, unusable by `vorbis_book_decodev_set`) or
  `dim < 1`, and rejects `numbooks < 1`.
- **Tree-walk fall-off forces end-of-packet** (`codebook.c`): when
  `decode_packed_entry_number` chases a codebook's decode tree through all
  `dec_maxlength` bits without reaching a leaf, it now sets the overrun
  sentinel (`oggpack_seteop`) rather than relying on the trailing
  `oggpack_adv(b, read+1)` to trip it. That advance's overflow check is
  byte-granular, so slack in the final partial byte could leave end-of-packet
  unset and let the `0xffffffff` entry reach `decode_map_apply`, which would
  then dequantize garbage (`dec_type 1`) or read `q_val` out of bounds
  (`dec_type 2`, when `quantvals` is not a power of two). Only a one-used-entry
  book reaches this path: fuller trees are complete or rejected by the
  underpopulated-tree check in `_make_words`, and a valid stream only emits
  that book's single codeword, so it never falls off. The `dec_type 3` index
  bound is kept as a secondary guard.
- **Zero-entry codebooks parse on ESP-IDF** (`codebook.c`): `entries` is a
  legal 24-bit field, and `_make_decode_table` is deliberately written to
  accommodate 0- and 1-sized books (the `nodeb==4` special case). But the
  `lengthlist` allocations (unordered and ordered cases) and the maptype-2
  `dec_type 3` `q_val` allocation size themselves from `entries`/`used_entries`,
  so a zero-entry book requests 0 bytes. `heap_caps_malloc(0)` returns `NULL`
  on ESP-IDF (glibc returns a unique non-`NULL` pointer), which the `if(!p)`
  checks would misread as OOM and reject the book, causing a stream to decode
  on-host but not on-target. The three sizes are now clamped to at least one
  byte; the read/pack loops that follow are empty when the count is zero, so
  the extra byte is never touched.

### Performance changes relative to the lowmem branch

- **Hoisted dequantization invariants** (`codebook.c`): upstream lowmem's
  `decode_map` recomputes the add/shift/mask dequantization constants for
  every decoded vector. The fork splits it into `decode_map_ctx_init`
  (computes a `decode_map_ctx` of loop invariants once per
  `vorbis_book_decodev*` call, and is where the shift-range validation
  lives) and `decode_map_apply` (the per-vector hot path). The
  `q_bits <= 8` byte-vs-short branch in the `dec_type 2` path is hoisted
  out of the per-dimension loop with pre-typed pointers, and the
  `dec_type 3` path uses typed pointer arithmetic instead of upstream's
  non-standard `void *` arithmetic.
- **Codebook-specific memory placement**: the long-lived codebook
  structures (`book_param`, `dec_table`, `q_val`) are allocated through
  `_ogg_codebook_malloc`/`_ogg_codebook_calloc` instead of plain
  `_ogg_malloc`/`_ogg_calloc`. On ESP-IDF these map to a separate
  Kconfig-controlled placement policy (`custom_allocator.h`) that defaults
  to **prefer internal RAM**, since decode tables are read with
  random-access patterns in the hot path. Host builds fall back to
  `_ogg_malloc`/`_ogg_calloc` via `os.h`.

## Synthesis / MDCT Subsystem (from the lowmem branch)

Upstream master's backward MDCT produces a full n-sample block directly,
and the surrounding synthesis code keeps two full-block PCM copies:
`vb->pcm` (block arena, current block) and `v->pcm[i]` (DSP arena,
previous block's tail, kept across packets for overlap-add). The fork
replaces both the transform and the buffer scheme with the design from
upstream's **lowmem** branch, then fixes two bugs found while porting.

- **`mdct_backward` transforms n/2 values in place** (`mdct.c`): the lowmem
  pipeline (`presymmetry`, `mdct_butterflies`, a permutation-only
  `mdct_bitreverse`, `mdct_step7`, `mdct_step8`) replaces the master-shaped
  full-block transform, which folded a cross-product deinterleave into its
  bitreverse step. The lowmem transform defers that deinterleave, along
  with windowing and overlap-add, to `mdct_unroll_lap()`, which now runs
  once per readout chunk instead of once per decoded block.
- **`mdct_shift_right`** (`mdct.c`): saves the n/4 odd-indexed values of a
  just-transformed block into a tail buffer, for `mdct_unroll_lap()` to
  overlap-add against on the next block.
- **`mdct_unroll_lap`** (`mdct.c`): reconstructs, windows, and overlap-adds
  the current block's half-transform against the previous block's saved
  tail, writing `[start,end)` samples of the frame at the given stride.
  Called from `vorbis_synthesis_lapout()` once per readout chunk rather
  than once per block, so a partial or retried read replays the same
  reconstruction instead of re-reading a buffer.
- **Two behavioral fixes relative to upstream lowmem** (`mdct.c`):
  - `mdct_unroll_lap`'s cross-lap for the negated-mirror region computes
    `+ MULT31(-*l,*wL++)` instead of upstream's `- MULT31(*l,*wL++)`.
    `MULT31` truncates, so `-MULT31(l,w) != MULT31(-l,w)`; negating the
    sample before the multiply (instead of negating the product after)
    matches the old master-shaped pipeline bit-exactly. Upstream lowmem's
    version is off by a uniform 2 s7.24 units in these regions.
  - `mdct_step8` case 0's last cross-product now writes `x+6,x+7`; upstream
    lowmem writes `x+5,x+6`, colliding with the pair written just above it
    and corrupting every 4th sample of blocksize-8192 streams. This looks
    like an upstream bug worth reporting to the lowmem branch - it would
    affect any other decoder built from that branch's MDCT at n=8192 (a
    blocksize the Vorbis spec allows, though mainstream encoders rarely
    emit it - which is how the bug survived upstream).
- **Raw fixed-point readout** (`mdct_unroll_lap` in `mdct.c`): emits raw
  s7.24 `int32` samples rather than upstream's clipped 16-bit output.
  Rounding and clipping now live in the wrapper (`clip_to_16()` in
  `ogg_vorbis_decoder.cpp`), which needs the extra precision below the
  16-bit output to round-to-nearest instead of floor-shifting.
- **`work`/`mdctright` buffer scheme** (`ivorbiscodec.h`, `block.c`,
  `mapping0.c`, `synthesis.c`): replaces the two-stage PCM double buffer
  (`vb->pcm` in the block arena, `v->pcm[i]` history in the DSP arena) with
  the lowmem scheme. `vorbis_dsp_state::work[i]` is a `blocksizes[1]/2`
  `int32` plane that `mapping0_inverse` writes the floor-applied spectrum
  into and `mdct_backward` transforms in place; it is allocated for every
  channel, because residue decode and channel coupling touch every channel
  regardless of `channel_keep`. `vorbis_dsp_state::mdctright[i]` is a
  `blocksizes[1]/4` `int32` overlap tail that `mdct_shift_right` saves at
  the start of the next packet's decode and `mdct_unroll_lap` consumes at
  readout; it is allocated only for kept channels (`NULL` for dropped
  ones). Persistent PCM state per kept channel is therefore
  `3/4 * blocksizes[1]` `int32`s instead of `2 * blocksizes[1]`: at
  blocksize 2048 that is 6 KB versus 16 KB per channel, measured across
  both arenas as roughly 10 KB saved for mono, 20 KB for stereo, and 80 KB
  for 7.1.
- **Readout API** (`ivorbiscodec.h`, `block.c`): `vorbis_synthesis_pcmout`
  (plane passback into arena-owned buffers) is replaced by
  `vorbis_synthesis_pcmavail()` (pending sample count) and
  `vorbis_synthesis_lapout()` (per-channel, non-consuming; reconstructs
  `mdct_unroll_lap()`'s output into a caller-supplied buffer on demand).
  `vorbis_synthesis_read()` still advances the readout window and is the
  only consuming call. The wrapper strip-mines readout in fixed
  64-sample chunks (`LAPOUT_CHUNK` in `ogg_vorbis_decoder.cpp`) instead of
  grabbing a whole-packet plane pointer.
- **Shift-right ordering** (`synthesis.c`): `_vorbis_synthesis1` calls
  `mdct_shift_right` for each kept channel only after every header-parse
  `OV_EBADPACKET` check has passed, so a malformed packet cannot advance
  lap state before being rejected. It runs before the mapping inverse
  overwrites `vd->work[]` with the new block's spectrum, using `vd->W`
  (still the previous block's flag at that point in the packet) to size
  the shift.
- **`vorbis_synthesis_blockin` becomes bookkeeping-only** (`block.c`): no
  PCM is read or written there; the half-transform already sits in
  `vd->work[]` and the previous tail already sits in `vd->mdctright[]` by
  the time `blockin` runs. It advances `lW`/`W`/`granulepos`/`sample_count`
  and opens the `out_begin`/`out_end` readout window that
  `vorbis_synthesis_lapout` reconstructs from. The fork's hardened granule
  trimming (the `ogg_int64_t`-overflow guard on a negative/crafted
  granulepos; see Undefined-Behavior Cleanups) is preserved, now operating
  on `out_begin`/`out_end` instead of the old `pcm_current`/`pcm_returned`
  indices.
- **`_vorbis_apply_window` removed** (`window.c`, `window.h`): the function
  windowed and overlap-added a full block in place; it has no callers once
  `mdct_unroll_lap` performs windowing and overlap-add at readout instead.
  Removed under the Dead-Code Policy (`window.c` already carries the
  fork's modification header). `_vorbis_window` (the window-table lookup
  `_vds_init` calls) stays.

Equivalence with the pre-port pipeline was checked with a development
harness comparing old vs. new decode over 14 blocksize configurations
(64..8192, including mixed short/long transitions), 4000 random frames
each (~61M samples), all four window transitions, and chunked partial
reads: zero mismatches. All 10 checked-in test fixtures decode
byte-identical to the pre-port decoder.

## Memory Management

- **PSRAM-aware allocator** (`custom_allocator.h`, new file): `_ogg_malloc`,
  `_ogg_calloc`, `_ogg_realloc`, and `_ogg_free` are overridden on ESP-IDF
  builds to use the corresponding `heap_caps_*` APIs
  (`heap_caps_malloc_prefer`, `heap_caps_calloc_prefer`, and
  `heap_caps_realloc_prefer` for the "prefer" modes;
  `heap_caps_malloc` / `heap_caps_calloc` / `heap_caps_realloc` for the
  strict modes), with Kconfig-driven policy (prefer PSRAM, prefer
  internal, PSRAM-only, or internal-only). A second pair,
  `_ogg_codebook_malloc` / `_ogg_codebook_calloc`, carries an independent
  placement policy for codebook decode tables (default: prefer internal
  RAM: see the codebook section). Non-ESP-IDF builds fall through to
  plain `malloc` / `free`. The header is included from `os.h` so overrides
  apply project-wide.
- **Arena allocator in `vorbis_block`** (`block.{c,h}`, `ivorbiscodec.h`):
  upstream performs many small heap allocations per frame via a chained
  allocator on the decode path. The fork replaces that with a single
  pre-sized arena (`arena_data`, `arena_capacity`, `arena_used`) that is
  reset each frame by `_vorbis_block_ripcord()`. Arena size is computed
  once from `codec_setup_info` by `_vorbis_arena_compute_size()` in
  `block.c`. That function sums raw (unrounded) per-allocation sizes while
  `_vorbis_block_alloc` rounds each allocation up to `ARENA_ALIGN`, so it adds
  an alignment-slack term of `(3*channels + 4) * (ARENA_ALIGN - 1)`: one
  `ARENA_ALIGN-1` of rounding waste per allocation, with the per-packet
  allocation count bounded by `3*channels + 4` (4 mapping bundles - the
  `pcmbundle`/`zerobundle`/`nonzero`/`floormemo` pointer arrays in
  `mapping0.c` - plus one floor memo, one residue inner array, and one
  residue outer array per channel). No PCM sample storage lives in this
  arena: since the lowmem synthesis port, the per-channel half-transform
  and overlap-tail buffers live in the DSP setup arena instead (`work`/
  `mdctright`; see Synthesis / MDCT Subsystem), so the block arena no
  longer carries a pcm pointer array or per-channel pcm buffer. The slack
  scales with the channel count because the allocation count does; a flat
  estimate could be overflowed by a high-channel stream
  (Vorbis permits up to 255 channels), and `_vorbis_block_alloc`'s NULL return
  is dereferenced unchecked on the decode path (`synthesis.c`, `mapping0.c`,
  `res012.c`), so an undersized arena would crash rather than fail cleanly.
  `vorbis_block_init` now returns `-1` on arena allocation
  failure (upstream always returned `0`), so callers must check the
  return value. `_vorbis_block_alloc` and `_vorbis_block_ripcord` are
  `static inline` in `block.h` (upstream defines them as extern functions
  in `block.c`) so the decode hot path can inline them.
- **`ARENA_STACK` macro** (defined in `block.h`) replaces decode-path
  `alloca`-style allocations in:
  - `mapping0.c`: `pcmbundle`, `zerobundle`, `nonzero`, `floormemo`.
  - `res012.c`: the `partword` outer pointer array in `_01inverse` (its
    per-channel rows and `res2_inverse`'s `partword` come from
    `_vorbis_block_alloc` directly; since the residue look-layer elimination
    below, each row is a flat `unsigned char` array of decoded partition-class
    indices, not an array of pointers into a setup-arena decodemap).

  The codebook decode helpers do not use the arena; their per-vector
  scratch is a fixed stack buffer with heap fallback (see the codebook
  section). `floor0.c`'s order-sized `ilsp` array in `vorbis_lsp_to_curve`
  remains an `alloca`.
- **DSP setup arena in `vorbis_dsp_state`** (`block.{c,h}`, `backends.h`,
  `ivorbiscodec.h`, `floor0.c`, `floor1.c`, `res012.c`, `mapping0.c`): the
  per-stream DSP state is the second source of small-allocation churn after the
  per-packet block arena. Upstream `vorbis_synthesis_init` heap-allocates the
  `private_state`, the `pcm`/`pcmret`/`channel_keep` pointer arrays, each
  channel's PCM history buffer, the `b->mode` table, and (originally the
  dominant count) every floor/residue/mapping lookup that `*_look` builds (a
  residue `decodemap` alone was `partitions^groupbook_dim` separate
  allocations, before the residue look-layer elimination below removed it
  entirely). On a typical stereo stream this was ~240 individual
  `_ogg_malloc`s. The fork replaces them with one pre-sized arena:
  - **Fields** (`vorbis_dsp_state`): `setup_arena_data`, `setup_arena_capacity`,
    `setup_arena_used`. Bump-allocated via `_vorbis_setup_alloc` /
    `_vorbis_setup_calloc` (`static inline` in `block.h`, same `ARENA_ALIGN`
    rounding as the block arena through the shared `_vorbis_arena_round`).
  - **Sizing** is computed once by `_vorbis_dsp_arena_compute_size()` in
    `block.c` before any allocation, mirroring every site in `_vds_init`. The
    per-backend lookups are sized through a new `arena_size(...)` callback added
    to each backend vtable (`vorbis_func_floor` / `_residue` / `_mapping` in
    `backends.h`); each `*_arena_size` lives in the same file as its `*_look`
    (the `vorbis_look_*` structs are file-local) and mirrors that function's
    allocations 1:1. `mapping0_arena_size`
    recurses into the floor/residue `arena_size` callbacks per submap; `res0_arena_size`
    (residue backends 0/1/2) is a constant `0` since the residue look-layer
    elimination below - `res0_look` no longer allocates. The mirror
    is exact: the computed size equals the arena's final used watermark. A
    256-byte `DSP_ARENA_SAFETY` pad is added as insurance against an
    overlooked site or platform `sizeof` drift. `_vorbis_dsp_arena_compute_size`
    accumulates and returns the total in `ogg_int64_t`: a crafted header can
    point up to 64 modes * 16 submaps at floor/mapping lookups whose combined
    size is large, and although each per-mode term is a valid `long`, the
    sum can exceed `2^31` and would wrap a 32-bit `long` on the ESP32 target to a
    small value. `_ogg_malloc` would then succeed undersized and a backend
    `*_look`'s unchecked arena allocations would scribble past the buffer.
    `_vds_init` rejects any header whose 64-bit total exceeds
    `DSP_ARENA_MAX_BYTES` (`2^31-1`, the largest arena a `long` offset can
    address) via the existing `-1` init-failure path before the allocation.
  - **Per-look freeing is gone**: the backend `free_look` hooks are now no-ops
    and `vorbis_dsp_clear` releases the entire DSP state in a single
    `_ogg_free(setup_arena_data)`. `_vds_init` returns `-1` if the one arena
    allocation fails.
  - **`work`/`mdctright` buffers fold into the arena.** These replaced the
    old per-channel `v->pcm[i]` PCM history buffers (`blocksizes[1]` int32s
    each, once the bulk of decoder RAM) when the synthesis subsystem moved
    to the lowmem buffer scheme (see Synthesis / MDCT Subsystem): `work[i]`
    (`blocksizes[1]/2` int32s, every channel) and `mdctright[i]`
    (`blocksizes[1]/4` int32s, kept channels only) are carved from this same
    arena. The channel-keep mask is fixed *before* the arena is sized via
    `vorbis_synthesis_init_ex(v, vi, keep, n)` (`keep == NULL` keeps all
    channels, the behavior-preserving default), so a dropped channel's
    `mdctright` tail is never allocated rather than allocated-then-freed
    (its `work` plane is still allocated - residue decode and coupling
    write into every channel's plane; see the per-channel decode-mask entry
    below). The mask is therefore immutable for the stream (fixed once at
    init). The arena is the entire DSP heap footprint regardless: one
    allocation for the whole decoder state, not one per structure.
    (Codebooks are separate: they belong to `vorbis_info` and are
    heap-allocated through the codebook allocators.) See the per-channel
    decode-mask entry below for the CPU/memory details of channel
    selection itself.
- **Residue look-layer elimination** (`backends.h`, `res012.c`): lowmem
  Tremor has no `vorbis_look_residue0` at all - every decode-time precompute
  lives in the parsed setup struct, built once at unpack. The fork now
  matches: `vorbis_info_residue0` gains `stagemasks`/`stagebooks`/`stages`
  (heap-allocated in `res0_unpack` to `partitions`/`partitions*8` from the
  same `secondstages`/`booklist` data `res0_look` used to consume, freed by
  `res0_free_info`), replacing `vorbis_look_residue0`'s `partbooks`
  (`codebook ***`) and `decodemap` (`int **`, `partitions^groupbook_dim`
  rows of `groupbook_dim` ints - the single largest DSP-arena term before
  this change). `res0_look` is now an identity function returning the info
  pointer; `res0_arena_size` returns `0`; `res0_free_look` stays a no-op
  (nothing to free). `_01inverse`/`res2_inverse` decode each partition
  codeword's class indices arithmetically instead of indexing `decodemap`
  (lowmem's approach, ported into this fork's `_01inverse`/`res2_inverse`
  split rather than lowmem's merged single-function `res_inverse`): the
  phrasebook-decoded `temp` is split into `partitions_per_word` digits,
  most-significant first, by successive division against descending powers
  of `info->partitions` - the same digit order `decodemap[temp][k]` produced,
  so the change is bit-exact. `partword` changes from a block-arena array of
  pointers into the (now-gone) setup-arena `decodemap` to a flat per-channel
  `unsigned char` array of decoded class indices (`ARENA_STACK` outer array +
  `_vorbis_block_alloc`'d rows, as before - see the `ARENA_STACK` entry
  above); `_vorbis_arena_compute_size`'s residue terms in `block.c` are
  resized accordingly (`partwords * dim` bytes per row instead of `partwords`
  pointers). The `temp>=info->partvals` bounds check (rejecting a phrasebook
  entry beyond the valid partition-word range, same as before) is kept: it is
  what guarantees every decoded digit lands in `[0, info->partitions)`, which
  `stagemasks`/`stagebooks` are sized for - lowmem's own `res_inverse`
  omits this check, but dropping it here would be an out-of-bounds read.
- **Floor1 look-layer elimination** (`backends.h`, `floor1.c`): matching the
  lowmem branch's `floor1_info_unpack`, which builds `forward_index`/
  `hineighbor`/`loneighbor`/`posts` directly into `vorbis_info_floor1` at
  unpack time rather than a separate look struct. The fork's version moves
  (not reimplements) the exact precompute the old `floor1_look` ran: the
  sorted-postlist pass that fills `forward_index` (sort order -> range
  number) now runs inside the existing postlist duplicate-value check in
  `floor1_unpack` and reuses that check's `sortpointer` array instead of
  re-sorting, and the lo/hi neighbor scan is unchanged arithmetic, just
  addressed off `info->postlist` instead of `look->`. `forward_index`/
  `hineighbor`/`loneighbor` are heap-allocated in `floor1_unpack` to the
  parsed post count (`posts`/`posts-2`, capped by the existing `VIF_POSIT`
  check) as `unsigned char` - upstream lowmem's own typing, since every
  index value is `< posts <= 65` - narrower than the old look struct's
  spec-max `int[VIF_POSIT+2]`/`int[VIF_POSIT]` embedded arrays, freed by
  `floor1_free_info`. The old look struct's `n` (`postlist[1]`, only used
  transiently while computing the neighbor scan's initial search bound)
  and `quant_q` (a pure function of `info->mult`) are not carried forward -
  `n` is read directly off `info->postlist[1]` where it's needed, and
  `quant_q` becomes a decode-time lookup into a small static table
  (`floor1_quant_q[4]`, replacing the old `switch`) in `floor1_inverse1`,
  the only place it was used. `floor1_look` is now an identity function
  returning the info pointer; `floor1_arena_size` returns `0`;
  `floor1_free_look` stays a no-op (nothing to free).
- **Right-sized setup structs** (`backends.h`, `mapping0.c`, `res012.c`):
  upstream's parsed setup structs embed spec-maximum fixed arrays regardless of
  the stream. `vorbis_info_mapping0` reserved `chmuxlist[256]` and
  `coupling_mag`/`coupling_ang[256]` (3072 bytes), and `vorbis_info_residue0`
  reserved `secondstages[64]` and `booklist[512]` (2304 bytes); a typical
  stereo stream (2 mappings + 2 residues) touches a few dozen bytes of that
  ~10.5 KB. These five arrays are now pointers, heap-allocated by
  `mapping0_unpack` / `res0_unpack` to the parsed counts that govern every
  access (`vi->channels`, `coupling_steps`, `partitions`, and the
  cascade-bit sum `acc`), and freed by `mapping0_free_info` /
  `res0_free_info` (NULL-safe on partially-initialized structs since the
  parent struct is calloc'd). Because the allocation sizes now derive from
  attacker-controlled header fields, each count is validated against an
  explicit compile-time cap equal to the old fixed size (`VIM_CHANNELS`,
  `VIM_COUPLES`, `VIR_PARTS`, `VIR_BOOKS` in `backends.h`) before
  allocating - defense in depth beyond the read bit-widths that already
  bound them. Two behavioral subtleties: `chmuxlist` is always allocated
  and zeroed (calloc) because `mapping0_inverse` indexes it for every
  channel even when `submaps==1` leaves it unfilled, while
  `coupling_mag`/`ang` stay NULL when there is no coupling (every read is
  inside a `coupling_steps`-bounded loop); and `booklist` uses the
  `acc?acc:1` idiom so a zero-book residue never hits `malloc(0)`.
- **Right-sized `codec_setup_info` tables** (`codec_internal.h`, `info.c`,
  `synthesis.c`): the eight spec-max `[64]` tables in `codec_setup_info`
  (`mode_param`, `map_type`, `map_param`, `time_type`, `floor_type`,
  `floor_param`, `residue_type`, `residue_param`; 2048 bytes on a 32-bit
  target, of which a typical stream uses a few dozen) are now heap pointers
  allocated by `_vorbis_unpack_books` to the parsed counts
  (`modes`/`maps`/`times`/`floors`/`residues`, each a 6-bit read +1). Each
  count is read into a local, validated against `VI_SETUP_MAX` (64, the old
  fixed size - defense in depth beyond the bit width), and published to `ci`
  only after the section's table(s) are calloc'd, so `vorbis_info_clear`'s
  count-bounded free loops can never index a NULL table; for two-table
  sections (floor/residue/map) both tables exist before the count is
  nonzero because clear reads both. Clear also frees the tables themselves
  (NULL-guarded defensively) after the per-entry params. The safety-critical
  consequence of right-sizing: an audio packet's `mode` field is read with
  `ilog(modes)` bits, so a non-power-of-two mode count lets a crafted packet
  encode `mode >= modes`. Against the old `[64]` array that landed on a NULL
  entry and was rejected by the existing check; against a right-sized table
  it would be a heap OOB read. `_vorbis_synthesis1` and
  `vorbis_packet_blocksize` therefore bound `mode` against `ci->modes`
  (returning `OV_EBADPACKET`) before any `mode_param[mode]` access - the
  fork's only divergence in `synthesis.c`. Every other index into these
  tables is a header-time value already validated at unpack
  (`mode_param[i]->mapping < maps` in `info.c`, `floorsubmap < floors` and
  `residuesubmap < residues` in `mapping0.c`).
- **Right-sized `vorbis_info_floor1`** (`backends.h`, `floor1.c`): the floor1
  setup struct embedded six spec-max fixed arrays (`partitionclass[31]`,
  `class_dim`/`class_subs`/`class_book[16]`, `class_subbook[16][8]`,
  `postlist[65]`; 1064 bytes, of which a typical stereo stream's floors use
  well under 100). They are now pointers heap-allocated by `floor1_unpack` to
  the counts that govern every access - `partitions` (5-bit read, so <=
  `VIF_PARTS`), `maxclass+1` (4-bit class ids, so <= `VIF_CLASS`), and the
  post count (rejected above `VIF_POSIT` by the pre-existing check) - and
  freed by `floor1_free_info` (NULL-safe, parent struct is calloc'd). The
  counts are discovered progressively, so allocation is staged:
  `partitionclass` after the `partitions` read, the four class arrays once
  the partition-class loop has fixed `maxclass`, and `postlist` after a new
  pre-pass that totals `class_dim[partitionclass[j]]` (the fill loop then
  reads the same values in the same order as upstream's interleaved
  count/fill loop). When `partitions==0` the partition/class arrays stay
  NULL - no loop or decode path indexes them (`maxclass` stays -1) - while
  `postlist` is always allocated (`count+2 >= 2`; entries 0 and 1 are
  unconditional). `class_book` and `class_subbook` are calloc'd because
  upstream reads `class_book[j]` even when `class_subs[j]==0` leaves it
  unwritten, and `class_subbook` rows are only filled up to
  `1<<class_subs[j]` entries. `class_subbook` stays rectangular as a flat
  `[maxclass+1][8]` array with a fixed stride of 8 (`classv*8+k` indexing in
  unpack and `floor1_inverse1`); ragged per-row sizing was not worth the
  offset bookkeeping since dropping unused class rows is the bulk of the
  saving. Unlike the mode case above there is no packet-controlled index
  into any of these arrays: every decode-time index (`partitionclass[i]`
  with `i<partitions`, class arrays at `classv<=maxclass`, subbook at
  `cval&(csub-1) < 1<<class_subs`, `postlist` within `posts=count+2`)
  derives from the same parsed counts that size the allocations. floor0's
  info struct (~104 bytes) is left as-is.
- **Comment handling removed from the decoder** (`info.c`, `ivorbiscodec.h`):
  the decoder discards Vorbis comments entirely (the OggVorbisDecoder wrapper
  streams the real comment packet past without buffering and feeds a synthetic
  empty one), so the entire `vorbis_comment` machinery was removed:
  - The `vorbis_comment` struct and its `vorbis_comment_init` / `vorbis_comment_clear`
    functions are gone, as is the dead tag-lookup API (`vorbis_comment_query`,
    `vorbis_comment_query_count`, and their `tagcompare` / `_v_toupper` helpers).
  - `_vorbis_unpack_comment` no longer takes a `vorbis_comment *` and allocates
    nothing. It validates and skips the packet via `oggpack_adv` (upstream
    allocated `vendor`, `user_comments`, and `comment_lengths`, including a
    1-byte vendor and two `count+1` arrays even for an empty comment header).
  - `vorbis_synthesis_headerin` drops its `vorbis_comment *vc` parameter; the
    only state it needs, "comment header seen", is now a single
    `int comment_header_seen` field in `vorbis_info`, which already persists
    across the three header packets and gates the id -> comment -> setup ordering.
  This removes one heap-managed struct, two API functions, and three one-time
  heap allocations per stream. Comment retrieval is intentionally
  unsupported in this fork.
- **Per-channel decode mask** (`ivorbiscodec.h`, `block.c`, `mapping0.c`,
  `synthesis.c`): a `channel_keep` bitmask on `vorbis_dsp_state` (packed
  `VORBIS_KEEP_BYTES` bytes, one bit per channel; the `vorbis_keep_*`
  inlines in `ivorbiscodec.h` read/write it), set via
  `vorbis_synthesis_init_ex()`, lets the caller (the OggVorbisDecoder
  wrapper, when channel selection is active) decode only the channels it
  will output. The same bitmask is the caller-facing `keep` argument, so
  selection costs the wrapper a 32-byte stack mask rather than a 1 KB int
  array. Allocated all-set in `_vds_init` and freed in `vorbis_dsp_clear`,
  so default behavior is unchanged. The entropy decode and inverse channel
  coupling always run for every channel (residue bits are
  interleaved/variable-length and coupling mixes magnitude/angle partners,
  so neither can be skipped per channel); the mask only gates the
  per-channel-independent tail:
  - CPU: `mapping0_inverse` skips floor-apply (`inverse2`) and the
    half-block iMDCT (`mdct_backward`, the dominant cost) for unkept
    channels; `_vorbis_synthesis1` (`synthesis.c`) skips their
    `mdct_shift_right` tail save; and `vorbis_synthesis_lapout` rejects a
    non-kept channel with `OV_EINVAL` before running `mdct_unroll_lap`'s
    window/overlap-add, so dropped channels never reach readout
    reconstruction either. Kept channels traverse identical code, so their
    output is bit-exact vs a full decode.
  - Memory: `work[i]` (the `blocksizes[1]/2` half-transform plane) is
    allocated for every channel regardless of `keep`, since residue decode
    and coupling write into it before the mask is ever checked. Only
    `mdctright[i]` (the `blocksizes[1]/4` overlap tail) is skipped for
    dropped channels - `NULL`, never allocated rather than
    allocated-then-freed. The keep mask is fixed at init via
    `vorbis_synthesis_init_ex()` (`keep == NULL` keeps all channels) and is
    immutable for the stream, since it gates this arena-owned allocation.
    `vorbis_synthesis_lapout` returns `OV_EINVAL` for an unkept channel,
    replacing the old `vorbis_synthesis_pcmout`'s NULL-plane report. Saving
    is `(channels - kept) * (blocksizes[1]/4) * 4` bytes - smaller than
    before the lowmem synthesis port, since `work[]` is no longer
    skippable; e.g. 10,240 B for 5.1 (6ch) -> 1ch at blocksize 2048
    (`5 * (2048/4) * 4`).

## Xtensa Optimizations

Built on Xtensa targets (ESP32 / ESP32-S2 / ESP32-S3). Guarded by the
compiler-defined `#ifdef __XTENSA__`.

- **Funnel-shift fast path in `bitwise.c`**: `oggpack_read` and
  `oggpack_look` use a branchless Xtensa funnel shift (two aligned
  32-bit loads plus the `SRC` instruction) on the fast path
  (`endbyte < storage - 7`), replacing the upstream cascading if-chain.
  The fast path reads two aligned 32-bit words, up to `(ptr & ~3) + 7`
  (`ptr + 7` when `ptr` is 4-byte aligned), so the guard is `storage - 7`
  rather than the scalar path's `storage - 4`. This keeps the top byte
  within `storage - 1`; the scalar path is self-bounded and handles the
  last bytes. A `storage - 4` guard over-reads up to 3 bytes past the
  buffer (masked off, but a latent OOB read that can fault when the packet
  ends at a mapped-region boundary).

## Portability and Housekeeping

- **`os.h`**: includes `custom_allocator.h` so the PSRAM-aware allocator
  overrides apply project-wide; defines host-build fallbacks for
  `_ogg_codebook_malloc` / `_ogg_codebook_calloc` (plain `_ogg_malloc` /
  `_ogg_calloc`); and retains the `HAVE_CONFIG_H`-guarded
  `#include "config.h"` for autotools-style builds. The `STIN` compiler
  ladder (including its VBCC/Watcom branches) and the absence of `<math.h>`,
  `M_PI`, and `rint` were already present in the imported upstream os.h,
  not added by this fork. They differ from the gitlab `master` / `lowmem`
  os.h because that import appears to come from a Tremor snapshot whose
  os.h had been synced to libvorbis's.
- **`misc.h`**: the endianness
  struct union is consolidated behind a single `WORDS_BIGENDIAN` gate
  rather than separate `BIG_ENDIAN` / `LITTLE_ENDIAN` struct layouts;
  `LOOKUP_T`'s `const` qualifier is removed (propagates to
  `const LOOKUP_T *` in `mdct.c` and `window.c`); `<sys/types.h>` is
  gated on `HAVE_SYS_TYPES_H`. The `VFLOAT_MULT`, `VFLOAT_MULTI`, and
  `VFLOAT_ADD` helpers are removed; nothing on the fork's decode or setup
  path called them.
- **`CLIP_TO_15` removed**: both upstream variants (the branchless
  `misc.h` fallback and the `asm_arm.h` inline-asm version) are deleted.
  Nothing in the fork calls `CLIP_TO_15`; 16-bit saturation is done by the
  wrapper (`clip_to_16()` in `src/ogg_vorbis_decoder.cpp`).
- **`info.c`**: `<ctype.h>` is dropped (its only use was the comment-tag
  matching in the now-removed query API); bitrate fields are cast to
  `ogg_int32_t` to suppress signed / unsigned warnings; the setup-header
  unpack functions reject non-positive book/floor/residue/map/mode counts
  up front. The unused decode primitives `vorbis_info_blocksize` and
  `vorbis_synthesis_idheader` are removed: no live decode path calls them (the
  wrapper recognizes identification headers through its own
  `is_vorbis_identification`), and `info.c` is already a forked file, so the
  Dead-Code Policy prunes them rather than preserving an upstream diff.
- **`ivorbiscodec.h`**: arena fields added to `vorbis_block` (see Memory
  Management). Encoder-only declarations `vorbis_comment_add` and
  `vorbis_comment_add_tag` were removed; the fork is decode-only. The
  `vorbis_comment` struct and the `vorbis_comment_init`/`_clear`/`_query`/
  `_query_count` declarations were removed (see Memory Management); a
  `comment_header_seen` flag was added to `vorbis_info` and the
  `vorbis_synthesis_headerin` prototype dropped its `vorbis_comment *` argument.
- **`mapping0.c`**: beyond the decode-path `ARENA_STACK` usage, a
  redundant `memset(info, 0, sizeof(*info))` in `mapping0_unpack` is
  removed. The struct is already zeroed by the preceding `_ogg_calloc`,
  so the change is behavior-neutral. The upstream `static int seq`
  debug counter and the commented-out `_analysis_output` scaffolding in
  `mapping0_inverse` are deleted: `seq` was incremented every block but
  read only by the dead debug calls, and as a file-scope static shared
  across decoder instances its non-atomic increment was a data race
  under the concurrent-stream decoding the library supports. The value
  was never observed, so removing it changes no output.
- **`res012.c`**: the `#ifdef TRAIN_RES` block in `res0_look` is deleted.
  It assigned to `look->training_data`, a member that does not exist on
  this tree's `vorbis_look_residue0` struct. Upstream Tremor inherited the
  `#ifdef` from libvorbis's encoder codebook-training scaffolding but never
  carried the matching struct field, so the block could only ever fail to
  compile if `TRAIN_RES` were defined. Nothing defines it, and it has no
  meaning in a decode-only library.
- **`floor0.c`**: `vorbis_lsp_to_curve` is changed from extern to
  `static`; it is only called within the file.
- **`mdct.c`, `window.c`**: `LOOKUP_T *` declarations are changed to
  `const LOOKUP_T *` to restore const-correctness after the `const`
  qualifier is dropped from `LOOKUP_T` in `misc.h`. `window.c` also drops
  `<math.h>`. It originally unrolled the left and right window ramps by 4
  in `_vorbis_apply_window` to overlap the load/multiply/store latencies;
  that function (and the unrolling with it) was removed once the lowmem
  synthesis port moved windowing into `mdct_unroll_lap` at readout time
  (see Synthesis / MDCT Subsystem and Dead-Code Policy).
- **Warning hygiene (`floor0.c`, `floor1.c`)**: the non-conversion warnings
  that surface when the tremor sources are built with `-Wall -Wextra` (the
  build normally suppresses them with `-w`) are silenced without changing
  behavior. The unused parameters required by the floor function-pointer
  signatures are cast to `void` (`vorbis_lsp_to_curve`'s `ln`,
  `floor0_inverse2`'s `vb`, `floor1_look`'s `mi`). `mdct.c` had a similar
  fix (the master-shaped `mdct_backward`'s rotate+window block reused its
  dead function-scope `iX` instead of redeclaring it, removing a
  `-Wshadow`); that pipeline was deleted by the lowmem synthesis port (see
  Synthesis / MDCT Subsystem), and the lowmem-shaped `mdct_backward` that
  replaced it introduces no `-Wshadow`. The bulk fixed-point conversion
  warnings (`-Wshorten-64-to-32`, `-Wsign-conversion`) are inherent to
  Tremor's `int`/`long` arithmetic and are left as-is.

## Undefined-Behavior Cleanups

- **Left shift of negative values**: upstream Tremor's fixed-point math
  left-shifts signed values it knows may be negative, relying on the
  two's-complement bit pattern. This is well-defined on every target the
  decoder runs on but is undefined behavior per the C standard, which
  UndefinedBehaviorSanitizer flags as `invalid-shift-base`. Each such
  shift was rewritten to operate on the unsigned counterpart and cast the
  result back (e.g. `x << 1` becomes
  `(ogg_int32_t)((ogg_uint32_t)x << 1)`). On any two's-complement machine
  this is bit-for-bit identical to the original signed shift, so the
  change is behavior-preserving while removing the UB. Sites fixed:
  - `misc.h`: `MULT31` and `MULT31_SHIFT15` (high-word combine). The
    `VFLOAT_*` helpers, which had the same issue, were removed outright.
  - `codebook.c` (relative to the upstream lowmem code): the
    `q_min << -shift` and `(v[i] * q_del) << -shift` upscaling paths in
    `decode_map_ctx_init` / `decode_map_apply`, where the shifted values
    are signed; the maptype-2 read accumulation in `decpack`
    (`oggpack_read` returns a signed `long`); and the `next << 16` node
    combine in `decode_packed_entry_number`'s 16/32-bit path.
  - `floor1.c`: the `room` doubling in the post-list unwrap.
  - `floor0.c`: the cosine-table term in `vorbis_coslook2_i`.
  - `asm_arm.h`: `MULT31` and the `<<1` doublings in `XPROD31`/`XNPROD31`.
    This is the inline-ARM-assembly path, compiled only when `_ARM_ASSEM_`
    is defined (never on the supported Xtensa or host targets); the fix
    mirrors `misc.h` for completeness so the dead path is also UB-free.

  Other shift sites are untouched: shifts with unsigned bases
  (`bitwise.c`, the bit-reverse helpers, `floor0.c`'s `pi`/`qi`) are
  already well-defined. `signed-integer-overflow` in the MDCT/residue
  modular arithmetic remains deliberate upstream behavior and is not
  addressed here.

- **Out-of-range dequantization shift counts** (`codebook.c`): the
  dequantization constants applied during vector decode are derived from
  the bitstream-supplied `q_minp`/`q_delp` fields plus the caller's
  fixed-point `point`, so a malformed codebook can make either shift count
  reach or exceed 32, which is UB whether shifting left or right, regardless
  of the unsigned-cast fix above. `decode_map_ctx_init` rejects
  `|shift| >= 32` for both the `q_min` and `q_del` shifts once per
  `vorbis_book_decodev*` call (not per vector, so no measurable cost on
  valid streams), and the affected decode helper returns `-1`. All callers
  (`floor0_inverse1`, `_01inverse`, `_02inverse`) already treat `-1` as a
  packet-error bailout, so the malformed packet is dropped cleanly. The
  related `q_pack * dim > 32` unpack-time rejection is described in the
  codebook hardening section.

- **Negative shift exponent from a truncated id header** (`info.c`):
  `_vorbis_unpack_info` set `ci->blocksizes[i] = 1 << oggpack_read(opb,4)`
  directly. On a truncated identification packet `oggpack_read` returns its
  `-1` end-of-packet sentinel, making `1 << -1` a shift by a negative count
  (UB; UBSan `shift-exponent-negative`). The two reads are now taken into
  locals and rejected if negative before the shift, mirroring the
  `rangebits` guard in `floor1.c`; the existing EOP check only fires after
  the shift. Reachable only through the exported `vorbis_synthesis_headerin`
  API. The C++ wrapper's 30-byte identification-header minimum keeps every
  in-tree path away from it.

- **Out-of-bounds value-array index from a sparse codebook**
  (`codebook.c`): the lowmem `_make_words` carries only the
  overpopulated-tree check. Master's `sharedbook.c` also rejected an
  underpopulated tree, and that check was lost in the lowmem rewrite, so
  a Huffman codebook whose lengthlist does not fill the tree is accepted.
  Its interior child slots stay 0 and point back at the root, which lets
  `decode_packed_entry_number` loop on that cycle during decode, use up
  `dec_maxlength` bits without reaching a leaf, and return `0xffffffff`
  with the end-of-packet sentinel still unset; the `oggpack_eop` guard in
  `decode_map_apply` then does not fire. For `dec_type == 3` (maptype-2
  books with `q_bits*dim > 24`) that value is used as a scalar index into
  the packed `q_val` array and reads past the end (a high-address read on
  64-bit hosts, `q_val - q_pack` on 32-bit targets). Fixed in two places:
  `_make_words` re-adds the marker-based underpopulated-tree rejection
  (one used entry stays the exception), and `decode_map_apply`'s
  `dec_type == 3` arm rejects `entry >= used_entries` before indexing.
  Types 1 and 2 use `entry` as packed bits, not an index, so a stray
  value there only yields garbage samples and needs no bound. Neither
  check is on the per-vector hot path; valid streams decode
  bit-identically. (Both gaps are present verbatim in the upstream Tremor
  lowmem branch.)

- **Out-of-range floor 0 setup fields**: `floor0.c`'s `floor0_unpack`
  reads `ampbits` as a 6-bit field (range 0..63) and later uses it as a
  left-shift count in `(1<<info->ampbits)-1`. A malformed setup header
  with `ampbits >= 32` is UB. `floor0_unpack` now rejects values outside
  `[1, 31]` alongside the existing `order`/`rate`/`barkmap`/`numbooks`
  validators. Caught at setup, so there is no per-frame cost.

- **Out-of-range exponent in `vorbis_invsqlook_i`**: `floor0_inverse2`
  passes an accumulator-derived `qexp` to `vorbis_invsqlook_i(a, e)`,
  which finishes with `val >> ((e>>1)+21)`. Malformed floor coefficients
  can drive the resulting shift either negative or `>= 32` (both UB).
  The lookup now returns 0 when the computed shift falls outside
  `[0, 31]`: one unsigned-compare branch on the floor-curve hot path,
  taken only for malformed input.

- **UB on shifts in `floor0_inverse1` amplitude decode**: the two-line
  amplitude reconstruction `maxval = (1 << info->ampbits) - 1` and
  `amp = ((ampraw * info->ampdB) << 4) / maxval` has two left-shift UB
  sites once `ampbits == 31` is reachable (which it is, since the
  setup-time validator only rejects `>= 32`): `1 << 31` overflows
  signed int, and the `ampraw * info->ampdB` product can exceed
  `INT32_MAX` before the `<< 4`. Both are now done with unsigned
  arithmetic (`1U << info->ampbits` and `(unsigned)ampraw *
  (unsigned)info->ampdB`), which makes the shifts well-defined for any
  `ampbits` in `[1, 31]`. The Vorbis spec leaves `ampbits` as a 6-bit
  field without a semantic upper bound, but Tremor's int32 math
  implicitly bounds it at 31, and floor 0 is unused by real encoders
  (they all emit floor 1), so the unspecified-but-bounded output on
  malformed input is harmless, while valid streams are unaffected.

- **Granule-position difference overflow in the page-trim logic**
  (`block.c`): `vorbis_synthesis_blockin` decides how many samples to
  trim from a short or partial-last page by subtracting the page's
  `granulepos` from the decoder's running sample count. Both values are
  `ogg_int64_t`, and the page granpos comes straight from the bitstream.
  A crafted negative granpos (really a huge unsigned sample number, such
  as `INT64_MIN`) makes the `sample_count - granpos` subtraction overflow
  `ogg_int64_t`, which is signed-overflow UB. On ESP32 the result was
  then narrowed into a 32-bit `long extra` as well. Both trim sites now
  keep `extra` in `ogg_int64_t` and set it to `0` when the page granpos
  is negative, since such a granpos denotes a sample count far beyond
  what has been decoded and there is nothing to trim. The subtraction
  therefore only runs on non-negative operands. The existing clamp that
  stops a set-EOP frame with a backdated granpos from rewinding
  `out_begin` past `out_end` (renamed from `pcm_current`/`pcm_returned`
  when the lowmem synthesis port turned `blockin` into pure bookkeeping;
  see Synthesis / MDCT Subsystem) is unchanged. Well-formed streams
  decode identically.

## Touched Files

New files:

| File | Role |
| --- | --- |
| `custom_allocator.h` | PSRAM-aware allocator overrides + codebook placement policy |
| `CHANGES.md` | This document |
| `bitwise.c` | Folded in from libogg (decode-only subset) |
| `ogg/ogg.h` | Folded in from libogg (decode-only subset) |
| `ogg/os_types.h` | Folded in from libogg (simplified) |

Deleted files:

| File | Reason |
| --- | --- |
| `sharedbook.c` | Obsoleted by the lowmem single-step `vorbis_book_unpack`; the two-step `static_codebook` pipeline (and with it `vorbis_staticbook_unpack`, `vorbis_book_init_decode`, `_book_unquantize`, the first-table acceleration) no longer exists |

Modified files:

| File | Change |
| --- | --- |
| `ivorbiscodec.h` | Block + DSP-setup arena fields; encode-only decls removed; `channel_keep` field; `vorbis_synthesis_init_ex()` decl; unused `vorbis_synthesis_init` / `vorbis_synthesis_idheader` / `vorbis_info_blocksize` decls removed (see Dead-Code Policy) |
| `codec_internal.h` | `static_codebook *book_param[256]` + `codebook *fullbooks` replaced by a single heap-allocated `codebook *book_param` array (lowmem design); the eight spec-max `[64]` setup tables (`mode_param`, `map_type`/`map_param`, `time_type`, `floor_type`/`floor_param`, `residue_type`/`residue_param`) converted to right-sized heap pointers with a `VI_SETUP_MAX` cap |
| `backends.h` | `arena_size(...)` callback added to the floor/residue/mapping vtables for DSP-setup-arena sizing; `vorbis_info_mapping0`'s `chmuxlist`/`coupling_mag`/`coupling_ang`, `vorbis_info_residue0`'s `secondstages`/`booklist`, and `vorbis_info_floor1`'s `partitionclass`/`class_dim`/`class_subs`/`class_book`/`class_subbook`/`postlist` converted from spec-max fixed arrays to right-sized heap pointers (`class_subbook` flattened to stride-8 rows), with `VIM_CHANNELS`/`VIM_COUPLES`/`VIR_PARTS`/`VIR_BOOKS` caps and the existing `VIF_PARTS`/`VIF_CLASS`/`VIF_POSIT` bounds; `vorbis_info_residue0` gains `stagemasks`/`stagebooks`/`stages` (residue look-layer elimination, see Memory Management); `vorbis_info_floor1` gains `posts`/`forward_index`/`hineighbor`/`loneighbor` (floor1 look-layer elimination, see Memory Management) |
| `block.{h,c}` | Block arena alloc/ripcord/sizing + `ARENA_STACK` (no longer sized for any per-channel PCM buffer - see Synthesis / MDCT Subsystem); `_vorbis_arena_round` and the DSP setup arena (`_vorbis_setup_alloc`/`_calloc`, `_vorbis_dsp_arena_compute_size`, single-free `vorbis_dsp_clear`); `vorbis_synthesis_init_ex` fixes the channel-keep mask before sizing so a dropped channel's `mdctright[]` tail is never allocated (`work[]` is allocated for every channel); `vorbis_synthesis_pcmout` replaced by `vorbis_synthesis_pcmavail`/`_lapout` (`_lapout` returns `OV_EINVAL` for an unkept channel, replacing the old NULL-plane report); `vorbis_synthesis_blockin` is now bookkeeping-only (`out_begin`/`out_end` readout window replaces `pcm_current`/`pcm_returned`; the decode-mask gates that used to live here moved to `mapping0_inverse`, `synthesis.c`'s `mdct_shift_right` loop, and `vorbis_synthesis_lapout`); granpos-difference trim arithmetic hardened against `ogg_int64_t` overflow on a crafted negative granpos (see UB cleanups), now operating on `out_begin`/`out_end`; unused channel-blind `vorbis_synthesis_init` shim removed (`vorbis_synthesis_init_ex` is the sole synthesis-init entry point); `block.c` gains `#include "backends.h"`/`"block.h"`; `vorbis_synthesis_read`'s parameter renamed `bytes` to `samples` to match its `ivorbiscodec.h` declaration and its sample-count semantics; `_vorbis_arena_compute_size`'s residue `partword` terms recount for the flat `unsigned char` row layout (`partwords*dim` bytes/channel) instead of a pointer-array layout (residue look-layer elimination, see Memory Management) |
| `os.h` | Allocator and toolchain hooks; host fallbacks for the codebook allocators |
| `misc.h` | endianness and `LOOKUP_T` cleanup; unsigned-cast shift fixes (UB cleanup); removed unused `VFLOAT_*` helpers and `CLIP_TO_15` |
| `codebook.{h,c}` | Replaced with the lowmem-branch single-step design, then modified: `oggpack_eop` emulation; setup `alloca`s (`lengthlist`, `q_val` scratch, `_make_decode_table` `work`) moved to checked heap allocations; NULL checks on `dec_table`/`q_val`/`book_param`; `dim<1` reject; `_book_maptype1_quantvals` saturation; `_make_words` bounds (`rn`); ordered/unordered unpack validation; `decode_map` split into `decode_map_ctx_init`/`decode_map_apply` with shift-range rejection and hoisted invariants; decode `v` scratch = 32-entry stack buffer + heap fallback (was `alloca(4*dim)`); unsigned-cast shift fixes; codebook allocations routed through `_ogg_codebook_*` |
| `info.c` | Comment handling removed (no `vorbis_comment` struct/init/clear/query; `_vorbis_unpack_comment` skips the packet without allocating; `vorbis_synthesis_headerin` tracks `vi->comment_header_seen` and dropped its `vorbis_comment *` arg); `_vorbis_unpack_books` calls the lowmem `vorbis_book_unpack` into a heap-allocated flat `book_param` array (NULL-checked, via `_ogg_codebook_calloc`); non-positive count guards; per-mode `_ogg_calloc` in the mode loop NULL-checked; `_vorbis_unpack_books` allocates each section's setup table(s) to the validated parsed count before publishing that count, and `vorbis_info_clear` frees the tables after the per-entry params; unused `vorbis_info_blocksize` / `vorbis_synthesis_idheader` primitives removed (see Dead-Code Policy) |
| `mapping0.c` | `ARENA_STACK` on decode-path temporaries; `mapping0_inverse` writes into `vd->work[]` (the lowmem half-transform plane) instead of a block-arena PCM buffer, and its decode-mask gate covers floor-apply / half-block iMDCT only (windowing moved to readout-time `mdct_unroll_lap`, gated there instead - see Synthesis / MDCT Subsystem); `mapping0_unpack` NULL-checks its `vorbis_info_mapping0` allocation and right-sizes `chmuxlist`/`coupling_mag`/`coupling_ang` to the validated parsed counts (freed by `mapping0_free_info`); upstream `seq` debug counter, the dead `_analysis_output` scaffolding, and its now-unused `<stdio.h>` include removed; `mapping0_look` routed to the DSP setup arena + `mapping0_arena_size` (recurses into floor/residue sizing); `free_look` no-op |
| `res012.c` | `ARENA_STACK`/arena allocation for `partword`; `res0_unpack` NULL-checks its `vorbis_info_residue0` allocation, right-sizes `secondstages`/`booklist` to the validated parsed counts, and (residue look-layer elimination) builds `stagemasks`/`stagebooks`/`stages` from them (freed by `res0_free_info`); `res0_look` is now an identity function (returns the info pointer) and `res0_arena_size` returns `0` - no more `vorbis_look_residue0`, `partbooks`, or `decodemap`; `_01inverse`/`res2_inverse` decode each partition codeword's class indices arithmetically (lowmem's classword digit decomposition) instead of a `decodemap[]` lookup, and `partword` rows are flat `unsigned char` class-index arrays instead of `int` pointer arrays into `decodemap`; `free_look` no-op; dead `#ifdef TRAIN_RES` block removed (referenced a `training_data` struct member absent from this fork, it never-compiled encoder-training scaffolding) |
| `floor0.c` | Setup validation (`ampbits` range, `numbooks<1`, referenced books must have a value mapping and `dim>=1`); `floor0_unpack` NULL-checks its `vorbis_info_floor0` allocation; unsigned-cast shift fix in `vorbis_coslook2_i` and the `floor0_inverse1` amplitude decode (UB cleanups); `vorbis_invsqlook_i` exponent guard; `vorbis_lsp_to_curve` made `static`; `floor0_look` routed to the DSP setup arena + `floor0_arena_size`; `free_look` no-op; unused params cast to `void` (`vorbis_lsp_to_curve`'s `ln`, `floor0_inverse2`'s `vb`) |
| `floor1.c` | `<math.h>` removed (no behavioral change); unsigned-cast shift fix on `room` (UB cleanup); `floor1_unpack` NULL-checks its `vorbis_info_floor1` allocation and right-sizes the six setup arrays to the parsed counts (staged allocation as each count is discovered, plus a post-count pre-pass before the `postlist` fill; freed by `floor1_free_info`); `floor1_inverse2` tail loop converted from `out[j]*=ly` (raw 0-255 dB index) to `out[j]=MULT31_SHIFT15(out[j],FLOOR_fromdB_LOOKUP[ly])`, matching `render_line`. Upstream Tremor never applies the dB lookup here. Dead for spec-valid streams (post X=n is always present, so `hx==n` and the loop body never runs); fixes the in-bounds wrong output for degenerate floor configs where `hx<n`. `ly` is guarded to [0,255]; `floor1_look`'s unused `mi` param cast to `void`; (floor1 look-layer elimination) `floor1_unpack` now also builds `posts`/`forward_index`/`hineighbor`/`loneighbor` - moved verbatim from the old `floor1_look`, reusing the postlist duplicate-check's sort instead of re-sorting; `floor1_look` is an identity function returning the info pointer and `floor1_arena_size` returns `0`; `quant_q` (a function of `mult`) becomes a static lookup table (`floor1_quant_q[4]`) read in `floor1_inverse1` instead of a look-struct field; `free_look` no-op |
| `asm_arm.h` | `CLIP_TO_15` (`_V_CLIP_MATH`) section removed; ARM multiply/LSP helpers retained; unsigned-cast shift fixes in `MULT31`/`XPROD31`/`XNPROD31` (UB cleanup; the `_ARM_ASSEM_` asm path is never built on supported targets) |
| `mdct.c` | Backward transform replaced by the lowmem half-block design (`presymmetry`, `mdct_butterflies`, permutation-only `mdct_bitreverse`, `mdct_step7`, `mdct_step8`) plus `mdct_shift_right` and `mdct_unroll_lap`; two behavioral fixes relative to upstream lowmem (unroll cross-lap negation order for bit-exactness, `mdct_step8` case 0's `x+5,x+6` -> `x+6,x+7` off-by-one) and one intentional divergence (`mdct_unroll_lap` emits raw s7.24 int32 instead of clipped 16-bit); the master-shaped full-block pipeline (including the `const LOOKUP_T *` / `-Wshadow` fixes that used to live in it) is deleted (see Synthesis / MDCT Subsystem) |
| `mdct.h` | `mdct_backward`'s signature drops its `out` parameter (transforms `in` in place, deinterleave deferred to readout); the encode-only, unused `mdct_forward` declaration is removed; `mdct_shift_right` and `mdct_unroll_lap` declared (lowmem synthesis port) |
| `window.c` | `const LOOKUP_T *` fixes; `<math.h>` removed; window ramps unrolled by 4; `_vorbis_apply_window` removed once the lowmem synthesis port left it with no callers (windowing moved to readout-time `mdct_unroll_lap` - see Synthesis / MDCT Subsystem and Dead-Code Policy) |
| `window.h` | `_vorbis_apply_window` declaration removed to match `window.c` |
| `bitwise.c` | Decode-only subset plus Xtensa funnel-shift fast path |
| `synthesis.c` | Packet-supplied `mode` bounded against `ci->modes` in `_vorbis_synthesis1` and `vorbis_packet_blocksize` before any `mode_param[mode]` access (required by the right-sized `mode_param` table); `_vorbis_synthesis1` also saves each kept channel's previous-block iMDCT tail via `mdct_shift_right` into `vd->mdctright[]` before the mapping inverse overwrites `vd->work[]`, run only after every header-parse error check so a malformed packet cannot advance lap state (see Synthesis / MDCT Subsystem); no longer near-byte-identical to upstream master (see Dead-Code Policy) |

Unchanged from upstream master: `registry.{c,h}`,
`lsp_lookup.h`, `mdct_lookup.h`, `window_lookup.h`.

`config_types.h` (an unused autotools type-definition header that nothing
`#include`s) was dropped from the fork; `ogg/os_types.h` supplies the
integer typedefs the decoder actually uses.

`synthesis.c` no longer diverges from upstream by only the two
`mode >= ci->modes` guards: the lowmem synthesis port added the
`mdct_shift_right` tail-save loop described above. The `#include "block.h"`
is upstream's, but the `block.h` it includes now supplies the arena
allocator, so the `_vorbis_block_alloc` / `_vorbis_block_ripcord` it calls
are the fork's `static inline` arena versions, not upstream's extern chain
allocator.
