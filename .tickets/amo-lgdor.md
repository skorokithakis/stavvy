---
id: amo-lgdor
status: open
deps: [amo-mahwx]
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Reply player: HTTP fetch, MP3/WAV decode, playback

Ready for implementation.

Objective: play_url(url) fetches a URL over HTTP, decodes it and plays it on the speaker, then reports completion through a callback. havpe-server sends URLs such as http://<server>:8085/tts.mp3 (ElevenLabs MP3), tone.wav (16-bit mono 48 kHz) and error.wav (16-bit mono 44.1 kHz).

Scope:
- esp_http_client in streaming mode, no full-file buffering.
- Espressif's esp_audio_codec simple decoder for MP3 and WAV. Choose the container from the Content-Type or the URL extension.
- Start playback at the decoded sample rate and channel count, through the audio module.
- Run in its own task. The decoder needs a stack of about 20 KB and about 28 KB heap for MP3.
- Allow a stop request (for a cancelled session).
- Measure and report free heap and minimum free heap during MP3 playback, with Wi-Fi up.

Non-goals: other formats, seeking, volume control.

## Design

RAM gate: if MP3 decoding does not fit next to Wi-Fi, the wake word and the display buffers, stop and report the numbers. Do not try to work around it. The agreed fallback is a havpe-server change to serve WAV; the architect decides.

