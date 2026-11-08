#include "voice_session.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "display.h"
#include "esphome_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "player.h"
#include "sdkconfig.h"
#include "trigger.h"

static const char *TAG = "voice_session";

// One task owns the session. The trigger, API and player callbacks only post
// messages here, because trigger_disable() and player_stop() wait for their
// own tasks: calling either from a callback would deadlock or stall that task.
//
// The stack has to fit the audio chunk buffer and the ESP-IDF I2S and LVGL call
// chains underneath it. The queue messages themselves are static, because two
// of them nested on the stack (one in the task loop, one in drain_messages)
// would otherwise take half the stack by themselves.
#define SESSION_TASK_STACK_BYTES 6144
#define SESSION_TASK_PRIORITY 5

// Six slots cover the longest burst the client sends (STT_VAD_END, STT_END,
// TTS_START, TTS_END, RUN_END) plus one voice response or connection change.
// KEY presses and client disconnects deliberately do not use the queue: they
// arrive as task notification bits, so a full queue cannot lose them. Each slot
// carries a full event value because the API's value buffers are only valid
// during the callback, and copying in the callback is the only way to keep the
// text.
#define SESSION_MESSAGE_QUEUE_LENGTH 6

// Callbacks that must never be dropped set these bits on the session task.
// A queue receive cannot be woken by a notification, so the session also waits
// for a queue message with a short timeout and checks the bits on every wake.
#define SESSION_NOTIFICATION_KEY_BUTTON (1u << 0)
#define SESSION_NOTIFICATION_CLIENT_DISCONNECTED (1u << 1)
#define SESSION_NOTIFICATION_STARTUP (1u << 2)

// The KEY press count lives outside the notification bit because a bit is only
// a flag: two quick presses between wakes would otherwise collapse into one
// toggle. The KEY task increments this before notifying, and the session task
// exchanges it back to zero and applies the parity.
static atomic_uint key_press_count;

// 32 ms of 16 kHz 16-bit mono, which is exactly one window of the client's
// voice activity detector, so it can judge end of speech without buffering.
#define SESSION_AUDIO_CHUNK_SAMPLES (AUDIO_CAPTURE_SAMPLE_RATE_HZ * 32 / 1000)
#define SESSION_AUDIO_CHUNK_BYTES ((int)sizeof(int16_t) * SESSION_AUDIO_CHUNK_SAMPLES)

#define SESSION_WAKE_WORD_PHRASE "Stavrobot"

// The tone is the Home Assistant Voice PE's own wake_word_triggered sound, so
// Stavvy announces a session with the chime the user knows from that device.
// main/sounds/regenerate.sh produces it as raw s16le at the ES8311's maximum
// rate.
#define WAKE_TONE_SAMPLE_RATE_HZ 48000
// The session task's stack is 6 KB, so the copy buffer is kept small. A 5 ms
// chunk also keeps the write from holding the audio lock for long.
#define WAKE_TONE_CHUNK_SAMPLES (WAKE_TONE_SAMPLE_RATE_HZ / 200)

// The client answers VoiceAssistantRequest immediately. Five seconds tells a
// slow network apart from a client that has forgotten the request; after that
// the trigger has to come back rather than let the session wait forever.
#define SESSION_RESPONSE_TIMEOUT_MICROSECONDS ((int64_t)5 * 1000 * 1000)

// The client's own capture window is 15 seconds, so this backstop only fires
// when the client never sends any stop event at all.
#define SESSION_STREAMING_TIMEOUT_MICROSECONDS ((int64_t)20 * 1000 * 1000)

// The webhook and the speech synthesis both run after end of speech, and their
// own timeouts are not bounded tightly, so this covers the whole pipeline since
// the last event rather than just the STT wait. Every accepted event restarts
// it, so a short but active pipeline can take longer overall without being
// mistaken for a stuck one.
#define SESSION_THINKING_TIMEOUT_MICROSECONDS ((int64_t)60 * 1000 * 1000)

// A player completion is the only event that ends SPEAKING, and the player's
// HTTP fetch can block for a long time. This backstop stops a playback that
// never reports back, so a lost completion cannot leave the trigger disabled.
#define SESSION_SPEAKING_TIMEOUT_MICROSECONDS ((int64_t)120 * 1000 * 1000)

// A queue receive cannot be woken by a task notification, so this bounds how
// long a KEY press or disconnect can sit unprocessed. It is far below every
// timeout above, and the tick work is only a timer comparison.
#define SESSION_QUEUE_WAIT_MILLISECONDS 100

#if CONFIG_SESSION_SIMULATED_TRIGGER
// Bench convenience only: give the client time to connect and subscribe after
// the connection callback, then raise a button trigger as if KEY was pressed.
#define SESSION_SIMULATED_TRIGGER_DELAY_MICROSECONDS ((int64_t)3 * 1000 * 1000)
#endif

typedef enum {
    SESSION_STATE_IDLE,
    SESSION_STATE_AWAITING_RESPONSE,
    SESSION_STATE_STREAMING,
    SESSION_STATE_THINKING,
    SESSION_STATE_SPEAKING,
} session_state;

typedef enum {
    SESSION_MESSAGE_WAKE_WORD,
    SESSION_MESSAGE_CLIENT_CONNECTED,
    SESSION_MESSAGE_VOICE_RESPONSE,
    SESSION_MESSAGE_VOICE_EVENT,
    SESSION_MESSAGE_PLAYBACK_FINISHED,
} session_message_type;

typedef struct {
    session_message_type type;
    int event_type;
    uint32_t port;
    bool error;
    esp_err_t result;
    char text[ESPHOME_API_EVENT_VALUE_MAX_BYTES + 1];
} session_message;

static QueueHandle_t message_queue;
// Set when the session task is created. The trigger, API and player callbacks
// use it to signal the task without going through the queue.
static TaskHandle_t session_task_handle;
// Both live in BSS rather than on the task stack: every message carries a full
// event value, and nesting two of them would cost 2 KB of the session task's
// stack. Only the session task touches these.
static session_message received_message;
static session_message drained_message;
// The session task is the only writer, but the heartbeat task reads both of
// these, so they are atomic rather than relying on a single word not tearing.
static _Atomic session_state state = SESSION_STATE_IDLE;
// Only meaningful in AWAITING_RESPONSE, STREAMING, THINKING and SPEAKING.
static int64_t deadline_microseconds;
// False until the session task has applied the boot state. KEY polling starts
// during trigger_init(), which runs before the API listener exists, so a press
// in that window must not enable detection on its own; the startup handler does
// it once everything is ready.
static bool session_started;
// Kept until the next session starts so the screen can keep showing why the
// last session failed after the state line has gone back to idle.
static bool session_had_error;
// The requested microphone mute state. KEY flips it; the trigger is only
// enabled while it is false. It starts false on every boot and is not stored.
static _Atomic bool muted;

#if CONFIG_SESSION_SIMULATED_TRIGGER
static int64_t simulated_trigger_at_microseconds;
#endif

static void handle_message(const session_message *message);
static void handle_notifications(void);
static void drain_messages(void);
static void discard_pending_messages(void);
static void return_to_idle(void);

static int64_t now_microseconds(void)
{
    return esp_timer_get_time();
}

// Runs in the API, trigger and player tasks. A full queue drops the message
// instead of blocking: the player completion callback must never wait, because
// the session task can be inside player_stop() waiting for that callback to
// return.
static void post_message(const session_message *message)
{
    if (xQueueSend(message_queue, message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Dropping session message %d because the queue is full", (int)message->type);
    }
}

static const char *find_event_value(const esphome_api_event_data *data, size_t data_count, const char *name)
{
    for (size_t index = 0; index < data_count; index++) {
        if (data[index].name != NULL && strcmp(data[index].name, name) == 0) {
            return data[index].value != NULL ? data[index].value : "";
        }
    }
    return NULL;
}

static void handle_api_connected(void *context)
{
    (void)context;
    const session_message message = {.type = SESSION_MESSAGE_CLIENT_CONNECTED};
    post_message(&message);
}

static void handle_api_disconnected(void *context)
{
    (void)context;
    // A disconnect has to reach the session even when its queue is full, so it
    // is a notification bit rather than a message.
    xTaskNotify(session_task_handle, SESSION_NOTIFICATION_CLIENT_DISCONNECTED, eSetBits);
}

static void handle_api_voice_response(uint32_t port, bool error, void *context)
{
    (void)context;
    session_message message = {.type = SESSION_MESSAGE_VOICE_RESPONSE, .port = port, .error = error};
    post_message(&message);
}

static void handle_api_voice_event(int event_type, const esphome_api_event_data *data, size_t data_count,
                                   void *context)
{
    (void)context;

    // Only post what the session acts on. The client sends RUN_START, STT_START
    // and a pair of INTENT events in every run, and dropping them here keeps
    // them from taking queue slots away from the stop events.
    switch (event_type) {
    case ESPHOME_API_EVENT_STT_VAD_END:
    case ESPHOME_API_EVENT_STT_END:
    case ESPHOME_API_EVENT_TTS_START:
    case ESPHOME_API_EVENT_TTS_END:
    case ESPHOME_API_EVENT_RUN_END:
    case ESPHOME_API_EVENT_ERROR:
        break;
    default:
        ESP_LOGD(TAG, "Dropping unused voice event %d", event_type);
        return;
    }

    session_message message = {.type = SESSION_MESSAGE_VOICE_EVENT, .event_type = event_type};

    // Values point into the API task's static buffers, which are reused by the
    // next message, so the session needs its own copy.
    const char *value = NULL;
    switch (event_type) {
    case ESPHOME_API_EVENT_STT_END:
    case ESPHOME_API_EVENT_TTS_START:
        value = find_event_value(data, data_count, "text");
        break;
    case ESPHOME_API_EVENT_TTS_END:
        value = find_event_value(data, data_count, "url");
        break;
    case ESPHOME_API_EVENT_ERROR:
        value = find_event_value(data, data_count, "message");
        if (value == NULL) {
            value = find_event_value(data, data_count, "code");
        }
        break;
    default:
        break;
    }
    if (value != NULL) {
        snprintf(message.text, sizeof(message.text), "%s", value);
    }
    post_message(&message);
}

static void handle_player_finished(esp_err_t result)
{
    const session_message message = {.type = SESSION_MESSAGE_PLAYBACK_FINISHED, .result = result};
    post_message(&message);
}

// Anything still queued when a session starts belongs to the previous run; a
// stale RUN_END would otherwise end the new session immediately. Only session
// events are stale: connection changes and wake words are put back, and KEY
// presses and disconnects are notifications, so they never pass through here.
static void discard_pending_messages(void)
{
    const UBaseType_t pending = uxQueueMessagesWaiting(message_queue);
    session_message message;
    for (UBaseType_t index = 0; index < pending; index++) {
        if (xQueueReceive(message_queue, &message, 0) != pdTRUE) {
            return;
        }
        switch (message.type) {
        case SESSION_MESSAGE_VOICE_RESPONSE:
        case SESSION_MESSAGE_VOICE_EVENT:
        case SESSION_MESSAGE_PLAYBACK_FINISHED:
            ESP_LOGD(TAG, "Discarding stale session message %d", (int)message.type);
            break;
        default:
            // The snapshot above stops this loop from reading the message it
            // just put back, so it cannot spin forever.
            if (xQueueSend(message_queue, &message, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Dropping message %d because the queue is full", (int)message.type);
            }
            break;
        }
    }
}

// The single place a session ends. It closes the microphone, gives the trigger
// back and leaves the last text on screen. The mute state may have been
// changed while the session ran, so it is applied here rather than when the
// button was pressed. Muted wins over the error line because the microphone
// state is what the idle screen has to show; the error text stays in the
// reply field either way.
static void return_to_idle(void)
{
    audio_capture_stop();
    state = SESSION_STATE_IDLE;
    deadline_microseconds = 0;

    if (muted) {
        display_set_state(DISPLAY_STATE_MUTED);
    } else {
        display_set_state(session_had_error ? DISPLAY_STATE_ERROR : DISPLAY_STATE_IDLE);
        trigger_enable();
    }

    ESP_LOGI(TAG, "Session finished; free heap %u bytes, minimum free heap %u bytes, largest free block: %u bytes",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// Boot is always unmuted, but main() cannot enable detection itself: KEY
// polling starts during trigger_init(), before the API listener exists, and a
// press in that window must not be overridden. The session task applies the
// boot state once trigger_init() and esphome_api_start() have both returned.
static void handle_startup(void)
{
    session_started = true;
    if (muted) {
        display_set_state(DISPLAY_STATE_MUTED);
    } else {
        display_set_state(DISPLAY_STATE_IDLE);
        trigger_enable();
    }
    ESP_LOGI(TAG, "Voice session ready, microphone %s; free heap %u bytes", muted ? "muted" : "unmuted",
             (unsigned)esp_get_free_heap_size());
}

// Embedded with EMBED_FILES. ESP-IDF does not align embedded data, so the blob
// cannot be fed to the codec in place: the samples are copied into an aligned
// buffer as they are written.
extern const uint8_t wake_word_triggered_pcm[];
extern const uint8_t _binary_wake_word_triggered_pcm_end[];

// Blocks the session for the length of the tone, which is shorter than the wait
// for the client's answer in the common case. Anything the client sends while it
// plays waits in the queue and is handled on the next pass.
static void play_wake_word_tone(void)
{
    const size_t total_bytes = (size_t)(_binary_wake_word_triggered_pcm_end - wake_word_triggered_pcm);

    esp_err_t result = audio_playback_start(WAKE_TONE_SAMPLE_RATE_HZ, 1);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the wake word tone (0x%x)", result);
        return;
    }

    int16_t chunk[WAKE_TONE_CHUNK_SAMPLES];
    const size_t chunk_bytes = sizeof(chunk);
    for (size_t offset = 0; offset < total_bytes; offset += chunk_bytes) {
        const size_t remaining = total_bytes - offset;
        const size_t bytes = remaining < chunk_bytes ? remaining : chunk_bytes;
        memcpy(chunk, wake_word_triggered_pcm + offset, bytes);
        result = audio_playback_write(chunk, bytes / sizeof(int16_t));
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Could not play the wake word tone (0x%x)", result);
            break;
        }
    }

    // Closing the codec disables the I2S channel, which discards whatever the
    // last DMA buffers still hold, so the asset ends with a little silence.
    audio_playback_stop();
}

static void start_session(void)
{
    if (state != SESSION_STATE_IDLE) {
        ESP_LOGW(TAG, "Ignoring a trigger while a session is already running");
        return;
    }
    // Detection is off while muted, so the only way here is the bench trigger.
    if (muted) {
        ESP_LOGI(TAG, "Ignoring a trigger while the microphone is muted");
        return;
    }

    discard_pending_messages();

    if (!esphome_api_voice_assistant_subscribed()) {
        display_set_transcript("");
        display_set_reply("");
        display_set_state(DISPLAY_STATE_NO_SERVER);
        ESP_LOGW(TAG, "Trigger with no subscribed API client, staying idle");
        return;
    }

    // Release the microphone before streaming takes it. The wake word task owns
    // the capture device until trigger_disable() returns, and both directions
    // share the one I2S bus.
    trigger_disable();

    session_had_error = false;
    display_set_transcript("");
    display_set_reply("");

    const esp_err_t result = esphome_api_send_voice_assistant_request(true, SESSION_WAKE_WORD_PHRASE);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start a voice session with the API client (0x%x)", result);
        session_had_error = true;
        display_set_reply("Could not reach the server.");
        return_to_idle();
        return;
    }

    // The listening face waits for handle_voice_response(): the microphone only
    // opens once the client answers, and showing the face now would invite the
    // user to speak into a gap where nothing is captured.
    state = SESSION_STATE_AWAITING_RESPONSE;
    deadline_microseconds = now_microseconds() + SESSION_RESPONSE_TIMEOUT_MICROSECONDS;
    ESP_LOGI(TAG, "Session started by the wake word; free heap %u bytes",
             (unsigned)esp_get_free_heap_size());

    // Only once the request is out: a tone for a session that never started
    // would tell the user to speak into nothing. The client's answer arrives
    // while this runs and waits in the queue.
    play_wake_word_tone();
}

static void handle_voice_response(uint32_t port, bool error)
{
    if (state != SESSION_STATE_AWAITING_RESPONSE) {
        ESP_LOGW(TAG, "Ignoring VoiceAssistantResponse while not waiting for one");
        return;
    }

    // port 0 means the client wants audio over the API connection. Anything
    // else is UDP audio, which this firmware does not implement.
    if (error || port != 0) {
        ESP_LOGE(TAG, "The client could not start the session (port %u, error %d)", (unsigned)port, error);
        esphome_api_send_voice_assistant_request(false, NULL);
        session_had_error = true;
        display_set_reply("The server could not start the request.");
        return_to_idle();
        return;
    }

    const esp_err_t result = audio_capture_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open the microphone for streaming (0x%x)", result);
        esphome_api_send_voice_assistant_request(false, NULL);
        session_had_error = true;
        display_set_reply("Could not open the microphone.");
        return_to_idle();
        return;
    }

    state = SESSION_STATE_STREAMING;
    deadline_microseconds = now_microseconds() + SESSION_STREAMING_TIMEOUT_MICROSECONDS;
    display_set_state(DISPLAY_STATE_LISTENING);
    ESP_LOGI(TAG, "Streaming %u bytes every 32 ms; free heap %u bytes",
             (unsigned)SESSION_AUDIO_CHUNK_BYTES, (unsigned)esp_get_free_heap_size());
}

// KEY is a plain button: it toggles the microphone mute and never starts a
// session. While a session runs the change is only recorded, because the
// microphone belongs to the session and the reply should not be cut off; the
// new state is applied by return_to_idle().
static void handle_key_button(void)
{
    muted = !muted;
    ESP_LOGI(TAG, "Microphone %s; free heap %u bytes", muted ? "muted" : "unmuted",
             (unsigned)esp_get_free_heap_size());

    if (state != SESSION_STATE_IDLE) {
        ESP_LOGI(TAG, "The new mute state takes effect when the session ends");
        return;
    }

    if (muted) {
        // Release the microphone before changing the screen, so the red Muted
        // line is only shown once no capture is running.
        trigger_disable();
    }
    display_set_state(muted ? DISPLAY_STATE_MUTED : DISPLAY_STATE_IDLE);
    // Before the startup handler has run, enabling here would let detection
    // start before the API listener exists; the startup handler enables it
    // once everything is ready.
    if (!muted && session_started) {
        trigger_enable();
    }
}

static void handle_voice_event(int event_type, const char *text)
{
    // The client runs its pipeline strictly in order, so every event left over
    // from a cancelled run arrives before the VoiceAssistantResponse to the new
    // request. Ignoring events while waiting for that response (and while
    // idle) throws away the stale run without having to match it up.
    if (state == SESSION_STATE_IDLE || state == SESSION_STATE_AWAITING_RESPONSE) {
        ESP_LOGD(TAG, "Ignoring event %d in state %d", event_type, (int)state);
        return;
    }

    if (state == SESSION_STATE_THINKING) {
        // The webhook and the TTS run after end of speech, so the allowed time
        // is measured from the latest event rather than from STT_VAD_END.
        deadline_microseconds = now_microseconds() + SESSION_THINKING_TIMEOUT_MICROSECONDS;
    }

    switch (event_type) {
    case ESPHOME_API_EVENT_STT_VAD_END:
        if (state == SESSION_STATE_STREAMING) {
            audio_capture_stop();
            state = SESSION_STATE_THINKING;
            deadline_microseconds = now_microseconds() + SESSION_THINKING_TIMEOUT_MICROSECONDS;
            display_set_state(DISPLAY_STATE_THINKING);
            ESP_LOGI(TAG, "End of speech, waiting for the reply");
        }
        break;

    case ESPHOME_API_EVENT_STT_END:
        if (text[0] != '\0') {
            display_set_transcript(text);
            ESP_LOGI(TAG, "Transcript: %s", text);
        }
        break;

    case ESPHOME_API_EVENT_TTS_START:
        // Shortcuts and error sounds carry no text, in which case the previous
        // text (often the transcript or the error) should stay on screen.
        if (text[0] != '\0') {
            display_set_reply(text);
            ESP_LOGI(TAG, "Reply: %s", text);
        }
        break;

    case ESPHOME_API_EVENT_ERROR:
        session_had_error = true;
        audio_capture_stop();
        if (state != SESSION_STATE_SPEAKING) {
            state = SESSION_STATE_THINKING;
            deadline_microseconds = now_microseconds() + SESSION_THINKING_TIMEOUT_MICROSECONDS;
        }
        display_set_state(DISPLAY_STATE_ERROR);
        if (text[0] != '\0') {
            display_set_reply(text);
        }
        ESP_LOGW(TAG, "Voice assistant error: %s", text[0] != '\0' ? text : "no message");
        break;

    case ESPHOME_API_EVENT_TTS_END: {
        audio_capture_stop();
        if (text[0] == '\0') {
            // Some runs, such as a shortcut with nothing to say, end without a
            // URL. RUN_END follows and returns the session to idle.
            ESP_LOGW(TAG, "TTS_END without a URL, nothing to play");
            state = SESSION_STATE_THINKING;
            deadline_microseconds = now_microseconds() + SESSION_THINKING_TIMEOUT_MICROSECONDS;
            display_set_state(DISPLAY_STATE_THINKING);
            break;
        }

        state = SESSION_STATE_SPEAKING;
        deadline_microseconds = now_microseconds() + SESSION_SPEAKING_TIMEOUT_MICROSECONDS;
        display_set_state(DISPLAY_STATE_SPEAKING);
        ESP_LOGI(TAG, "Playing reply %s", text);

        const esp_err_t result = player_play_url(text, handle_player_finished);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Could not queue %s for playback (0x%x)", text, result);
            session_had_error = true;
            display_set_reply("Could not play the reply.");
            return_to_idle();
        }
        break;
    }

    case ESPHOME_API_EVENT_RUN_END:
        ESP_LOGI(TAG, "Run ended");
        audio_capture_stop();
        // The client sends RUN_END right after TTS_END. Playback is still
        // running then, and the player completion returns the session to idle,
        // so ending here would cut the reply off.
        if (state != SESSION_STATE_SPEAKING) {
            return_to_idle();
        }
        break;

    default:
        break;
    }
}

static void handle_client_disconnected(void)
{
    if (state == SESSION_STATE_IDLE) {
        return;
    }
    ESP_LOGW(TAG, "API client disconnected during a session, stopping");
    audio_capture_stop();
    // Safe here because this runs in the session task, not in the player
    // callback that player_stop() would wait for.
    player_stop();
    return_to_idle();
}

static void handle_playback_finished(esp_err_t result)
{
    if (state != SESSION_STATE_SPEAKING) {
        ESP_LOGW(TAG, "Ignoring a playback completion outside a session");
        return;
    }
    if (result != ESP_OK) {
        // An empty MP3 is a success with no audio, which the server sends for
        // some replies, so only a real error is reported here.
        ESP_LOGE(TAG, "Playing the reply failed (0x%x)", result);
        session_had_error = true;
        display_set_reply("Could not play the reply.");
    }
    return_to_idle();
}

static void handle_tick(void)
{
    const int64_t now = now_microseconds();

#if CONFIG_SESSION_SIMULATED_TRIGGER
    if (simulated_trigger_at_microseconds != 0 && now >= simulated_trigger_at_microseconds) {
        simulated_trigger_at_microseconds = 0;
        ESP_LOGI(TAG, "Raising a simulated wake word trigger");
        start_session();
        return;
    }
#endif

    if (state == SESSION_STATE_IDLE || deadline_microseconds == 0 || now < deadline_microseconds) {
        return;
    }

    if (state == SESSION_STATE_SPEAKING) {
        // The player's completion callback is the only thing that ends
        // playback; if it is lost, stopping here keeps the trigger from staying
        // disabled forever. The callback is not invoked for a stopped request.
        ESP_LOGW(TAG, "Playback did not finish within %d seconds, stopping",
                 (int)(SESSION_SPEAKING_TIMEOUT_MICROSECONDS / 1000000));
        session_had_error = true;
        display_set_reply("Playback stopped.");
        player_stop();
        return_to_idle();
    } else if (state == SESSION_STATE_AWAITING_RESPONSE) {
        // A client that is still dialling STT has not read the request yet, so
        // a start=false would be processed after that request's late response.
        // If a new session starts in the meantime, the late response would look
        // like the new one and the start=false's RUN_END would then end it.
        // Dropping the connection is the only way to discard that response:
        // havpe-server resets its pipeline per connection and reconnects.
        ESP_LOGW(TAG, "No VoiceAssistantResponse within %d seconds, disconnecting the client",
                 (int)(SESSION_RESPONSE_TIMEOUT_MICROSECONDS / 1000000));
        esphome_api_disconnect_client();
        session_had_error = true;
        display_set_reply("The server did not answer.");
        return_to_idle();
    } else if (state == SESSION_STATE_THINKING) {
        ESP_LOGW(TAG, "No pipeline event within %d seconds, cancelling",
                 (int)(SESSION_THINKING_TIMEOUT_MICROSECONDS / 1000000));
        esphome_api_send_voice_assistant_request(false, NULL);
        session_had_error = true;
        display_set_reply("The server stopped responding.");
        return_to_idle();
    }
}

// One chunk in, one message out, and then whatever the client has said in the
// meantime. Handling the queue and the notification bits here rather than in a
// second task keeps the session to a single task's stack; the cost is that a
// stop event is handled up to one chunk (32 ms) late, which the client ignores
// anyway because its pipeline is no longer active.
static void stream_audio(void)
{
    int16_t samples[SESSION_AUDIO_CHUNK_SAMPLES];

    while (state == SESSION_STATE_STREAMING) {
        esp_task_wdt_reset();
        const esp_err_t read_result = audio_capture_read(samples, SESSION_AUDIO_CHUNK_SAMPLES);
        if (read_result != ESP_OK) {
            ESP_LOGE(TAG, "Microphone read failed while streaming (0x%x)", read_result);
            session_had_error = true;
            display_set_reply("The microphone stopped working.");
            break;
        }

        const esp_err_t send_result =
            esphome_api_send_audio((const uint8_t *)samples, SESSION_AUDIO_CHUNK_BYTES, false);
        if (send_result != ESP_OK) {
            // A failed write also makes the API task drop the client, which
            // posts a disconnect; ending here avoids streaming into nothing.
            ESP_LOGW(TAG, "Sending audio failed (0x%x), ending the session", send_result);
            break;
        }

        drain_messages();
        handle_notifications();

        if (state == SESSION_STATE_STREAMING && now_microseconds() >= deadline_microseconds) {
            ESP_LOGW(TAG, "Streaming for %d seconds without a stop event, cancelling",
                     (int)(SESSION_STREAMING_TIMEOUT_MICROSECONDS / 1000000));
            esphome_api_send_voice_assistant_request(false, NULL);
            session_had_error = true;
            display_set_reply("The server stopped responding.");
            break;
        }
    }

    // Only the break paths above leave the loop with capture still open. Every
    // handler that moved the session out of STREAMING released the microphone
    // before it could re-enable the trigger, so stopping here again would close
    // the wake word task's capture rather than the session's.
    if (state == SESSION_STATE_STREAMING) {
        audio_capture_stop();
        return_to_idle();
    }
}

static void handle_message(const session_message *message)
{
    switch (message->type) {
    case SESSION_MESSAGE_WAKE_WORD:
        start_session();
        break;

    case SESSION_MESSAGE_CLIENT_CONNECTED:
        ESP_LOGI(TAG, "API client connected");
#if CONFIG_SESSION_SIMULATED_TRIGGER
        // The client subscribes right after connecting, so a short delay is
        // enough for esphome_api_voice_assistant_subscribed() to say yes.
        simulated_trigger_at_microseconds = now_microseconds() + SESSION_SIMULATED_TRIGGER_DELAY_MICROSECONDS;
#endif
        break;

    case SESSION_MESSAGE_VOICE_RESPONSE:
        handle_voice_response(message->port, message->error);
        break;

    case SESSION_MESSAGE_VOICE_EVENT:
        handle_voice_event(message->event_type, message->text);
        break;

    case SESSION_MESSAGE_PLAYBACK_FINISHED:
        handle_playback_finished(message->result);
        break;
    }
}

static void drain_messages(void)
{
    while (xQueueReceive(message_queue, &drained_message, 0) == pdTRUE) {
        handle_message(&drained_message);
    }
}

// Takes every pending notification bit in one go. The bits themselves are the
// delivery mechanism, so a press and a disconnect that happen close together
// are both seen rather than coalescing into a single "something happened".
static void handle_notifications(void)
{
    uint32_t notifications = 0;
    xTaskNotifyWait(0, UINT32_MAX, &notifications, 0);
    if (notifications == 0) {
        return;
    }

    if ((notifications & SESSION_NOTIFICATION_STARTUP) != 0) {
        handle_startup();
    }
    // A pending disconnect is handled before a KEY press so a session that no
    // longer has a client is torn down first.
    if ((notifications & SESSION_NOTIFICATION_CLIENT_DISCONNECTED) != 0) {
        handle_client_disconnected();
    }
    if ((notifications & SESSION_NOTIFICATION_KEY_BUTTON) != 0) {
        // The bit only says that at least one press arrived. The counter says
        // how many, so presses that landed between wakes are not merged into a
        // single toggle; an even number leaves the mute state where it was.
        const unsigned presses = atomic_exchange(&key_press_count, 0);
        if ((presses % 2) != 0) {
            handle_key_button();
        }
    }
}

static void session_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();

        if (state == SESSION_STATE_STREAMING) {
            stream_audio();
            continue;
        }

        // A queue receive cannot be woken by a notification, so the short
        // timeout here is what bounds notification latency. handle_tick() only
        // compares deadlines, so running it this frequently costs nothing.
        handle_notifications();
        if (xQueueReceive(message_queue, &received_message,
                          pdMS_TO_TICKS(SESSION_QUEUE_WAIT_MILLISECONDS)) == pdTRUE) {
            handle_message(&received_message);
            // Events arrive in bursts, so settle the whole burst in one pass
            // instead of waking up once per message.
            drain_messages();
        } else {
            handle_tick();
        }
    }
}

void voice_session_init(void)
{
    message_queue = xQueueCreate(SESSION_MESSAGE_QUEUE_LENGTH, sizeof(session_message));
    ESP_ERROR_CHECK(message_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    const esphome_api_callbacks callbacks = {
        .connected = handle_api_connected,
        .disconnected = handle_api_disconnected,
        .voice_response = handle_api_voice_response,
        .voice_event = handle_api_voice_event,
        .context = NULL,
    };
    // Must happen before esphome_api_start(), which may deliver a connection
    // immediately.
    esphome_api_set_callbacks(&callbacks);

    const BaseType_t task_result = xTaskCreate(session_task, "voice_session", SESSION_TASK_STACK_BYTES, NULL,
                                               SESSION_TASK_PRIORITY, &session_task_handle);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Voice session task created; free heap %u bytes", (unsigned)esp_get_free_heap_size());
}

void voice_session_start(void)
{
    xTaskNotify(session_task_handle, SESSION_NOTIFICATION_STARTUP, eSetBits);
}

void voice_session_handle_wake_word(void)
{
    const session_message message = {.type = SESSION_MESSAGE_WAKE_WORD};
    post_message(&message);
}

void voice_session_handle_key_button(void)
{
    // KEY has to toggle mute even while the queue is full or a session owns the
    // microphone, so it is a notification rather than a queued message. The
    // counter is incremented first: the notification is only a wake-up, and
    // the session must be able to see every press that preceded it.
    atomic_fetch_add(&key_press_count, 1);
    xTaskNotify(session_task_handle, SESSION_NOTIFICATION_KEY_BUTTON, eSetBits);
}

const char *voice_session_state_name(void)
{
    static const char *const names[] = {
        [SESSION_STATE_IDLE] = "idle",
        [SESSION_STATE_AWAITING_RESPONSE] = "awaiting response",
        [SESSION_STATE_STREAMING] = "streaming",
        [SESSION_STATE_THINKING] = "thinking",
        [SESSION_STATE_SPEAKING] = "speaking",
    };
    return names[atomic_load(&state)];
}

bool voice_session_is_muted(void)
{
    return atomic_load(&muted);
}
