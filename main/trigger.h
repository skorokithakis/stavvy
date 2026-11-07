#pragma once

#include "esp_err.h"

// What raised the trigger. The session layer starts listening either way, but
// it may want to treat an explicit button press differently from a wake word.
typedef enum {
    TRIGGER_SOURCE_WAKE_WORD,
    TRIGGER_SOURCE_BUTTON,
} trigger_source_t;

// Runs in the trigger task. Keep it short, and do not call trigger_enable() or
// trigger_disable() from it: both wait for that task to act, so calling either
// from the callback would deadlock.
typedef void (*trigger_callback_t)(trigger_source_t source);

// Loads the wake word model and configures the KEY button. Call once, after
// audio_init(). Detection stays off until trigger_enable().
void trigger_init(trigger_callback_t callback);

// Asks the trigger task to start microphone capture and wake word detection.
// Returns after the request is queued, not after capture has started.
esp_err_t trigger_enable(void);

// Stops detection and releases the microphone. Blocks until capture has been
// closed, so the caller can open its own capture as soon as this returns.
// Enable and disable calls are serialised against each other, so either may be
// called from any task.
void trigger_disable(void);
