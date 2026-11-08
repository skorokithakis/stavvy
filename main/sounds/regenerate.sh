#!/bin/sh
# Regenerates wake_word_triggered.pcm from the Home Assistant Voice PE sound.
#
# The source is the FLAC that the Voice PE plays when its wake word fires, so
# Stavvy uses the chime the user knows from that device. Only the two opening
# notes and their decay are kept: everything after 450 ms is the long tail that
# makes the clip feel slow when it overlaps the wait for the server's answer.
# The fade-out removes the click a hard cut would leave, and the 30 ms of silence
# covers the last DMA buffer that audio_playback_stop() can cut off.
set -eu

cd "$(dirname "$0")"

COMMIT="d4e6fa43d6a1a7d342b7d4d403567e06516719de"
SOURCE_URL="https://raw.githubusercontent.com/esphome/home-assistant-voice-pe/${COMMIT}/sounds/wake_word_triggered.flac"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

curl -sSL -o "$WORK_DIR/wake_word_triggered.flac" "$SOURCE_URL"

# Raw s16le at the source's own 48 kHz mono, which is what the ES8311 plays and
# what voice_session.c asks audio_playback_start() for. The source is already
# 16-bit, so nothing is requantised here; -ar and -ac only pin the format in case
# the upstream file changes.
ffmpeg -hide_banner -loglevel error -y \
    -i "$WORK_DIR/wake_word_triggered.flac" \
    -af "atrim=end=0.45,afade=t=out:st=0.40:d=0.05,apad=pad_dur=0.03" \
    -ar 48000 -ac 1 -f s16le -acodec pcm_s16le \
    wake_word_triggered.pcm
