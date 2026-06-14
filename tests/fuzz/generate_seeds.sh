#!/usr/bin/env bash
# Generate a seed corpus for fuzz_ogg_vorbis_decode.
#
# Outputs:
#   seeds_ogg/   Ogg Vorbis files covering channel counts, sample rates,
#                bitrate modes, and content shapes.
#
# Requires: ffmpeg on PATH (with libvorbis enabled).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg not on PATH" >&2
    exit 1
fi

# Prefer libvorbis (higher quality, non-experimental). Fall back to ffmpeg's
# native Vorbis encoder, which needs `-strict experimental` but is still
# sufficient for fuzzer seed material.
vorbis_codec="libvorbis"
strict_flag=()
if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "libvorbis"; then
    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -qE "^ A..X.D vorbis"; then
        echo "[seeds] libvorbis not available, using ffmpeg's native vorbis encoder"
        vorbis_codec="vorbis"
        strict_flag=(-strict experimental)
    else
        echo "error: ffmpeg has no Vorbis encoder available" >&2
        exit 1
    fi
fi

rm -rf seeds_ogg
mkdir -p seeds_ogg

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Generate a PCM WAV we can transcode to Vorbis in many variants.
gen_source() {
    local out=$1 rate=$2 chans=$3 dur=$4 kind=$5
    local filter
    case "$kind" in
        tone)    filter="sine=frequency=440:sample_rate=$rate:duration=$dur" ;;
        silence) filter="anullsrc=r=$rate:cl=mono:duration=$dur" ;;
        noise)   filter="anoisesrc=r=$rate:d=$dur:amplitude=0.5" ;;
        dc)      filter="aevalsrc=exprs=0.5:s=$rate:d=$dur" ;;
        impulse) filter="aevalsrc=exprs='if(eq(n,100),0.99,0)':s=$rate:d=$dur" ;;
        sweep)   filter="sine=frequency=100:beep_factor=4:sample_rate=$rate:duration=$dur" ;;
        *) echo "unknown kind $kind" >&2; return 1 ;;
    esac
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$filter" \
        -ac "$chans" -ar "$rate" -t "$dur" "$out"
}

# Encode a WAV to Ogg Vorbis with explicit codec params.
# $1 src.wav  $2 out.ogg  $3 quality (-q:a, -1..10)  $4.. extra ffmpeg args
encode_vorbis() {
    local src=$1 out=$2 quality=$3; shift 3
    ffmpeg -hide_banner -loglevel error -y -i "$src" "${strict_flag[@]}" -c:a "$vorbis_codec" \
        -q:a "$quality" "$@" "$out"
}

echo "[seeds] generating Ogg Vorbis variants..."

# Baseline: stereo 44.1 kHz q5.
gen_source "$tmp/tone_st_441.wav" 44100 2 2 tone
encode_vorbis "$tmp/tone_st_441.wav" seeds_ogg/vorbis_stereo_44100_q5.ogg 5

# Mono / multichannel: only libvorbis supports !=2 channels.
if [[ "$vorbis_codec" == "libvorbis" ]]; then
    gen_source "$tmp/tone_mono_441.wav" 44100 1 2 tone
    encode_vorbis "$tmp/tone_mono_441.wav" seeds_ogg/vorbis_mono_44100_q3.ogg 3
else
    echo "[seeds] skipping mono/multichannel seeds (ffmpeg native vorbis is stereo-only)"
fi

# Other sample rates.
gen_source "$tmp/tone_st_480.wav" 48000 2 2 tone
encode_vorbis "$tmp/tone_st_480.wav" seeds_ogg/vorbis_stereo_48000_q5.ogg 5

gen_source "$tmp/tone_st_960.wav" 96000 2 1 tone
encode_vorbis "$tmp/tone_st_960.wav" seeds_ogg/vorbis_stereo_96000_q6.ogg 6

gen_source "$tmp/tone_st_220.wav" 22050 2 2 tone
encode_vorbis "$tmp/tone_st_220.wav" seeds_ogg/vorbis_stereo_22050_q2.ogg 2

gen_source "$tmp/tone_st_80.wav" 8000 2 2 tone
encode_vorbis "$tmp/tone_st_80.wav" seeds_ogg/vorbis_stereo_8000_q0.ogg 0

# Quality-range sweep (affects blocksize, codebook selection).
encode_vorbis "$tmp/tone_st_441.wav" seeds_ogg/vorbis_stereo_44100_qm1.ogg -1
encode_vorbis "$tmp/tone_st_441.wav" seeds_ogg/vorbis_stereo_44100_q0.ogg   0
encode_vorbis "$tmp/tone_st_441.wav" seeds_ogg/vorbis_stereo_44100_q10.ogg 10

# CBR / ABR modes (different codebook setup paths).
ffmpeg -hide_banner -loglevel error -y -i "$tmp/tone_st_441.wav" "${strict_flag[@]}" -c:a "$vorbis_codec" \
    -b:a 64k seeds_ogg/vorbis_stereo_44100_cbr64k.ogg
ffmpeg -hide_banner -loglevel error -y -i "$tmp/tone_st_441.wav" "${strict_flag[@]}" -c:a "$vorbis_codec" \
    -b:a 256k seeds_ogg/vorbis_stereo_44100_cbr256k.ogg

# Multichannel (exercises coupling / mapping). Only libvorbis supports this.
if [[ "$vorbis_codec" == "libvorbis" ]]; then
    gen_source "$tmp/tone_6ch.wav" 48000 6 2 tone
    encode_vorbis "$tmp/tone_6ch.wav" seeds_ogg/vorbis_6ch_48000_q4.ogg 4

    gen_source "$tmp/tone_8ch.wav" 48000 8 2 tone
    encode_vorbis "$tmp/tone_8ch.wav" seeds_ogg/vorbis_8ch_48000_q4.ogg 4
fi

# Content-shape seeds.
gen_source "$tmp/silence.wav" 44100 2 5 silence
encode_vorbis "$tmp/silence.wav" seeds_ogg/vorbis_silence_5s.ogg 5

gen_source "$tmp/dc.wav" 44100 2 2 dc
encode_vorbis "$tmp/dc.wav" seeds_ogg/vorbis_dc_offset.ogg 5

gen_source "$tmp/noise.wav" 44100 2 2 noise
encode_vorbis "$tmp/noise.wav" seeds_ogg/vorbis_white_noise.ogg 5

gen_source "$tmp/impulse.wav" 44100 2 1 impulse
encode_vorbis "$tmp/impulse.wav" seeds_ogg/vorbis_impulse.ogg 5

# Duration edge cases.
ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i "sine=frequency=440:sample_rate=44100:duration=0.1" \
    -ac 2 -ar 44100 "${strict_flag[@]}" -c:a "$vorbis_codec" -q:a 5 \
    seeds_ogg/vorbis_very_short.ogg

ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i "sine=frequency=440:sample_rate=44100" \
    -ac 2 -ar 44100 -t 0.005 "${strict_flag[@]}" -c:a "$vorbis_codec" -q:a 5 \
    seeds_ogg/vorbis_tiny.ogg

# With metadata (non-trivial comment header).
ffmpeg -hide_banner -loglevel error -y -i "$tmp/tone_st_441.wav" "${strict_flag[@]}" -c:a "$vorbis_codec" -q:a 5 \
    -metadata title="Fuzz Seed" -metadata artist="microVorbis" \
    -metadata album="microVorbis" -metadata comment="seed corpus entry" \
    seeds_ogg/vorbis_with_metadata.ogg

# ---------------------------------------------------------------------------
# Fuzzer config tails.
#
# fuzz_ogg_vorbis_decode reads its decoder configuration from the BACK of each
# input (FuzzedDataProvider): one cfg byte (constructor variant + CRC), then for
# role-select a count + per-channel roles, then up to 64 chunk-control bytes.
# A bare .ogg therefore loses ~74 bytes off its last page to those reads (a tiny
# seed loses most of itself). Appending a config tail keeps the ENTIRE .ogg
# intact as decoder payload while still giving libFuzzer a mutable region for the
# feature flags. A few variants pre-set cfg so the downmix / role-select / CRC
# paths are seeded directly rather than found by mutation.
#
# cfg layout (matches the harness): bit0 = enable CRC, bits1-2 = mode
#   (0 passthrough/follow-file, 1 mono downmix, 2 stereo downmix, 3 role-select),
#   bit3 = replay the stream a second time across a reset().
# ---------------------------------------------------------------------------

# Emit one raw byte from a decimal value. Octal escape keeps this portable to
# macOS's stock bash 3.2 (whose printf lacks \xHH).
emit_byte() { printf "\\$(printf '%03o' "$1")"; }

# Append a config tail to a file.
#   $1 file  $2 cfg  [$3 sel_count  $4.. role bytes]
# Tail layout, by consumption order off the back: cfg, then (role-select only)
# sel_count + roles, then 64 neutral chunk-control bytes. 64 control bytes >=
# the harness MAX_CONTROL_BYTES, so every control byte comes from the pad and
# none is peeled off the real stream.
append_config_tail() {
    local f=$1 cfg=$2; shift 2
    {
        local i
        for ((i=0;i<64;i++)); do emit_byte 32; done  # ~1 KiB chunks (1 + 32*32)
        if [ $# -gt 0 ]; then
            local sel=$1 r; shift
            for r in "$@"; do emit_byte "$r"; done
            emit_byte "$sel"
        fi
        emit_byte "$cfg"
    } >> "$f"
}

# Copy a pristine base seed and give the copy a specific config tail.
#   $1 base.ogg  $2 dstname  $3 cfg  [$4 sel_count  $5.. roles]
mkvariant() {
    local src=$1 dst=$2; shift 2
    [[ -f "$src" ]] || return 0
    cp "$src" "seeds_ogg/$dst"
    append_config_tail "seeds_ogg/$dst" "$@"
}

echo "[seeds] appending fuzzer config tails"

# Snapshot the pristine bases before adding variants, so variants are not
# double-tailed by the pass below.
base_list="$tmp/base_seeds.txt"
find seeds_ogg -maxdepth 1 -type f -name '*.ogg' | sort > "$base_list"

# Feature-flag variants. The stereo base is always present; the 6ch base only
# exists with libvorbis (mkvariant no-ops if its source is missing).
st=seeds_ogg/vorbis_stereo_44100_q5.ogg
mkvariant "$st" cfg_crc_on.ogg                          1        # passthrough + CRC
mkvariant "$st" cfg_downmix_mono.ogg                    2        # mono downmix
mkvariant "$st" cfg_downmix_stereo.ogg                  4        # stereo downmix
mkvariant "$st" cfg_roleselect.ogg                      6 2 0 1  # select 2 channels (FL,FR)
mkvariant seeds_ogg/vorbis_6ch_48000_q4.ogg cfg_roleselect_6ch.ogg 6 2 0 1
mkvariant "$st" cfg_replay.ogg                          8        # passthrough + reset-replay

# Every pristine base: neutral passthrough tail (cfg=0) so the full .ogg
# survives as payload while libFuzzer still gets a mutable flag region.
while IFS= read -r f; do
    append_config_tail "$f" 0
done < "$base_list"

echo "[seeds] $(ls seeds_ogg | wc -l | tr -d ' ') Ogg Vorbis seeds generated"
echo "[seeds] done"
