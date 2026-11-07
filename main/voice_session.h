#pragma once

#include <stdbool.h>

// Owns one voice session from trigger to playback. All state changes, trigger
// enable/disable, playback and display updates happen in the session task:
// trigger, button, API and player callbacks only post messages or notification
// bits to it. That is required because trigger_disable(), player_stop() and
// player_play_url() wait for the task that invoked the callback, so calling
// them from a callback would stall or deadlock that task.
//
// Call this before trigger_init() and esphome_api_start(): it creates the
// queue the callbacks post to and registers the API callbacks. It must come
// before trigger_init() because the button task starts polling KEY as soon as
// it exists, and before esphome_api_start() so no connection is missed. The
// trigger is left disabled; voice_session_start() applies the boot state.
void voice_session_init(void);

// Call this after trigger_init() and esphome_api_start() have both returned.
// The session task then applies the boot state itself, enabling wake word
// detection if the microphone is unmuted. Doing it here rather than in main()
// means a KEY press that arrives during the rest of boot is not overridden.
void voice_session_start(void);

// Wake word callback. It only queues the event, so it is safe to call from the
// trigger task.
void voice_session_handle_wake_word(void);

// KEY callback. KEY toggles the microphone mute. The press increments an
// atomic counter and sets a notification bit on the session task rather than
// using the queue, so a full queue cannot lose it and two quick presses are not
// merged into one toggle.
void voice_session_handle_key_button(void);

// The session state and mute flag, for the diagnostics heartbeat. Both are
// written only by the session task and read atomically here.
const char *voice_session_state_name(void);
bool voice_session_is_muted(void);
