# Modifications to Upstream Tremor

This directory is a fork of [Tremor](https://gitlab.xiph.org/xiph/tremor)
(aka `libvorbisidec`), Xiph.Org's fixed-point Vorbis decoder. The original
source is licensed under a BSD-3-Clause-style license; see [COPYING](COPYING).

The fork draws from two upstream branches: the overall tree structure and
most files come from the **master** branch (`block.c`, `synthesis.c`,
`registry.c`, the backend vtables), while the **codebook subsystem** was
replaced with the design from upstream's **lowmem** branch (single-step
`vorbis_book_unpack`, compact decode tables, no `static_codebook`) and then
modified further. The sections below describe the fork's changes relative
to upstream.

## Dead-Code Policy

Files here are edited in place, and upstream security fixes are ported by
diffing against the original Tremor / libvorbis sources (e.g. libvorbis
`28965ede` / `a629068d`). A file's diffability against upstream is therefore a
maintenance asset, and it decides how aggressively unreachable code is pruned:

- **Files kept byte-identical to upstream** (`synthesis.c`, `registry.{c,h}`,
  the `*_lookup.h` tables) are left verbatim, dead code and all. They expose
  upstream API the decoder never calls (`vorbis_synthesis_trackonly` and
  `vorbis_packet_blocksize` in `synthesis.c`, plus the `decodep == 0` arm of
  `_vorbis_synthesis1` that only `trackonly` reaches), but deleting it would
  forfeit the zero-diff that keeps porting cheap. That code reports 0% in the
  fuzzer coverage report by design; it is not a coverage gap to chase.
- **Files already forked in place** (`info.c`, `block.c`, the floor/residue/
  mapping backends, ...) have no clean upstream diff left to protect, so
  unreachable code in them is removed. This is why the comment-query API,
  `sharedbook.c`, the encode-only entry points, and the standalone unused
  primitives `vorbis_synthesis_init`, `vorbis_synthesis_idheader`, and
  `vorbis_info_blocksize` are gone.
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
  an alignment-slack term of `(4*channels + 5) * (ARENA_ALIGN - 1)`: one
  `ARENA_ALIGN-1` of rounding waste per allocation, with the per-packet
  allocation count bounded by `4*channels + 5` (pcm pointer array + one
  pcm/floor/residue-inner/residue-outer buffer per channel + 4 mapping
  bundles). The slack scales with the channel count because the allocation
  count does; a flat estimate could be overflowed by a high-channel stream
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
  - `res012.c`: the `partword` pointer array in `_01inverse` (its rows and
    `res2_inverse`'s `partword` come from `_vorbis_block_alloc` directly).

  The codebook decode helpers do not use the arena; their per-vector
  scratch is a fixed stack buffer with heap fallback (see the codebook
  section). `floor0.c`'s order-sized `ilsp` array in `vorbis_lsp_to_curve`
  remains an `alloca`.
- **DSP setup arena in `vorbis_dsp_state`** (`block.{c,h}`, `backends.h`,
  `ivorbiscodec.h`, `floor0.c`, `floor1.c`, `res012.c`, `mapping0.c`): the
  per-stream DSP state is the second source of small-allocation churn after the
  per-packet block arena. Upstream `vorbis_synthesis_init` heap-allocates the
  `private_state`, the `pcm`/`pcmret`/`channel_keep` pointer arrays, each
  channel's PCM history buffer, the `b->mode` table, and (the dominant count)
  every floor/residue/mapping lookup that `*_look` builds (a residue `decodemap`
  alone is `partitions^groupbook_dim` separate allocations). On a typical stereo
  stream this is ~240 individual `_ogg_malloc`s. The fork replaces them with one
  pre-sized arena:
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
    allocations 1:1, including using the same local `ilog`. `mapping0_arena_size`
    recurses into the floor/residue `arena_size` callbacks per submap. The mirror
    is exact: the computed size equals the arena's final used watermark. A
    256-byte `DSP_ARENA_SAFETY` pad is added as insurance against an
    overlooked site or platform `sizeof` drift.
  - **Per-look freeing is gone**: the backend `free_look` hooks are now no-ops
    and `vorbis_dsp_clear` releases the entire DSP state in a single
    `_ogg_free(setup_arena_data)`. `_vds_init` returns `-1` if the one arena
    allocation fails.
  - **PCM history buffers fold into the arena.** The per-channel `v->pcm[i]`
    buffers (`blocksizes[1]` int32s each, the bulk of decoder RAM) used to be
    separate per-channel heap allocations. The channel-keep mask is now fixed
    *before* the arena is sized via `vorbis_synthesis_init_ex(v, vi, keep,
    n)` (`keep == NULL` keeps all channels, the behavior-preserving default),
    so dropped channels are never allocated rather than allocated-then-freed, and
    the kept buffers are carved from the arena. The mask is therefore immutable
    for the stream (fixed once at init). The arena is therefore the entire DSP
    heap footprint: one allocation for the whole decoder state instead of
    `1 + N`. (Codebooks are separate: they belong to
    `vorbis_info` and are heap-allocated through the codebook allocators.)
    See the per-channel decode-mask entry below for the CPU/memory details of
    channel selection itself.
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
- **Per-channel decode mask** (`ivorbiscodec.h`, `block.c`, `mapping0.c`):
  a `channel_keep` bitmask on `vorbis_dsp_state` (packed `VORBIS_KEEP_BYTES`
  bytes, one bit per channel; the `vorbis_keep_*` inlines in `ivorbiscodec.h`
  read/write it), set via `vorbis_synthesis_init_ex()`, lets the caller (the
  OggVorbisDecoder wrapper, when channel selection is active) decode only the
  channels it will output. The same bitmask is the caller-facing `keep` argument, so selection
  costs the wrapper a 32-byte stack mask rather than a 1 KB int array. Allocated
  all-set in `_vds_init` and freed in `vorbis_dsp_clear`, so default behavior is
  unchanged. The entropy decode and inverse channel coupling
  always run for every channel (residue bits are interleaved/variable-length and
  coupling mixes magnitude/angle partners, so neither can be skipped per
  channel); the mask only gates the per-channel-independent tail:
  - CPU: `mapping0_inverse` skips floor-apply (`inverse2`), the inverse MDCT
    (`mdct_backward`, the dominant cost), and windowing for unkept channels, and
    `vorbis_synthesis_blockin` skips their overlap-add. Kept channels traverse
    identical code, so their output is bit-exact vs a full decode.
  - Memory: the persistent per-channel PCM history buffer (`v->pcm[i]`,
    `pcm_storage` = `blocksizes[1]` int32s each) is allocated only for kept
    channels. The keep mask is fixed at init via `vorbis_synthesis_init_ex()`
    (`keep == NULL` keeps all channels), so unkept channels are *never
    allocated* rather than allocated-then-freed:
    no transient peak, no per-channel malloc/free churn, and the buffers fold
    into the single DSP setup arena. The mask is therefore immutable for the
    stream (fixed once at init). `vorbis_synthesis_pcmout` reports `NULL` for an
    unkept channel instead of doing NULL+offset pointer arithmetic. Saving is
    `(channels - kept) * blocksizes[1] * 4` bytes; e.g. 40,960 B for 5.1 (6ch)
    -> 1ch at blocksize 2048 (`5 * 2048 * 4`). The per-block scratch (`vb->pcm`,
    from the block arena) is intentionally left full-size as residue decode and
    coupling need every channel.

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
  qualifier is dropped from `LOOKUP_T` in `misc.h`. `window.c` also
  drops `<math.h>` and unrolls the left and right window ramps by 4 in
  `_vorbis_apply_window` to overlap the load/multiply/store latencies;
  scalar tail loops cover any remainder.
- **Warning hygiene (`floor0.c`, `floor1.c`, `mdct.c`)**: the four
  non-conversion warnings that surface when the tremor sources are built
  with `-Wall -Wextra` (the build normally suppresses them with `-w`) are
  silenced without changing behavior. The unused parameters required by
  the floor function-pointer signatures are cast to `void`
  (`vorbis_lsp_to_curve`'s `ln`, `floor0_inverse2`'s `vb`, `floor1_look`'s
  `mi`); in `mdct.c`'s `mdct_backward`, the inner `iX` in the rotate+window
  block reuses the dead function-scope `iX` rather than redeclaring it,
  removing a `-Wshadow`. The bulk fixed-point conversion warnings
  (`-Wshorten-64-to-32`, `-Wsign-conversion`) are inherent to Tremor's
  `int`/`long` arithmetic and are left as-is.

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
  `pcm_current` past `pcm_returned` is unchanged. Well-formed streams
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
| `codec_internal.h` | `static_codebook *book_param[256]` + `codebook *fullbooks` replaced by a single heap-allocated `codebook *book_param` array (lowmem design) |
| `backends.h` | `arena_size(...)` callback added to the floor/residue/mapping vtables for DSP-setup-arena sizing |
| `block.{h,c}` | Block arena alloc/ripcord/sizing + `ARENA_STACK`; `_vorbis_arena_round` and the DSP setup arena (`_vorbis_setup_alloc`/`_calloc`, `_vorbis_dsp_arena_compute_size`, single-free `vorbis_dsp_clear`); `vorbis_synthesis_init_ex` fixes the channel-keep mask before sizing so dropped channels are never allocated and kept `v->pcm[i]` buffers live in the arena; `vorbis_synthesis_pcmout` returns NULL for unkept channels; decode-mask gate in `vorbis_synthesis_blockin`; granpos-difference trim arithmetic in `vorbis_synthesis_blockin` hardened against `ogg_int64_t` overflow on a crafted negative granpos (see UB cleanups); unused channel-blind `vorbis_synthesis_init` shim removed (`vorbis_synthesis_init_ex` is the sole synthesis-init entry point); `block.c` gains `#include "backends.h"`/`"block.h"`; `vorbis_synthesis_read`'s parameter renamed `bytes` to `samples` to match its `ivorbiscodec.h` declaration and its sample-count semantics |
| `os.h` | Allocator and toolchain hooks; host fallbacks for the codebook allocators |
| `misc.h` | endianness and `LOOKUP_T` cleanup; unsigned-cast shift fixes (UB cleanup); removed unused `VFLOAT_*` helpers and `CLIP_TO_15` |
| `codebook.{h,c}` | Replaced with the lowmem-branch single-step design, then modified: `oggpack_eop` emulation; setup `alloca`s (`lengthlist`, `q_val` scratch, `_make_decode_table` `work`) moved to checked heap allocations; NULL checks on `dec_table`/`q_val`/`book_param`; `dim<1` reject; `_book_maptype1_quantvals` saturation; `_make_words` bounds (`rn`); ordered/unordered unpack validation; `decode_map` split into `decode_map_ctx_init`/`decode_map_apply` with shift-range rejection and hoisted invariants; decode `v` scratch = 32-entry stack buffer + heap fallback (was `alloca(4*dim)`); unsigned-cast shift fixes; codebook allocations routed through `_ogg_codebook_*` |
| `info.c` | Comment handling removed (no `vorbis_comment` struct/init/clear/query; `_vorbis_unpack_comment` skips the packet without allocating; `vorbis_synthesis_headerin` tracks `vi->comment_header_seen` and dropped its `vorbis_comment *` arg); `_vorbis_unpack_books` calls the lowmem `vorbis_book_unpack` into a heap-allocated flat `book_param` array (NULL-checked, via `_ogg_codebook_calloc`); non-positive count guards; per-mode `_ogg_calloc` in the mode loop NULL-checked; unused `vorbis_info_blocksize` / `vorbis_synthesis_idheader` primitives removed (see Dead-Code Policy) |
| `mapping0.c` | `ARENA_STACK` on decode-path temporaries; decode-mask gate on floor-apply / iMDCT / window in `mapping0_inverse`; `mapping0_unpack` NULL-checks its `vorbis_info_mapping0` allocation; upstream `seq` debug counter, the dead `_analysis_output` scaffolding, and its now-unused `<stdio.h>` include removed; `mapping0_look` routed to the DSP setup arena + `mapping0_arena_size` (recurses into floor/residue sizing); `free_look` no-op |
| `res012.c` | `ARENA_STACK`/arena allocation for `partword`; `res0_unpack` NULL-checks its `vorbis_info_residue0` allocation; `res0_look` routed to the DSP setup arena + `res0_arena_size`; `free_look` no-op; dead `#ifdef TRAIN_RES` block removed (referenced a `training_data` struct member absent from this fork, it never-compiled encoder-training scaffolding) |
| `floor0.c` | Setup validation (`ampbits` range, `numbooks<1`, referenced books must have a value mapping and `dim>=1`); `floor0_unpack` NULL-checks its `vorbis_info_floor0` allocation; unsigned-cast shift fix in `vorbis_coslook2_i` and the `floor0_inverse1` amplitude decode (UB cleanups); `vorbis_invsqlook_i` exponent guard; `vorbis_lsp_to_curve` made `static`; `floor0_look` routed to the DSP setup arena + `floor0_arena_size`; `free_look` no-op; unused params cast to `void` (`vorbis_lsp_to_curve`'s `ln`, `floor0_inverse2`'s `vb`) |
| `floor1.c` | `<math.h>` removed (no behavioral change); unsigned-cast shift fix on `room` (UB cleanup); `floor1_unpack` NULL-checks its `vorbis_info_floor1` allocation; `floor1_look` routed to the DSP setup arena + `floor1_arena_size`; `free_look` no-op; `floor1_inverse2` tail loop converted from `out[j]*=ly` (raw 0-255 dB index) to `out[j]=MULT31_SHIFT15(out[j],FLOOR_fromdB_LOOKUP[ly])`, matching `render_line`. Upstream Tremor never applies the dB lookup here. Dead for spec-valid streams (post X=n is always present, so `hx==n` and the loop body never runs); fixes the in-bounds wrong output for degenerate floor configs where `hx<n`. `ly` is guarded to [0,255]; `floor1_look`'s unused `mi` param cast to `void` |
| `asm_arm.h` | `CLIP_TO_15` (`_V_CLIP_MATH`) section removed; ARM multiply/LSP helpers retained; unsigned-cast shift fixes in `MULT31`/`XPROD31`/`XNPROD31` (UB cleanup; the `_ARM_ASSEM_` asm path is never built on supported targets) |
| `mdct.c` | `const LOOKUP_T *` fixes; inner `iX` in `mdct_backward`'s rotate+window block reuses the dead function-scope `iX` instead of redeclaring it (removes a `-Wshadow`) |
| `window.c` | `const LOOKUP_T *` fixes; `<math.h>` removed; window ramps unrolled by 4 |
| `bitwise.c` | Decode-only subset plus Xtensa funnel-shift fast path |

Unchanged from upstream master: `registry.{c,h}`, `synthesis.c`,
`window.h`, `lsp_lookup.h`, `mdct.h`, `mdct_lookup.h`,
`window_lookup.h`.

`config_types.h` (an unused autotools type-definition header that nothing
`#include`s) was dropped from the fork; `ogg/os_types.h` supplies the
integer typedefs the decoder actually uses.

`synthesis.c` is byte-identical to upstream (the `#include "block.h"` is
upstream's), but the `block.h` it includes now supplies the arena
allocator, so the `_vorbis_block_alloc` / `_vorbis_block_ripcord` it calls
are the fork's `static inline` arena versions, not upstream's extern chain
allocator.
