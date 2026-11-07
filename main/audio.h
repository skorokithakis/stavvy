#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Capture is fixed at 16 kHz mono because the ES7210 converts the microphones
// to exactly that and every consumer downstream (wake word, voice streaming)
// expects it. Playback is variable, because the reply player must play whatever
// sample rate the source file uses.
#define AUDIO_CAPTURE_SAMPLE_RATE_HZ 16000

// Brings up the shared I2S bus and both codecs. Must run after board_i2c_init()
// and board_power_init(): the codecs are on the shared I2C bus and stay
// unpowered until the PMIC enables the peripheral rails.
void audio_init(void);

// The ES8311 and the ES7210 share one I2S bus, so capture and playback cannot
// run at the same time: starting one direction stops the other. Switching
// re-clocks the bus, which is why the two directions cannot be interleaved.
// All functions take a module lock, so callers in different tasks do not need
// to coordinate with one another; a blocking read or write holds that lock
// until its transfer completes (at most one second on a stalled bus).
esp_err_t audio_capture_start(void);
esp_err_t audio_capture_read(int16_t *samples, size_t sample_count);
void audio_capture_stop(void);

// channel_count is 1 for mono or 2 for interleaved stereo. Mono is what the
// single speaker needs; stereo is accepted for sources that are already
// interleaved. Sample rate support comes from the ES8311 (8 kHz to 48 kHz).
esp_err_t audio_playback_start(uint32_t sample_rate_hz, uint8_t channel_count);
esp_err_t audio_playback_write(const int16_t *samples, size_t sample_count);
void audio_playback_stop(void);
