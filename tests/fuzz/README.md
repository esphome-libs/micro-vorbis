# microVorbis fuzzer

libFuzzer harness for the Ogg Vorbis decoder.

- `fuzz_ogg_vorbis_decode` drives `OggVorbisDecoder::decode()` with raw Ogg
  Vorbis container bytes in variably-sized chunks. Exercises the Ogg demuxer,
  Vorbis header parsing (identification, comment, setup), and the full Tremor
  bitstream decoder in one pipeline.

## Decoder configuration coverage

The harness also varies how the decoder is *constructed*, so the downmix,
CRC-validation, and raw channel-selection paths get exercised, not just the
default follow-the-file decode. Configuration is consumed from the **tail** of
the input -- `FuzzedDataProvider` integral reads come off the back, so the
**payload prefix is preserved**: only the trailing config/control bytes are
peeled off, so the front of a real `.ogg` seed (including the OSS-Fuzz corpus)
still decodes. The decoder never sees the stripped tail bytes, so the seed's
final partial Ogg page is truncated, but the rest decodes as-is.

- One config byte: bit 0 enables CRC validation; bits 1-2 pick the constructor
  mode -- 0 follow-the-file, 1 mono downmix, 2 stereo downmix, 3 raw role-select;
  bit 3 replays the whole stream a second time across a `reset()`.
- Role-select mode then pulls a count (passed verbatim, so out-of-range values
  exercise the invalid-count fallback) and one role byte per output channel.
- An exhausted provider reads 0, i.e. the historical default: follow-the-file
  channels, CRC off, single pass. Tiny inputs therefore behave exactly as before.

The replay bit drives the re-stream path a looping caller hits: `reset()` frees
and re-allocates the block/DSP/codebook arenas, restarts synthesis state, and
fires a second `STREAM_INFO_READY`. The decoded bytes match the first pass, so it
adds little line coverage; its value is exercising the free/realloc/decode
ordering under ASan, which a single pass never does. Gating it on a bit keeps
non-replay inputs at full throughput.

On every decode the harness asserts a set of **Tier 1 structural invariants**
(single-decode, no reference needed): the reported channel count is positive and
matches the configured target when that target is fixed; the decoded byte
count is a multiple of the channel count times the bytes per sample; a single
decode never writes more than `max_output_bytes()` (the per-call buffer-sizing
bound); and the decoder never reports more bytes than the output buffer holds.
A violation aborts, surfacing it like any sanitizer finding.

## Requirements

- A Clang with the libFuzzer runtime.
  - **macOS:** `brew install llvm`: Apple's stock clang omits the libFuzzer
    runtime, so the Homebrew build is required.
  - **Linux:** the system `clang++` already ships libFuzzer; no extra install.
- ffmpeg with libvorbis on `PATH` for corpus generation.

The build commands below use `$CLANGXX` for the compiler. Point it at the right
Clang for your platform:

```sh
export CLANGXX=$(brew --prefix llvm)/bin/clang++   # macOS / Homebrew LLVM
export CLANGXX=clang++                             # Linux / system clang
```

## Build

```sh
cd tests/fuzz
cmake -B build-libfuzzer -DCMAKE_CXX_COMPILER="$CLANGXX" .
cmake --build build-libfuzzer
```

For crash reproducers without libFuzzer:

```sh
cmake -B build-standalone -DFUZZ_USE_LIBFUZZER=OFF -DCMAKE_CXX_COMPILER="$CLANGXX" .
cmake --build build-standalone
./build-standalone/fuzz_ogg_vorbis_decode path/to/crashing.ogg
```

## Seed corpus

```sh
./generate_seeds.sh           # creates seeds_ogg/
mkdir -p corpus_ogg
cp seeds_ogg/* corpus_ogg/
```

Each generated seed gets a **config tail** appended (see "Decoder configuration
coverage" above): because the harness consumes its config/control bytes from the
back of the input, a bare `.ogg` would lose its final page to those reads. The
tail is a neutral chunk-control region plus one `cfg` byte, so the whole `.ogg`
survives as decoder payload while libFuzzer still has a mutable region to flip
the flags. A few `cfg_*.ogg` variants pre-set `cfg` (CRC on, mono/stereo
downmix, role-select) so those wrapper paths are seeded directly rather than
found by mutation.

Both directories are local-only: the repo-wide `*.ogg` gitignore pattern
keeps the seeds out of the tree, and `corpus_ogg/` is explicitly ignored so
libFuzzer can grow it without polluting `git status`. Regenerate seeds any
time with `./generate_seeds.sh`.

### Merging the OSS-Fuzz public corpus

OSS-Fuzz has been running `vorbis_decode_fuzzer` against upstream libvorbis
for years. Its accumulated public corpus is freely downloadable and, after
a coverage-guided merge against this harness, gives a high-value boost to both
seed diversity and regression safety:

```sh
curl -L -o /tmp/ossfuzz-vorbis.zip \
  https://storage.googleapis.com/vorbis-backup.clusterfuzz-external.appspot.com/corpus/libFuzzer/vorbis_decode_fuzzer/public.zip
unzip -q /tmp/ossfuzz-vorbis.zip -d /tmp/ossfuzz-vorbis/
./build-libfuzzer/fuzz_ogg_vorbis_decode -merge=1 -max_len=65536 \
  corpus_ogg/ /tmp/ossfuzz-vorbis/
```

`-merge=1` keeps only inputs that add new coverage against *this* harness.
If the merge encounters an input that crashes, libFuzzer writes
`crash-<sha>` to the cwd, restarts, and continues, so the merge is safe to
run on a dirty corpus.

## Run

```sh
./build-libfuzzer/fuzz_ogg_vorbis_decode -dict=vorbis.dict corpus_ogg/
```

Useful flags: `-max_total_time=60`, `-jobs=4`, `-workers=4`,
`-max_len=65536`, `-rss_limit_mb=4096`.

## UBSan suppressions

The build disables `signed-integer-overflow,integer-divide-by-zero`.
Tremor's fixed-point DSP (MDCT, IMDCT, residue decode) relies on modular
int32 arithmetic that's UB per the C standard but deliberate on every real
target (pre-existing upstream behavior). Leaving those checks on would drown
out real memory-safety findings.

The `shift` check stays on: the left-shift-of-negative UB in the fixed-point
math has been fixed in the fork by doing the shift on the unsigned
counterpart and casting back (bit-for-bit identical on two's-complement; see
`src/tremor/CHANGES.md`). ASan and the rest of UBSan stay active.

## Corpus coverage

To see which functions in `src/tremor/` the saved corpus actually exercises:

```sh
./coverage.sh           # per-function report on stdout
./coverage.sh --html    # also write cov-html/ for line-by-line browsing
```

The script builds a separate `build-cov/` with clang source-based coverage
instrumentation, replays `corpus_ogg/` once via libFuzzer's `-runs=0` mode,
and renders the report with `llvm-cov`. Functions at 0% are codepaths the
corpus isn't reaching, candidates for new seeds or dict entries.

## When a crash is found

1. libFuzzer drops `crash-<sha>` in the current directory.
2. Minimize: `./build-libfuzzer/fuzz_ogg_vorbis_decode -minimize_crash=1 -runs=10000 crash-<sha>`.
3. Reproduce under the standalone binary for cleaner stack traces.
4. Keep the reproducer in `crashes/`; once the fix lands and the input no
   longer reproduces, move it to `crashes/fixed_verified/`. Crash inputs are
   local-only (the repo-wide `crash-*` gitignore pattern keeps them out of
   the tree). Replay them after decoder changes for regression cover:

   ```sh
   ./build-libfuzzer/fuzz_ogg_vorbis_decode -runs=0 crashes/
   ```
