#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The detector consumes 16 kHz mono audio. One feature step is 10 ms (see
// main/wake_word/stavrobot.json), so a caller that hands over exactly one step
// at a time gets one spectrogram slice per call and keeps the inference cadence
// at the model's own stride. Reading larger chunks would make several features
// and therefore several inferences happen back to back inside one call, which
// would hide the per-inference timing this module has to report.
#define WAKE_WORD_FRAME_SAMPLES 160

// Loads the model, allocates the two tensor arenas and sets up the feature
// frontend. Aborts the boot on any failure: without a working model there is no
// session trigger, so there is nothing sensible to fall back to.
void wake_word_init(void);

// Discards everything learned from earlier audio: the frontend's window, the
// sliding window of probabilities and the cooling-off counter. Detection starts
// with a clean slate, so the last words before a session ended cannot make the
// next session fire immediately.
void wake_word_reset(void);

// Consumes one frame of WAKE_WORD_FRAME_SAMPLES samples and returns true when
// the wake word is detected. Must only be called from one task at a time, and
// only after wake_word_init().
bool wake_word_process(const int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif
