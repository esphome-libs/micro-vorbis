# Modifications to Upstream Tremor

This directory is a fork of [Tremor](https://gitlab.xiph.org/xiph/tremor)
(aka `libvorbisidec`), Xiph.Org's fixed-point Vorbis decoder. The original
source is licensed under a BSD-3-Clause-style license; see [COPYING](COPYING).

## Provenance

The fork is upstream's **master** tree with two subsystems taken from
upstream's **lowmem** branch, plus a bit reader taken from
[libogg](https://gitlab.xiph.org/xiph/ogg). Everything is edited in place;
there is no patch or staging step.

| Origin | Files |
| --- | --- |
| Tremor master, byte-identical | `registry.{c,h}`, `lsp_lookup.h`, `mdct_lookup.h`, `window_lookup.h` |
| Tremor master, modified | `block.{c,h}`, `synthesis.c`, `info.c`, `mapping0.c`, `res012.c`, `floor0.c`, `floor1.c`, `window.{c,h}`, `backends.h`, `codec_internal.h`, `ivorbiscodec.h`, `misc.h`, `os.h`, `asm_arm.h`, `mdct.h` |
| Tremor lowmem, modified | `codebook.{c,h}` (single-step codebook unpack, compact decode tables), `mdct.c` (half-block backward transform, readout-time overlap-add) |
| libogg, decode-only subset | `bitwise.c`, `ogg/ogg.h`, `ogg/os_types.h` |
| Fork-only | `custom_allocator.h`, this file |
| Dropped | `sharedbook.c` (obsoleted by the lowmem codebook design), `config_types.h` (unused), master's `ivorbisfile.*`/`vorbisfile.c` (Ogg framing is handled by micro-ogg-demuxer) |

Master supplies the file layout, the backend vtable dispatch
(`backends.h`/`registry.c`), the `vorbis_block`/`vorbis_dsp_state` API split,
and the floor/residue/mapping/info parsers. Lowmem supplies the codebook
subsystem and the MDCT plus the `work`/`mdctright` synthesis-buffer scheme,
which the fork grafts onto master's block-based API. Lowmem's own
chained-buffer bit reader, fused `dsp.c`, and look-free `codec_internal.h`
are not used. Where the fork's design merely resembles lowmem (the residue and
floor1 setup structs carrying their own decode-time precompute), the code is
fork-written inside master's file and function structure.

## Dead-Code Policy

Upstream fixes are ported by diffing against the original sources, so a
file's diffability is a maintenance asset and decides how unreachable code is
handled:

- **Byte-identical files** (`registry.{c,h}`, the `*_lookup.h` tables) keep
  their dead code.
- **Forked files** drop unreachable code: the comment-query API,
  `sharedbook.c`, the encode-only entry points, `vorbis_synthesis_init`,
  `vorbis_synthesis_idheader`, `vorbis_info_blocksize`, and
  `_vorbis_apply_window` are gone.
- **Documented API surface stays even without in-tree callers**:
  `vorbis_synthesis_trackonly`, `vorbis_packet_blocksize`, and the
  `decodep == 0` arm of `_vorbis_synthesis1` remain in `synthesis.c`
  (fast-forward without decode; blocksize lookup for seeking). They report 0%
  in fuzzer coverage by design.
- **Upstream vtable slots stay populated**: the `*_free_look` hooks in
  `backends.h` are documented no-ops (the DSP arena frees everything in one
  shot) rather than NULL slots that would need a call-site guard.

## Bit Reader (from libogg)

`bitwise.c`, `ogg/ogg.h`, and `ogg/os_types.h` are copied from libogg, not
from lowmem's chained-buffer reader.

- `bitwise.c` keeps only the five read-side functions Tremor uses
  (`oggpack_readinit`, `oggpack_look`, `oggpack_adv`, `oggpack_read`,
  `oggpack_bytes`). The write side, the `oggpackB_*` family, and the
  `_V_SELFTEST` block are absent.
- `ogg/ogg.h` declares the flat `oggpack_buffer`, those five readers, and
  `ogg_packet`. The Ogg page/stream/sync framing API is absent.
- `ogg/os_types.h` replaces libogg's platform `#ifdef` ladder with
  `#include <stdint.h>` and direct typedefs, plus the `_ogg_malloc` family
  defaults.
- **Xtensa fast path** (`#ifdef __XTENSA__`): `oggpack_read` and
  `oggpack_look` use two aligned 32-bit loads and the `SRC` funnel-shift
  instruction when `endbyte < storage - 7`, replacing the cascading byte-load
  chain. The fast path reads up to `(ptr & ~3) + 7`, so the guard is
  `storage - 7` rather than the scalar path's `storage - 4`; the scalar path
  handles the last bytes.

## Codebook Subsystem (from the lowmem branch)

`codebook.h` is lowmem's (the `codebook` struct with `dec_table`,
`dec_nodeb`, `dec_leafw`, `dec_maxlength`, `q_min`/`q_del`/`q_bits`/`q_pack`,
`dec_type`). `codebook.c` is lowmem's design with the hardening and
performance changes listed below.

- `vorbis_book_unpack` parses the codebook header and builds the final decode
  structures in one pass. There is no `static_codebook`, no `sharedbook.c`,
  and the bitstream `lengthlist` exists only transiently during unpack.
- The Huffman tree is a packed `dec_table` with a per-book node size
  (`dec_nodeb`: 1/2/4 bytes) and leaf width (`dec_leafw`), decoded by a
  tree walk in `decode_packed_entry_number`. There is no first-table
  acceleration and no precomputed `valuelist`; values are dequantized on the
  fly (`dec_type` 1/2/3) from constants computed once at unpack.
- `codec_internal.h` holds a single heap-allocated `codebook *book_param`
  flat array (allocated in `_vorbis_unpack_books`, `info.c`) instead of
  master's `static_codebook *book_param[256]` plus `codebook *fullbooks`.
- **`oggpack_eop` emulation**: lowmem's codebook code expects `oggpack_eop()`
  from its chained-buffer reader. `codebook.c` supplies a `static inline`
  that reports end-of-packet when `b->ptr == NULL`, the flat-buffer overrun
  signal set by `oggpack_read`/`oggpack_adv` when a read goes past the end. A
  read landing exactly on the last bit is not an error. Used at three
  header-parse sites in `vorbis_book_unpack` and once in `decode_map_apply`.
  A companion `oggpack_seteop` sets the same sentinel.

### Hardening relative to lowmem

Lowmem trusts the bitstream in places where a crafted setup packet can modify
the stack, overflow, or hang. The fork adds these checks (some port libvorbis
commits `28965ede` and `a629068d`):

- **No setup-path `alloca`**: the transient `lengthlist` (`entries` is a
  24-bit field, up to 16 MB), the maptype-1 `q_val` scratch (`quantvals` can
  reach `2^23`), and `_make_decode_table`'s `work` array (up to ~64 KB) are
  heap-allocated with NULL checks and freed on every exit path.
- **Allocation failures fail cleanly**: `dec_table` (both the `nodeb==4` and
  packed paths), both long-lived `q_val` allocations, and `ci->book_param`
  are NULL-checked and fail through `vorbis_book_clear` / the header-error
  path, so `vorbis_synthesis_headerin` returns `OV_EBADHEADER`.
- **`dim < 1` rejected at unpack**: it would otherwise reach `n/dim`
  divisions on the decode path.
- **`_book_maptype1_quantvals`** is `static`, saturates its `acc *= vals` /
  `acc1 *= vals+1` products against `b->entries` instead of overflowing
  `long` (which could also make the loop never terminate), returns early for
  `dim < 1` / `entries < 1`, and guards the decrement branch on `vals <= 1`.
- **`_make_words` is bounds-checked**: it takes an `rn` parameter (the work
  buffer size) and rejects lengthlists whose tree chase would write past it.
  It also rejects underpopulated trees (one used entry excepted), a check
  master's `sharedbook.c` had and lowmem lacks; without it interior child
  slots point back at the root and `decode_packed_entry_number` can return
  `0xffffffff` without tripping end-of-packet.
- **Ordered-case validation**: rejects `length == 0`, codeword counts that
  cannot fit in `length` bits (`num-1 >= 2^length`), and `num` exceeding the
  remaining entries. The unordered case checks up front that the claimed
  entry count fits in the bytes left in the packet.
- **Shift-count guards**: `decode_map_ctx_init` rejects `|shift| >= 32` for
  the `q_min` and `q_del` dequantization shifts once per
  `vorbis_book_decodev*` call, and the maptype-2 / `dec_type 2` path rejects
  `q_pack * dim > 32` at unpack (it would produce out-of-range shifts in
  `decpack`).
- **`dec_type 3` index bound**: `decode_map_apply` rejects
  `entry >= used_entries` before using the entry as an index into the packed
  `q_val` array. Types 1 and 2 use the entry as packed bits, not an index.
- **Decode-path vector scratch off the stack**: the per-vector `v` scratch in
  `vorbis_book_decodevs_add`, `vorbis_book_decodev_add`,
  `vorbis_book_decodev_set`, and `vorbis_book_decodevv_add` is a fixed
  32-value stack buffer (`DECODE_VEC_STACK`; real encoders emit `dim <= 8`)
  with a checked heap fallback freed on all paths, instead of
  `alloca(4*dim)` (`dim` can reach 65535 from a crafted book).
- **Tree-walk fall-off forces end-of-packet**: when
  `decode_packed_entry_number` uses all `dec_maxlength` bits without reaching
  a leaf it calls `oggpack_seteop` rather than relying on the trailing
  `oggpack_adv`, whose overflow check is byte-granular. Only a one-used-entry
  book can reach this path.
- **Zero-entry codebooks parse on ESP-IDF**: the `lengthlist` and `dec_type 3`
  `q_val` allocation sizes are clamped to at least one byte, because
  `heap_caps_malloc(0)` returns NULL on ESP-IDF and the NULL checks would
  misread that as OOM.
- **Floor 0 book validation** (`floor0.c`): `floor0_unpack` rejects
  `numbooks < 1` and referenced books with `dec_type == 0` (no value mapping,
  unusable by `vorbis_book_decodev_set`) or `dim < 1`.

### Performance relative to lowmem

- **Hoisted dequantization invariants**: lowmem's `decode_map` recomputes the
  add/shift/mask constants for every vector. The fork splits it into
  `decode_map_ctx_init` (a `decode_map_ctx` of loop invariants computed once
  per `vorbis_book_decodev*` call; also where the shift validation lives) and
  `decode_map_apply` (the per-vector hot path). The `q_bits <= 8`
  byte-vs-short branch in the `dec_type 2` path is hoisted out of the
  per-dimension loop with pre-typed pointers, and the `dec_type 3` path uses
  typed pointer arithmetic instead of `void *` arithmetic.
- **Codebook-specific memory placement**: `book_param`, `dec_table`, and
  `q_val` are allocated through `_ogg_codebook_malloc`/`_ogg_codebook_calloc`,
  which on ESP-IDF carry a separate Kconfig placement policy defaulting to
  prefer-internal RAM (decode tables are accessed with random-access patterns
  on the hot path). Host builds map them to `_ogg_malloc`/`_ogg_calloc` in
  `os.h`.

## Synthesis / MDCT Subsystem (from the lowmem branch)

`mdct.c` is lowmem's half-block transform. The surrounding buffer scheme is
lowmem's too, but hosted in master's `block.c`/`synthesis.c` and master's
`vorbis_block`/`vorbis_dsp_state` API rather than lowmem's fused `dsp.c`.

- **`mdct_backward` transforms n/2 values in place** (`presymmetry`,
  `mdct_butterflies`, permutation-only `mdct_bitreverse`, `mdct_step7`,
  `mdct_step8`). The cross-product deinterleave, windowing, and overlap-add
  are deferred to `mdct_unroll_lap()`.
- **`mdct_shift_right`** saves the n/4 odd-indexed values of a
  just-transformed block into a tail buffer for the next block's overlap-add.
- **`mdct_unroll_lap`** reconstructs, windows, and overlap-adds the current
  half-transform against the previous block's saved tail, writing
  `[start,end)` samples of the frame at the given stride. It runs once per
  readout chunk from `vorbis_synthesis_lapout()`, so a partial or retried
  read replays the same reconstruction.
- **Two fixes relative to lowmem** (`mdct.c`):
  - `mdct_unroll_lap`'s cross-lap for the negated-mirror region computes
    `+ MULT31(-*l,*wL++)` instead of lowmem's `- MULT31(*l,*wL++)`. `MULT31`
    truncates, so the two differ; negating the sample before the multiply
    matches master's full-block pipeline bit-exactly. Lowmem's version is off
    by a uniform 2 s7.24 units in these regions.
  - `mdct_step8` case 0's last cross-product writes `x+6,x+7`; lowmem writes
    `x+5,x+6`, colliding with the pair written just above it and corrupting
    every 4th sample of blocksize-8192 streams. Present in upstream lowmem;
    mainstream encoders rarely emit that blocksize.
- **Raw fixed-point readout**: `mdct_unroll_lap` emits s7.24 `int32` samples
  instead of lowmem's `CLIP_TO_15` 16-bit output. Rounding and clipping live
  in the wrapper (`clip_to_16()` in `ogg_vorbis_decoder.cpp`), which needs
  the extra precision to round-to-nearest.
- **`work`/`mdctright` buffers** (`ivorbiscodec.h`, `block.c`, `mapping0.c`,
  `synthesis.c`): `vorbis_dsp_state::work[i]` is a `blocksizes[1]/2` `int32`
  plane per channel that `mapping0_inverse` writes the floor-applied spectrum
  into and `mdct_backward` transforms in place. It exists for every channel
  because residue decode and channel coupling touch every channel.
  `vorbis_dsp_state::mdctright[i]` is a `blocksizes[1]/4` `int32` overlap
  tail per kept channel (`NULL` for dropped ones). Persistent PCM state per
  kept channel is `3/4 * blocksizes[1]` `int32`s. Both live in the DSP setup
  arena (see Memory Management); the block arena holds no PCM.
- **Readout API** (`ivorbiscodec.h`, `block.c`): `vorbis_synthesis_pcmavail()`
  returns the pending sample count and `vorbis_synthesis_lapout()`
  reconstructs one channel into a caller-supplied buffer without consuming.
  `vorbis_synthesis_read()` is the only consuming call. This split is the
  fork's; master has `vorbis_synthesis_pcmout` (plane passback), lowmem has
  `vorbis_dsp_pcmout` (count-and-write in one call). The wrapper strip-mines
  readout in fixed 64-sample chunks (`LAPOUT_CHUNK`).
- **`mdct_shift_right` placement** (`synthesis.c`): `_vorbis_synthesis1`
  calls it for each kept channel after every header-parse `OV_EBADPACKET`
  check has passed and before the mapping inverse overwrites `vd->work[]`,
  using `vd->W` (still the previous block's flag) to size the shift. Lowmem
  calls it unconditionally for every packet, including non-decoded ones, and
  has no channel mask.
- **`vorbis_synthesis_blockin` is bookkeeping-only** (`block.c`): it moves no
  PCM. It advances `lW`/`W`/`granulepos`/`sample_count` and opens the
  `out_begin`/`out_end` readout window (lowmem's names) that
  `vorbis_synthesis_lapout` reconstructs from, with master's granule-trim
  logic hardened as described under Undefined-Behavior Cleanups.
- **`window.c`/`window.h`** contain only `_vorbis_window()`, the window-table
  selector `_vds_init` calls. Windowing itself happens in `mdct_unroll_lap`.

## Memory Management

- **PSRAM-aware allocator** (`custom_allocator.h`, fork-only): on ESP-IDF
  builds `_ogg_malloc`, `_ogg_calloc`, `_ogg_realloc`, and `_ogg_free` map to
  `heap_caps_*` (`heap_caps_malloc_prefer` / `calloc_prefer` /
  `realloc_prefer` for the prefer modes, plain `heap_caps_malloc` / `calloc`
  / `realloc` for the strict modes) under a Kconfig policy: prefer PSRAM,
  prefer internal, PSRAM-only, or internal-only. A second pair,
  `_ogg_codebook_malloc` / `_ogg_codebook_calloc`, carries an independent
  policy for codebook tables (default: prefer internal). Non-ESP-IDF builds
  use plain `malloc` / `free`. `os.h` includes the header so the overrides
  apply project-wide.
- **Block arena in `vorbis_block`** (`block.{c,h}`, `ivorbiscodec.h`):
  master's chained per-frame allocator is replaced by one pre-sized arena
  (`arena_data`, `arena_capacity`, `arena_used`) reset each frame by
  `_vorbis_block_ripcord()`. `_vorbis_arena_compute_size()` in `block.c`
  sizes it once from `codec_setup_info`: per-mode mapping bundles, floor
  memos, residue `partword` arrays (flat `unsigned char` rows of
  `partwords * dim` bytes per channel), plus a residue-2 term that reserves
  `min(channels,16) * max_bytes_2` because `res2_inverse`'s per-submap array
  size is channel-independent and several submaps may use residue type 2.
  Since `_vorbis_block_alloc` rounds each allocation up to `ARENA_ALIGN`, an
  alignment-slack term of `(3*channels + 4) * (ARENA_ALIGN - 1)` covers the
  bounded per-packet allocation count (4 mapping bundles plus one floor memo,
  one residue inner array, and one residue outer array per channel). The
  block arena holds no PCM. `_vorbis_block_alloc`'s NULL return is not
  checked on the decode path, so the size must be exact. `vorbis_block_init`
  returns `-1` on arena allocation failure (master always returned `0`).
  `_vorbis_block_alloc` and `_vorbis_block_ripcord` are `static inline` in
  `block.h`.
- **`ARENA_STACK` macro** (`block.h`) replaces decode-path `alloca` for
  `mapping0.c`'s `pcmbundle`/`zerobundle`/`nonzero`/`floormemo` and
  `res012.c`'s `partword` outer pointer array. The codebook decode helpers do
  not use the arena (see the codebook section); `floor0.c`'s order-sized
  `ilsp` array in `vorbis_lsp_to_curve` remains an `alloca`.
- **DSP setup arena in `vorbis_dsp_state`** (`block.{c,h}`, `backends.h`,
  `ivorbiscodec.h`, `floor0.c`): the whole per-stream DSP state is one
  allocation.
  - Fields: `setup_arena_data`, `setup_arena_capacity`, `setup_arena_used`.
    Bump-allocated via `_vorbis_setup_alloc` / `_vorbis_setup_calloc`
    (`static inline` in `block.h`, `ARENA_ALIGN` rounding through the shared
    `_vorbis_arena_round`).
  - Sizing: `_vorbis_dsp_arena_compute_size()` in `block.c` mirrors every
    allocation in `_vds_init` in order (`private_state`, the `work`/
    `mdctright`/`channel_keep` pointer arrays, each channel's `work` plane,
    each kept channel's `mdctright` tail, `b->mode`, and each mode's backend
    lookups via an `arena_size(...)` callback on the floor/residue/mapping
    vtables in `backends.h`). The computed size equals the arena's final used
    watermark; a 256-byte `DSP_ARENA_SAFETY` pad is added. The total is
    accumulated in `ogg_int64_t` and `_vds_init` rejects any header whose
    total exceeds `DSP_ARENA_MAX_BYTES` (`2^31-1`), since 64 modes * 16
    submaps of floor lookups can exceed `2^31` and would wrap a 32-bit
    `long` on ESP32 into an undersized, silently overrun arena.
  - The channel-keep mask is fixed before sizing via
    `vorbis_synthesis_init_ex(v, vi, keep, n)` (`keep == NULL` keeps all
    channels), so a dropped channel's `mdctright` is never allocated. The mask
    is immutable for the stream.
  - `vorbis_dsp_clear` is a single `_ogg_free(setup_arena_data)`; the backend
    `free_look` hooks are no-ops. `_vds_init` returns `-1` if the arena
    allocation fails. Codebooks are separate: they belong to `vorbis_info`.
- **No look layer for residue, floor1, or mapping0** (`backends.h`,
  `res012.c`, `floor1.c`, `mapping0.c`): every decode-time precompute lives
  in the parsed setup struct, built at unpack (lowmem's approach, written
  into master's vtable structure). `res0_look`, `floor1_look`, and
  `mapping0_look` return the info pointer; their `arena_size` callbacks
  return `0` (mapping0's is the per-submap sum of the floor/residue
  callbacks, nonzero only for a floor0 submap).
  - `vorbis_info_residue0` carries `stagemasks`/`stagebooks`/`stages`,
    heap-allocated in `res0_unpack` to `partitions`/`partitions*8` and freed
    by `res0_free_info`. `_01inverse`/`res2_inverse` (master's per-type
    split, not lowmem's single `res_inverse`) decode each partition
    codeword's class indices arithmetically: the phrasebook entry is split
    into `partitions_per_word` digits, most-significant first, by successive
    division against descending powers of `info->partitions`. `partword` rows
    are flat `unsigned char` class-index arrays. The `temp >= info->partvals`
    check is kept (lowmem omits it); it is what guarantees every digit is
    `< partitions`, which `stagemasks`/`stagebooks` are sized for.
  - `vorbis_info_floor1` carries `posts`, `forward_index`, `hineighbor`,
    `loneighbor` as `unsigned char` arrays (every index is `< posts <= 65`),
    heap-allocated in `floor1_unpack` and freed by `floor1_free_info`.
    `forward_index` is filled inside the postlist duplicate-value check,
    reusing its `sortpointer` sort. `quant_q` is a static table
    (`floor1_quant_q[4]`, indexed by `mult`) read in `floor1_inverse1`.
  - `mapping0_inverse` dispatches per submap off `ci->floor_type[]` /
    `ci->residue_type[]` into `_floor_P` / `_residue_P` and reads
    `ci->floor_param[]` / `ci->residue_param[]` as the look. floor0 is the
    one backend whose look computes real state (`linearmap`/`lsp_look`,
    sized by the mode's blockflag as well as the floor), so
    `vorbis_info_floor0` carries a two-entry `look_cache` indexed by
    blockflag that `mapping0_look` fills from the setup arena. This is safe
    because a `codec_setup_info` serves exactly one `vorbis_dsp_state` and
    both are torn down together (`vorbis_dsp_clear` then
    `vorbis_info_clear`).
- **Right-sized setup structs**: spec-maximum fixed arrays are heap pointers
  sized to the parsed counts, each count validated against a compile-time
  cap equal to the old fixed size before allocating.
  - `codec_setup_info` (`codec_internal.h`, `info.c`): `mode_param`,
    `map_type`, `map_param`, `time_type`, `floor_type`, `floor_param`,
    `residue_type`, `residue_param` are allocated by `_vorbis_unpack_books`
    to `modes`/`maps`/`times`/`floors`/`residues` (cap `VI_SETUP_MAX`, 64).
    Each count is published to `ci` only after its table(s) are calloc'd, so
    `vorbis_info_clear`'s count-bounded free loops never index a NULL table.
    Because an audio packet's `mode` field is read with `ilog(modes)` bits, a
    non-power-of-two mode count lets a packet encode `mode >= modes`, which
    against a right-sized table is a heap OOB read; `_vorbis_synthesis1` and
    `vorbis_packet_blocksize` bound `mode` against `ci->modes` (returning
    `OV_EBADPACKET`) before any `mode_param[mode]` access. `passlimit` and
    `coupling_passes` keep master's fixed shape.
  - `vorbis_info_mapping0` (`mapping0.c`): `chmuxlist`, `coupling_mag`,
    `coupling_ang` sized to `vi->channels` / `coupling_steps` (caps
    `VIM_CHANNELS`, `VIM_COUPLES`), freed by `mapping0_free_info`.
    `chmuxlist` is always calloc'd because `mapping0_inverse` indexes it for
    every channel even when `submaps == 1` leaves it unfilled;
    `coupling_mag`/`ang` stay NULL when there is no coupling.
  - `vorbis_info_residue0` (`res012.c`): `secondstages`, `booklist` sized to
    `partitions` and the cascade-bit sum `acc` (caps `VIR_PARTS`,
    `VIR_BOOKS`), freed by `res0_free_info`. `booklist` uses `acc?acc:1` so a
    zero-book residue never hits `malloc(0)`.
  - `vorbis_info_floor1` (`floor1.c`): `partitionclass`, `class_dim`,
    `class_subs`, `class_book`, `class_subbook`, `postlist` sized to
    `partitions` (`VIF_PARTS`), `maxclass+1` (`VIF_CLASS`), and the post
    count (`VIF_POSIT`), freed by `floor1_free_info`. Allocation is staged as
    each count is discovered, with a pre-pass totalling
    `class_dim[partitionclass[j]]` before `postlist` is allocated. When
    `partitions == 0` the partition/class arrays stay NULL; `postlist` is
    always allocated (entries 0 and 1 are unconditional). `class_book` and
    `class_subbook` are calloc'd because `class_book[j]` is read even when
    `class_subs[j] == 0`; `class_subbook` is a flat `[maxclass+1][8]` array
    with a fixed stride of 8. No packet-controlled index reaches any of these
    arrays. `vorbis_info_floor0` (~104 bytes) is left fixed.
- **No comment handling** (`info.c`, `ivorbiscodec.h`): the `vorbis_comment`
  struct, `vorbis_comment_init`/`_clear`/`_query`/`_query_count`, and their
  `tagcompare`/`_v_toupper` helpers do not exist. `_vorbis_unpack_comment`
  takes only the `oggpack_buffer`, validates the packet, and skips it via
  `oggpack_adv` without allocating. `vorbis_synthesis_headerin` has no
  `vorbis_comment *` parameter; the id, comment, setup header ordering is
  tracked by `vorbis_info::comment_header_seen`. The wrapper streams the real
  comment packet past unbuffered and feeds a synthetic empty one.
- **Per-channel decode mask** (`ivorbiscodec.h`, `block.c`, `mapping0.c`,
  `synthesis.c`): `vorbis_dsp_state::channel_keep` is a packed bitmask
  (`VORBIS_KEEP_BYTES` bytes, one bit per channel, read/written by the
  `vorbis_keep_*` inlines) set through `vorbis_synthesis_init_ex()`.
  Entropy decode and inverse coupling always run for every channel (residue
  bits are interleaved and coupling mixes partner channels); the mask gates
  only the per-channel tail:
  - CPU: `mapping0_inverse` skips floor-apply (`inverse2`) and the half-block
    iMDCT for unkept channels; `_vorbis_synthesis1` skips their
    `mdct_shift_right`; `vorbis_synthesis_lapout` returns `OV_EINVAL` for an
    unkept channel. Kept channels traverse identical code, so their output is
    bit-exact against a full decode.
  - Memory: `work[i]` is allocated for every channel; only `mdctright[i]` is
    skipped for dropped channels. Saving is
    `(channels - kept) * (blocksizes[1]/4) * 4` bytes.

## Portability and Housekeeping

- **`os.h`**: includes `custom_allocator.h`; defines host fallbacks for
  `_ogg_codebook_malloc` / `_ogg_codebook_calloc`; keeps the
  `HAVE_CONFIG_H`-guarded `#include "config.h"`. Its `STIN` ladder
  (including VBCC/Watcom branches) and the absence of `<math.h>`, `M_PI`, and
  `rint` come from the Tremor snapshot the fork was imported from and differ
  from the current gitlab `master`/`lowmem` `os.h`; they are not fork edits.
- **`misc.h`**: the endianness union is gated on a single `WORDS_BIGENDIAN`
  rather than separate `BIG_ENDIAN`/`LITTLE_ENDIAN` layouts; `LOOKUP_T` has
  no `const` qualifier (so `mdct.c` declares `const LOOKUP_T *` where it
  reads tables); `<sys/types.h>` is gated on `HAVE_SYS_TYPES_H`. The
  `VFLOAT_MULT`, `VFLOAT_MULTI`, `VFLOAT_ADD` helpers and `CLIP_TO_15`
  (`misc.h` fallback and `asm_arm.h` asm version alike) are absent; nothing
  in the fork uses them, and 16-bit saturation is the wrapper's.
- **`info.c`**: no `<ctype.h>`; bitrate fields are cast to `ogg_int32_t`;
  the setup-header unpack functions reject non-positive
  book/floor/residue/map/mode counts up front; every per-mode/floor/residue/
  mapping struct allocation on the header-parse path is NULL-checked and
  bails through the existing error paths.
- **`mapping0.c`**: no redundant `memset` after `_ogg_calloc` in
  `mapping0_unpack`; master's file-scope `static int seq` debug counter and
  the commented-out `_analysis_output` scaffolding are absent (the counter's
  non-atomic increment was a data race across concurrent decoder instances),
  as is the `<stdio.h>` include they needed.
- **`res012.c`**: master's `#ifdef TRAIN_RES` block is absent (it referenced
  a `training_data` member that never existed on this struct).
- **`floor0.c`**: `vorbis_lsp_to_curve` is `static`. The floor 0 curve math
  is master's.
- **`floor1.c`**: no `<math.h>`. `floor1_inverse2`'s tail loop applies the
  dB lookup (`out[j] = MULT31_SHIFT15(out[j], FLOOR_fromdB_LOOKUP[ly])`,
  `ly` clamped to `[0,255]`) where both upstream branches multiply by the raw
  dB index (`out[j] *= ly`). Spec-valid streams never run this loop (post
  `X=n` is always present, so `hx == n`); it fixes the output for degenerate
  floor configs where `hx < n`.
- **`mdct.h`**: `mdct_backward` has no `out` parameter; `mdct_forward` is not
  declared; `mdct_shift_right` and `mdct_unroll_lap` are.
- **`ivorbiscodec.h`**: no encoder-only declarations (`vorbis_comment_add`,
  `vorbis_comment_add_tag`); `vorbis_block` carries the block-arena fields
  and `vorbis_dsp_state` the setup-arena, `work`, `mdctright`, and
  `channel_keep` fields; `vorbis_synthesis_init_ex` is the only
  synthesis-init entry point; `vorbis_synthesis_read`'s parameter is named
  `samples` in both declaration and definition.
- **Warning hygiene**: unused parameters required by the backend
  function-pointer signatures are cast to `void` (`vorbis_lsp_to_curve`'s
  `ln`, `floor0_inverse2`'s `vb`, `floor1_look`'s `mi`, the `free_look`
  no-ops). The bulk fixed-point conversion warnings
  (`-Wshorten-64-to-32`, `-Wsign-conversion`) are inherent to Tremor's
  `int`/`long` arithmetic and are left as-is.
- **`COPYING`**: license text is upstream's; the header carries two
  attribution lines (Tremor, and libogg for the bit reader).

## Undefined-Behavior Cleanups

- **Left shift of negative values**: Tremor's fixed-point math left-shifts
  signed values that may be negative. This is well-defined on every supported
  target but UB per the C standard (UBSan `invalid-shift-base`). Each such
  shift operates on the unsigned counterpart and casts back, e.g. `x << 1`
  becomes `(ogg_int32_t)((ogg_uint32_t)x << 1)`, which is bit-identical on
  two's-complement machines. Sites:
  - `misc.h`: `MULT31` and `MULT31_SHIFT15`.
  - `codebook.c`: the `q_min << -shift` and `(v[i] * q_del) << -shift`
    upscaling paths in `decode_map_ctx_init` / `decode_map_apply`; the
    maptype-2 read accumulation in `decpack`; the `next << 16` node combine
    in `decode_packed_entry_number`.
  - `floor1.c`: the `room` doubling in the post-list unwrap.
  - `floor0.c`: the cosine-table term in `vorbis_coslook2_i`.
  - `asm_arm.h`: `MULT31` and the `<<1` doublings in `XPROD31`/`XNPROD31`
    (the `_ARM_ASSEM_` path, never built on Xtensa or host targets).

  Shifts with unsigned bases (`bitwise.c`, the bit-reverse helpers,
  `floor0.c`'s `pi`/`qi`) are untouched. `signed-integer-overflow` in the
  MDCT/residue modular arithmetic is deliberate upstream behavior and is not
  addressed.
- **Dequantization shift counts** (`codebook.c`): the shifts derived from the
  bitstream's `q_minp`/`q_delp` plus the caller's `point` can reach 32.
  `decode_map_ctx_init` rejects `|shift| >= 32` for both once per
  `vorbis_book_decodev*` call and the helper returns `-1`, which every caller
  (`floor0_inverse1`, `_01inverse`, `_02inverse`) treats as a packet error.
- **Truncated id header** (`info.c`): `_vorbis_unpack_info` reads the two
  blocksize exponents into locals and rejects a negative value (the
  `oggpack_read` end-of-packet sentinel) before shifting, instead of
  computing `1 << -1`. Reachable only through `vorbis_synthesis_headerin`;
  the wrapper's 30-byte identification-header minimum keeps in-tree paths
  away from it.
- **Floor 0 setup fields** (`floor0.c`): `floor0_unpack` rejects `ampbits`
  outside `[1, 31]` (a 6-bit field later used as a shift count).
- **`vorbis_invsqlook_i`** (`floor0.c`): returns 0 when the computed
  `(e>>1)+21` shift falls outside `[0, 31]`; malformed floor coefficients can
  drive it negative or past 31. One unsigned compare on the floor-curve path.
- **`floor0_inverse1` amplitude decode**: `maxval = (1U << ampbits) - 1` and
  `(unsigned)ampraw * (unsigned)ampdB` keep the shifts well-defined for
  `ampbits == 31`, which the setup validator allows.
- **Granule-position trim** (`block.c`): `vorbis_synthesis_blockin` trims a
  short or partial-last page by subtracting the page `granulepos` from the
  running sample count, both `ogg_int64_t`. A crafted negative granpos (such
  as `INT64_MIN`) would overflow that subtraction. Both trim sites keep
  `extra` in `ogg_int64_t` and set it to `0` when the granpos is negative (it
  denotes a sample count far beyond anything decoded, so there is nothing to
  trim). Master's `long extra` also narrowed the result on 32-bit targets.
  The clamp that stops a set-EOP frame with a backdated granpos from
  rewinding `out_begin` past `out_end` is master's.

## Files

| File | State |
| --- | --- |
| `bitwise.c`, `ogg/ogg.h`, `ogg/os_types.h` | New, from libogg: decode-only subset; Xtensa funnel-shift fast path in `bitwise.c` |
| `custom_allocator.h` | New: PSRAM-aware allocator overrides and codebook placement policy |
| `CHANGES.md` | New: this document |
| `COPYING` | Upstream license text; header gains Tremor and libogg attribution lines |
| `registry.{c,h}`, `lsp_lookup.h`, `mdct_lookup.h`, `window_lookup.h` | Byte-identical to master |
| `codebook.h` | Lowmem's, unchanged apart from the include path |
| `codebook.c` | Lowmem's design plus the hardening, performance, and UB changes above; codebook allocations routed through `_ogg_codebook_*` |
| `codec_internal.h` | Master's, with a single heap `codebook *book_param` and the eight setup tables as right-sized pointers under `VI_SETUP_MAX` |
| `mdct.c` | Lowmem's half-block transform plus `mdct_shift_right`/`mdct_unroll_lap`; two fixes vs lowmem; raw s7.24 output; `const LOOKUP_T *` table pointers |
| `mdct.h` | In-place `mdct_backward` signature; no `mdct_forward`; declares `mdct_shift_right`/`mdct_unroll_lap` |
| `block.{c,h}` | Block arena and DSP setup arena with their sizing functions; `ARENA_STACK`; `vorbis_synthesis_init_ex`; `pcmavail`/`lapout`/`read` readout; bookkeeping-only `blockin` with hardened granule trim; `work`/`mdctright` allocation |
| `synthesis.c` | Master's, plus the `mode >= ci->modes` guards in `_vorbis_synthesis1` and `vorbis_packet_blocksize` and the kept-channel `mdct_shift_right` loop in `_vorbis_synthesis1` |
| `ivorbiscodec.h` | Arena, `work`/`mdctright`, and `channel_keep` fields; `vorbis_comment` and encoder-only declarations absent; `comment_header_seen` in `vorbis_info`; `vorbis_synthesis_headerin` without a comment parameter |
| `backends.h` | `arena_size` vtable callback; right-sized pointer arrays with `VIM_*`/`VIR_*` caps; residue/floor1 setup structs carry their decode-time precompute; `vorbis_info_floor0::look_cache`; no `vorbis_look_*` struct definitions |
| `info.c` | No comment machinery; lowmem-style flat `book_param` unpack; right-sized setup tables; count and NULL guards; no `vorbis_info_blocksize`/`vorbis_synthesis_idheader`; blocksize-exponent guard |
| `mapping0.c` | `ARENA_STACK` temporaries; writes `vd->work[]`; decode-mask gate on floor-apply/iMDCT; right-sized `chmuxlist`/coupling arrays; look-free dispatch with floor0 `look_cache`; no `seq`/`_analysis_output` |
| `res012.c` | Right-sized `secondstages`/`booklist`; `stagemasks`/`stagebooks`/`stages` built at unpack; arithmetic class-index decode with the `partvals` bound; flat `unsigned char` `partword` rows; identity `res0_look`; no `TRAIN_RES` block |
| `floor0.c` | Setup validation (`ampbits`, `numbooks`, book `dec_type`/`dim`); NULL-checked allocation; unsigned-cast shifts; `vorbis_invsqlook_i` guard; `static vorbis_lsp_to_curve`; `floor0_look` on the setup arena with `floor0_arena_size`; `(void)` casts |
| `floor1.c` | Right-sized setup arrays; `forward_index`/neighbors built at unpack; identity `floor1_look`; `floor1_quant_q[4]`; unsigned-cast `room`; tail loop applies the dB lookup; no `<math.h>` |
| `window.{c,h}` | Only `_vorbis_window()` remains; no `<math.h>` |
| `misc.h` | `WORDS_BIGENDIAN` gate; non-const `LOOKUP_T`; `HAVE_SYS_TYPES_H` gate; unsigned-cast `MULT31`/`MULT31_SHIFT15`; no `VFLOAT_*`, no `CLIP_TO_15` |
| `os.h` | Allocator hooks; codebook allocator host fallbacks |
| `asm_arm.h` | Unsigned-cast shifts in `MULT31`/`XPROD31`/`XNPROD31`; no `CLIP_TO_15` |
