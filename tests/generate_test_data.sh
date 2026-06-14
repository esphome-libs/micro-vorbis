#!/usr/bin/env bash
# Generate the Ogg Vorbis fixtures used by the unit tests in this directory.
#
# The generated files are checked into data/ so the tests run without any
# encoder installed; rerun this script only when the fixture set needs to
# change. Each channel carries a distinct tone so plane-mapping bugs in the
# downmix / channel-selection code are detectable.
#
# Requires: ffmpeg (tone generation) and oggenc from vorbis-tools (libvorbis
# encoding; ffmpeg's native Vorbis encoder is stereo-only).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

for tool in ffmpeg oggenc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool not on PATH" >&2
        exit 1
    fi
done

mkdir -p data
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Per-channel sine tones via aevalsrc. The normalized downmix can no longer
# clip on its own, but amplitudes stay low (<= 0.4) so the tests' unclipped
# linear reconstructions (built from int16 planes) stay far from int16 range.
gen() {
    local out=$1 exprs=$2 layout=$3 rate=$4 dur=$5
    ffmpeg -hide_banner -loglevel error -y -f lavfi \
        -i "aevalsrc=exprs='$exprs':c=$layout:s=$rate:d=$dur" \
        "$tmp/src.wav"
    # Fixed serial number so regeneration is byte-deterministic (oggenc
    # otherwise randomizes the Ogg stream serial on every run)
    oggenc -Q -q 1 --serial 1 -o "data/$out" "$tmp/src.wav"
}

# Mono: single 440 Hz tone.
gen mono_44100.ogg "0.4*sin(2*PI*440*t)" mono 44100 1.5

# Stereo: distinct L/R tones.
gen stereo_44100.ogg "0.4*sin(2*PI*440*t)|0.4*sin(2*PI*880*t)" stereo 44100 2

# Stereo at a different sample rate (reset-reuse test decodes streams with
# differing parameters back to back).
gen stereo_8000.ogg "0.4*sin(2*PI*350*t)|0.4*sin(2*PI*450*t)" stereo 8000 1

# 5.1: six distinct tones (WAV order FL FR FC LFE BL BR; oggenc remaps to the
# Vorbis channel order FL C FR RL RR LFE).
gen surround51_48000.ogg \
    "0.25*sin(2*PI*300*t)|0.25*sin(2*PI*500*t)|0.25*sin(2*PI*700*t)|0.25*sin(2*PI*60*t)|0.25*sin(2*PI*900*t)|0.25*sin(2*PI*1100*t)" \
    5.1 48000 1.5

# Remaining downmix_stereo() channel counts (exprs are in the WAV layout order
# named; oggenc remaps each to the Vorbis order the decoder sees).
# 3.0: FL FR FC
gen surround30_44100.ogg \
    "0.25*sin(2*PI*300*t)|0.25*sin(2*PI*500*t)|0.25*sin(2*PI*700*t)" \
    3.0 44100 1

# quad: FL FR BL BR
gen quad_44100.ogg \
    "0.25*sin(2*PI*300*t)|0.25*sin(2*PI*500*t)|0.25*sin(2*PI*900*t)|0.25*sin(2*PI*1100*t)" \
    quad 44100 1

# 5.0: FL FR FC BL BR
gen surround50_44100.ogg \
    "0.25*sin(2*PI*300*t)|0.25*sin(2*PI*500*t)|0.25*sin(2*PI*700*t)|0.25*sin(2*PI*900*t)|0.25*sin(2*PI*1100*t)" \
    5.0 44100 1

# 6.1: FL FR FC LFE BC SL SR
gen surround61_48000.ogg \
    "0.25*sin(2*PI*300*t)|0.25*sin(2*PI*500*t)|0.25*sin(2*PI*700*t)|0.25*sin(2*PI*60*t)|0.25*sin(2*PI*1300*t)|0.25*sin(2*PI*900*t)|0.25*sin(2*PI*1100*t)" \
    6.1 48000 1

# 7.1: FL FR FC LFE BL BR SL SR
gen surround71_48000.ogg \
    "0.25*sin(2*PI*300*t)|0.25*sin(2*PI*500*t)|0.25*sin(2*PI*700*t)|0.25*sin(2*PI*60*t)|0.25*sin(2*PI*900*t)|0.25*sin(2*PI*1100*t)|0.25*sin(2*PI*1300*t)|0.25*sin(2*PI*1500*t)" \
    7.1 48000 1

echo "[test-data] generated:"
ls -l data/*.ogg
