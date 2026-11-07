#include "player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "player";

// The MP3 decoder uses about 28 KB of heap and the decoder library asks for a
// 20 KB stack. This is the one place in the firmware where that much memory is
// needed, and the stack only exists while the task does.
#define PLAYER_TASK_STACK_SIZE (20 * 1024)
#define PLAYER_TASK_PRIORITY 5

// The player task otherwise blocks on its request semaphore indefinitely
// between replies. A timed wait lets it reset the task watchdog while idle,
// and costs one wake-up per second.
#define PLAYER_IDLE_WAIT_MS 1000

// The URL is copied into the player because the caller's string may be a
// temporary buffer on another task's stack. The havpe-server URLs are well
// under a hundred characters; the rest is room for a server address change.
#define PLAYER_URL_MAX_LENGTH 256

// The reply server is on the local network, so a read that stalls for this long
// is a failed playback rather than something worth waiting out. It also bounds
// how long player_stop() can block, because an HTTP read already in progress
// cannot be interrupted from another task.
#define PLAYER_HTTP_TIMEOUT_MS 2000

// The HTTP client and the decoder both accept arbitrary chunk sizes. One
// kilobyte keeps the number of network reads and decoder calls low without
// spending much heap.
#define PLAYER_READ_SIZE 1024

// An MP3 frame is at most 1152 samples per channel, so 4096 bytes is enough for
// mono output and most stereo frames. The decoder reports a larger requirement
// instead of truncating, and the buffer grows once if needed.
#define PLAYER_DECODE_BUFFER_INITIAL_SIZE 4096

static SemaphoreHandle_t state_mutex;
// Signals the player task that a request is waiting.
static SemaphoreHandle_t request_semaphore;
// Signals a player_stop() caller that the task has finished with its request.
static SemaphoreHandle_t idle_semaphore;
// Serialises player_stop() calls. Without it, two tasks waiting for the same
// request would both block on idle_semaphore, which only one of them could
// take, and the other would wait forever.
static SemaphoreHandle_t stop_mutex;

// busy covers both "queued but not started" and "running". Keeping it set
// until the completion callback returns means player_stop() cannot return while
// the caller's callback is still doing session bookkeeping.
static bool busy;
static bool stop_requested;
static char requested_url[PLAYER_URL_MAX_LENGTH];
static player_completion_callback_t requested_callback;

static void lock_state(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
}

static void unlock_state(void)
{
    xSemaphoreGive(state_mutex);
}

static bool playback_stop_requested(void)
{
    lock_state();
    const bool requested = stop_requested;
    unlock_state();
    return requested;
}

static esp_audio_simple_dec_type_t decoder_type_from_url(const char *url)
{
    // The query string and fragment are not part of the file name. This lets
    // the same extension check work with URLs such as /tts.mp3?cache=123.
    const char *url_end = url + strcspn(url, "?#");
    const char *extension = NULL;
    for (const char *cursor = url; cursor < url_end; cursor++) {
        if (*cursor == '.') {
            extension = cursor + 1;
        }
    }
    if (extension == NULL) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
    const size_t extension_length = (size_t)(url_end - extension);
    if (extension_length == 3 && strncasecmp(extension, "mp3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (extension_length == 3 && strncasecmp(extension, "wav", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static esp_err_t open_stream(const char *url, esp_http_client_handle_t *client)
{
    const esp_http_client_config_t configuration = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = PLAYER_HTTP_TIMEOUT_MS,
        // Reads go straight into the decoder input buffer, so the HTTP client's
        // receive buffer only needs to hold the part it over-reads.
        .buffer_size = PLAYER_READ_SIZE,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t handle = esp_http_client_init(&configuration);
    if (handle == NULL) {
        ESP_LOGE(TAG, "Could not create an HTTP client for %s", url);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_open(handle, 0);
    if (result == ESP_OK) {
        // A negative return is an error; zero is a valid answer for a chunked
        // response or one without Content-Length.
        const int64_t content_length = esp_http_client_fetch_headers(handle);
        const int status_code = esp_http_client_get_status_code(handle);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Could not read the response headers from %s", url);
            result = ESP_FAIL;
        } else if (status_code != HttpStatus_Ok) {
            ESP_LOGE(TAG, "Server returned HTTP %d for %s", status_code, url);
            result = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGE(TAG, "Could not open %s (0x%x)", url, result);
    }

    if (result != ESP_OK) {
        esp_http_client_cleanup(handle);
        return result;
    }
    *client = handle;
    return ESP_OK;
}

static esp_err_t start_audio_for_frame(esp_audio_simple_dec_handle_t decoder)
{
    esp_audio_simple_dec_info_t information = {0};
    const esp_audio_err_t information_result = esp_audio_simple_dec_get_info(decoder, &information);
    if (information_result != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Could not read the decoded audio format (%d)", information_result);
        return ESP_FAIL;
    }
    // The audio module and the ES8311 are set up for 16-bit PCM only. A WAV
    // file with a different sample width is a source problem, not something to
    // convert here, because the board already struggles for RAM.
    if (information.bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported sample width %d bits, only 16-bit is supported", information.bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (information.channel != 1 && information.channel != 2) {
        ESP_LOGE(TAG, "Unsupported channel count %d", information.channel);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t result = audio_playback_start(information.sample_rate, information.channel);
    if (result != ESP_OK) {
        return result;
    }
    ESP_LOGI(TAG, "Decoded %lu Hz, %d channel(s), 16-bit; free heap %u bytes, minimum free heap %u bytes",
             (unsigned long)information.sample_rate, information.channel, (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    return ESP_OK;
}

static esp_err_t decode_and_play(esp_audio_simple_dec_handle_t decoder, esp_http_client_handle_t client)
{
    uint8_t *input = malloc(PLAYER_READ_SIZE);
    uint32_t output_size = PLAYER_DECODE_BUFFER_INITIAL_SIZE;
    uint8_t *output = malloc(output_size);
    if (input == NULL || output == NULL) {
        ESP_LOGE(TAG, "Could not allocate the decode buffers");
        free(input);
        free(output);
        return ESP_ERR_NO_MEM;
    }

    bool audio_started = false;
    esp_err_t result = ESP_OK;
    bool end_of_stream = false;

    while (!end_of_stream) {
        // A long reply keeps this loop running for its whole duration, so the
        // watchdog has to be fed here rather than only between requests.
        esp_task_wdt_reset();
        if (playback_stop_requested()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        const int read_size = esp_http_client_read(client, (char *)input, PLAYER_READ_SIZE);
        if (read_size < 0) {
            ESP_LOGE(TAG, "Could not read from the stream (0x%x)", read_size);
            result = ESP_FAIL;
            break;
        }
        if (read_size == 0) {
            end_of_stream = true;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input,
            .len = (uint32_t)read_size,
            // The decoder uses eos to flush data it has cached while waiting
            // for a complete frame; without it, the last frames are lost.
            .eos = end_of_stream,
        };

        do {
            if (playback_stop_requested()) {
                result = ESP_ERR_INVALID_STATE;
                break;
            }

            esp_audio_simple_dec_out_t frame = {
                .buffer = output,
                .len = output_size,
            };
            const esp_audio_err_t decode_result = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (decode_result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *larger_output = realloc(output, frame.needed_size);
                if (larger_output == NULL) {
                    ESP_LOGE(TAG, "Could not grow the decode buffer to %lu bytes", (unsigned long)frame.needed_size);
                    result = ESP_ERR_NO_MEM;
                    break;
                }
                output = larger_output;
                output_size = frame.needed_size;
                continue;
            }
            if (decode_result != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Decoding failed (%d)", decode_result);
                result = ESP_FAIL;
                break;
            }
            if (frame.decoded_size > 0) {
                if (!audio_started) {
                    result = start_audio_for_frame(decoder);
                    if (result != ESP_OK) {
                        break;
                    }
                    audio_started = true;
                }
                result = audio_playback_write((const int16_t *)frame.buffer,
                                              frame.decoded_size / sizeof(int16_t));
                if (result != ESP_OK) {
                    break;
                }
            }

            // The decoder caches input until it has a complete frame, so it
            // normally consumes everything. Consuming nothing while producing
            // nothing would make this loop spin forever, so treat it as a
            // corrupt stream instead.
            if (raw.len > 0 && raw.consumed == 0 && frame.decoded_size == 0) {
                ESP_LOGE(TAG, "Decoder made no progress on %lu bytes", (unsigned long)raw.len);
                result = ESP_FAIL;
                break;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        } while (raw.len > 0);

        if (result != ESP_OK) {
            break;
        }
    }

    if (audio_started) {
        audio_playback_stop();
    }
    free(output);
    free(input);
    return result;
}

static esp_err_t play_url(const char *url)
{
    const esp_audio_simple_dec_type_t decoder_type = decoder_type_from_url(url);
    if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGE(TAG, "Cannot choose a decoder for %s: only .mp3 and .wav URLs are supported", url);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Playing %s as %s; free heap %u bytes, minimum free heap %u bytes", url,
             decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 ? "MP3" : "WAV",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());

    esp_http_client_handle_t client = NULL;
    esp_err_t result = open_stream(url, &client);
    if (result != ESP_OK) {
        return result;
    }

    // Neither MP3 nor WAV needs a decoder-specific configuration; the simple
    // decoder obtains the format from the stream itself.
    esp_audio_simple_dec_cfg_t configuration = {
        .dec_type = decoder_type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = NULL;
    const esp_audio_err_t decoder_result = esp_audio_simple_dec_open(&configuration, &decoder);
    if (decoder_result != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Could not open the %s decoder (%d)",
                 decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 ? "MP3" : "WAV", decoder_result);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    result = decode_and_play(decoder, client);

    esp_audio_simple_dec_close(decoder);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

// Ends the current request. busy stays set until the callback has returned, so
// player_stop() cannot return while the caller's session code is still running
// in this task. That also means the callback cannot queue the next URL; there
// is no use for that yet, and allowing it would reopen the race.
static void finish_request(esp_err_t result)
{
    lock_state();
    const bool stopped = stop_requested;
    const player_completion_callback_t callback = requested_callback;
    requested_callback = NULL;
    stop_requested = false;
    unlock_state();

    if (stopped) {
        ESP_LOGI(TAG, "Playback stopped; free heap %u bytes, minimum free heap %u bytes",
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    } else {
        ESP_LOGI(TAG, "Playback finished (0x%x); free heap %u bytes, minimum free heap %u bytes", result,
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
        if (callback != NULL) {
            callback(result);
        }
    }

    lock_state();
    busy = false;
    unlock_state();
    xSemaphoreGive(idle_semaphore);
}

static void player_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        if (xSemaphoreTake(request_semaphore, pdMS_TO_TICKS(PLAYER_IDLE_WAIT_MS)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }
        esp_task_wdt_reset();

        lock_state();
        const bool skip = !busy || stop_requested;
        char url[PLAYER_URL_MAX_LENGTH];
        if (!skip) {
            memcpy(url, requested_url, sizeof(url));
        }
        unlock_state();

        if (skip) {
            // A stop can arrive between queueing and the task waking up. There
            // is nothing to play, but the stop request still needs its idle
            // acknowledgement.
            finish_request(ESP_ERR_INVALID_STATE);
            continue;
        }

        finish_request(play_url(url));
    }
}

void player_init(void)
{
    const esp_audio_err_t decoder_result = esp_audio_dec_register_default();
    const esp_audio_err_t parser_result = esp_audio_simple_dec_register_default();
    ESP_ERROR_CHECK(decoder_result == ESP_AUDIO_ERR_OK && parser_result == ESP_AUDIO_ERR_OK ? ESP_OK : ESP_FAIL);

    state_mutex = xSemaphoreCreateMutex();
    request_semaphore = xSemaphoreCreateBinary();
    idle_semaphore = xSemaphoreCreateBinary();
    stop_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(state_mutex != NULL && request_semaphore != NULL && idle_semaphore != NULL &&
                            stop_mutex != NULL
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);

    const BaseType_t task_result =
        xTaskCreate(player_task, "reply_player", PLAYER_TASK_STACK_SIZE, NULL, PLAYER_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Player ready: MP3 and WAV decoders registered, free heap %u bytes",
             (unsigned)esp_get_free_heap_size());
}

esp_err_t player_play_url(const char *url, player_completion_callback_t callback)
{
    if (url == NULL || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t url_length = strlen(url);
    if (url_length == 0 || url_length >= sizeof(requested_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    if (busy) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(requested_url, url, url_length + 1);
    requested_callback = callback;
    stop_requested = false;
    busy = true;
    unlock_state();

    // Drop an acknowledgement left over from the previous request, otherwise
    // player_stop() could take that one and return before this request ends.
    while (xSemaphoreTake(idle_semaphore, 0) == pdTRUE) {
    }
    xSemaphoreGive(request_semaphore);
    return ESP_OK;
}

esp_err_t player_stop(void)
{
    // Taking the stop mutex first makes a second stop wait for the first one
    // instead of racing it for the single idle acknowledgement.
    xSemaphoreTake(stop_mutex, portMAX_DELAY);

    lock_state();
    if (!busy) {
        unlock_state();
        xSemaphoreGive(stop_mutex);
        return ESP_OK;
    }
    stop_requested = true;
    unlock_state();

    // A request that finished earlier may have left an acknowledgement behind
    // if player_play_url() drained it just before the old finish signalled.
    // Taking it would return before this request has stopped, so keep waiting
    // until the state actually says the player is idle.
    for (;;) {
        xSemaphoreTake(idle_semaphore, portMAX_DELAY);
        lock_state();
        const bool still_busy = busy;
        unlock_state();
        if (!still_busy) {
            break;
        }
    }

    xSemaphoreGive(stop_mutex);
    return ESP_OK;
}
