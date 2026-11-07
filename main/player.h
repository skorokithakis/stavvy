#pragma once

#include "esp_err.h"

// Plays one URL at a time: fetches it over HTTP, decodes MP3 or WAV as it
// arrives, and writes the PCM to the speaker. The session layer enables and
// disables the microphone around playback; the player only touches the audio
// playback API.
typedef void (*player_completion_callback_t)(esp_err_t result);

// Registers the MP3/WAV decoders and starts the player task. Call once, after
// audio_init() and before the network is expected to deliver a URL.
void player_init(void);

// Queues a URL to play. The URL is copied, so the caller's string can be freed
// as soon as this returns. Returns ESP_ERR_INVALID_STATE if a previous request
// is still playing or finishing, and ESP_ERR_INVALID_ARG if the URL is empty or
// too long.
//
// The callback runs in the player task once playback ends. It is not called
// when player_stop() cancels the request, because the caller already knows it
// stopped. While the callback runs, the player still counts as busy, so a
// player_play_url() made from it is rejected; do not call player_stop() from
// it either, as that waits for the callback itself to return.
esp_err_t player_play_url(const char *url, player_completion_callback_t callback);

// Stops the current request and waits until the player task has closed the
// HTTP connection and the speaker. Returns ESP_OK when nothing is playing.
// The completion callback of a stopped request is not invoked.
//
// The HTTP client cannot interrupt a read that is already in flight, so this
// can wait for up to the client's network timeout (2 seconds).
esp_err_t player_stop(void);
