---
id: amo-mahwx
status: open
deps: [amo-pmgdr]
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Audio: microphone capture and speaker playback

Ready for implementation.

Objective: an audio module that other modules use to (a) read 16 kHz, 16-bit mono microphone frames and (b) play 16-bit PCM at a caller-chosen sample rate.

Scope:
- Use Espressif's esp_codec_dev component for the ES8311 (playback) and ES7210 (microphone ADC), on the shared I2C bus from the board module. I2S pins: MCLK 19, BCLK 20, WS 22, DOUT 23 (to ES8311), DIN 21 (from ES7210). Waveshare's 07_Audio_Test (ESP-IDF) and the XiaoZhi board file show working codec settings.
- The ES7210 delivers several TDM channels; output one microphone channel as mono.
- Playback API: start at a sample rate and channel count, write PCM, stop. Mono or stereo input; the codec output may be mono.
- A temporary self-test behind a Kconfig option (default off): play a short tone, then log microphone level for a few seconds. Leave it in; it is useful for hardware checks.
- Report free heap with audio running.

Non-goals: echo cancellation, volume control UI, resampling.

## Design

Both codecs share one I2S bus (same BCLK/WS/MCLK), so capture and playback cannot run at different sample rates at the same time. The product is half-duplex: switch the bus to the playback rate for playback and back to 16 kHz for capture. Do not add a resampler. The speaker amplifier has no known enable GPIO; it should get power from the AXP2101 ALDO rails. If no sound comes out, report it; do not guess pins.

