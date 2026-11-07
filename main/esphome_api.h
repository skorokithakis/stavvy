#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// The largest audio chunk that esphome_api_send_audio() accepts. havpe-server
// consumes 1024-byte PCM frames, and the extra room keeps a larger read from
// the capture path from being silently split.
#define ESPHOME_API_MAX_AUDIO_CHUNK_BYTES 2048

// Event data values larger than this are truncated at a UTF-8 character
// boundary. The LLM reply and the transcript can be much longer than this, and
// the session layer only needs a usable prefix for logging and state tracking.
#define ESPHOME_API_EVENT_VALUE_MAX_BYTES 1024

// Values match VoiceAssistantEvent in esphome_api.proto (and therefore in
// ESPHome's api.proto). They are repeated here so that callers can switch on an
// event without including the generated nanopb header.
typedef enum {
    ESPHOME_API_EVENT_ERROR = 0,
    ESPHOME_API_EVENT_RUN_START = 1,
    ESPHOME_API_EVENT_RUN_END = 2,
    ESPHOME_API_EVENT_STT_START = 3,
    ESPHOME_API_EVENT_STT_END = 4,
    ESPHOME_API_EVENT_INTENT_START = 5,
    ESPHOME_API_EVENT_INTENT_END = 6,
    ESPHOME_API_EVENT_TTS_START = 7,
    ESPHOME_API_EVENT_TTS_END = 8,
    ESPHOME_API_EVENT_WAKE_WORD_START = 9,
    ESPHOME_API_EVENT_WAKE_WORD_END = 10,
    ESPHOME_API_EVENT_STT_VAD_START = 11,
    ESPHOME_API_EVENT_STT_VAD_END = 12,
} esphome_api_event;

// One name/value pair from a VoiceAssistantEventResponse. The strings point
// into static buffers and are only valid for the duration of the callback.
// Values longer than ESPHOME_API_EVENT_VALUE_MAX_BYTES are truncated on a UTF-8
// character boundary.
typedef struct {
    const char *name;
    const char *value;
} esphome_api_event_data;

// All callbacks run in the API task, never in the task that calls
// esphome_api_send_*().
typedef void (*esphome_api_connection_callback)(void *context);
typedef void (*esphome_api_voice_response_callback)(uint32_t port, bool error, void *context);
typedef void (*esphome_api_voice_event_callback)(int event_type, const esphome_api_event_data *data,
                                                 size_t data_count, void *context);

typedef struct {
    esphome_api_connection_callback connected;
    esphome_api_connection_callback disconnected;
    esphome_api_voice_response_callback voice_response;
    esphome_api_voice_event_callback voice_event;
    void *context;
} esphome_api_callbacks;

// Copies the callbacks into the module. Call this before esphome_api_start():
// a client can connect as soon as the listener exists, and the callbacks are
// read without a lock because the API task is the only reader.
void esphome_api_set_callbacks(const esphome_api_callbacks *callbacks);

// Creates the listener on TCP port 6053 and starts the API task. The network
// stack must already be initialised, and mDNS already advertises the port, so
// this can run at boot. Call it before any other function in this module: it
// creates the mutex that they use.
void esphome_api_start(void);

// Whether a client currently holds the connection. Only one client is accepted
// at a time; a new connection replaces the old one.
bool esphome_api_client_connected(void);

// True when the connected client subscribed to voice assistant events and
// asked for audio over the API connection. A subscription without API audio
// would mean UDP audio, which this firmware does not implement, so that case
// reports false.
bool esphome_api_voice_assistant_subscribed(void);

// Sends VoiceAssistantRequest (type 90): start=true begins a voice session and
// start=false cancels it. wake_word_phrase may be NULL. Returns
// ESP_ERR_INVALID_STATE when no client is connected.
esp_err_t esphome_api_send_voice_assistant_request(bool start, const char *wake_word_phrase);

// Sends one VoiceAssistantAudio (type 106) chunk. end marks the last chunk of
// the stream. Safe to call from any task; frames are serialised by a mutex, so
// a capture task and the session task cannot interleave bytes.
esp_err_t esphome_api_send_audio(const uint8_t *data, size_t data_size, bool end);
