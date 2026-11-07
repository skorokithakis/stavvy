#pragma once

#include "esp_err.h"

// Runs in the trigger task when the wake word is detected. Keep it short, and
// do not call trigger_enable() or trigger_disable() from it: both wait for
// that task to act, so calling either from the callback would deadlock.
typedef void (*trigger_callback_t)(void);

// Runs in the button task on the press edge of KEY. KEY is a plain button now:
// it toggles the microphone mute, so it has to stay readable while wake word
// detection is off (muted or during a session) and the microphone belongs to
// someone else. Keep it short; do not start or stop detection from it.
typedef void (*trigger_button_callback_t)(void);

// Loads the wake word model, configures KEY and starts both the detection task
// and the button task. Call once, after audio_init(). Detection stays off until
// trigger_enable(); the button works from here on. The button task is started
// last, after every mutex and the model exist, so an early press cannot reach
// trigger_enable() or trigger_disable() before they are usable.
void trigger_init(trigger_callback_t wake_word_callback, trigger_button_callback_t key_button_callback);

// Asks the trigger task to start microphone capture and wake word detection.
// Returns after the request is queued, not after capture has started. Only
// affects detection; the button task runs regardless.
esp_err_t trigger_enable(void);

// Stops detection and releases the microphone. Blocks until capture has been
// closed, so the caller can open its own capture as soon as this returns. The
// button keeps working. Enable and disable calls are serialised against each
// other, so either may be called from any task.
void trigger_disable(void);
