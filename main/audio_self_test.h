#pragma once

// Plays a short tone and then logs the microphone level, to check that the
// codecs, the I2S wiring and the power rails all work on a new board.
// Only built when CONFIG_AUDIO_SELF_TEST is enabled: it makes noise at every
// boot, which is not what normal firmware should do.
void audio_self_test_start(void);
