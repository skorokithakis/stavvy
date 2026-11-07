#include "audio_self_test.h"

#include <math.h>
#include <stdint.h>

#include "audio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_AUDIO_SELF_TEST

static const char *TAG = "audio_self_test";

// 24 kHz deliberately differs from the 16 kHz capture rate, so one run also
// checks that the shared bus is re-clocked correctly when the direction
// changes. 440 Hz is the A above middle C and is easy to recognise by ear.
#define TONE_SAMPLE_RATE_HZ 24000
#define TONE_FREQUENCY_HZ 440
#define TONE_DURATION_MS 1000
#define TONE_AMPLITUDE 8000 // About a quarter of full scale, so the tone is not harsh.

// 480 samples is 20 ms of tone at 24 kHz and 30 ms of microphone data at
// 16 kHz, which keeps the stack buffer at a modest 960 bytes.
#define SAMPLE_CHUNK_COUNT 480

#define LEVEL_LOG_INTERVAL_MS 500
#define LEVEL_TEST_DURATION_MS 4000

#define SELF_TEST_TASK_STACK_SIZE 4096
#define SELF_TEST_TASK_PRIORITY 3

static void play_tone(void)
{
    ESP_ERROR_CHECK(audio_playback_start(TONE_SAMPLE_RATE_HZ, 1));
    ESP_LOGI(TAG, "Playing a %d Hz tone at %d Hz for %d ms; free heap %u bytes", TONE_FREQUENCY_HZ,
             TONE_SAMPLE_RATE_HZ, TONE_DURATION_MS, (unsigned)esp_get_free_heap_size());

    int16_t samples[SAMPLE_CHUNK_COUNT];
    const float two_pi = 6.2831853f;
    const float phase_increment = two_pi * TONE_FREQUENCY_HZ / TONE_SAMPLE_RATE_HZ;
    float phase = 0.0f;
    const size_t chunk_count = TONE_SAMPLE_RATE_HZ * TONE_DURATION_MS / 1000 / SAMPLE_CHUNK_COUNT;
    for (size_t chunk = 0; chunk < chunk_count; chunk++) {
        for (size_t index = 0; index < SAMPLE_CHUNK_COUNT; index++) {
            samples[index] = (int16_t)(TONE_AMPLITUDE * sinf(phase));
            phase += phase_increment;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }
        // The write blocks until the DMA has room for the chunk, which is what
        // paces the tone at real time.
        ESP_ERROR_CHECK(audio_playback_write(samples, SAMPLE_CHUNK_COUNT));
    }
    audio_playback_stop();
}

static void log_microphone_level(void)
{
    ESP_ERROR_CHECK(audio_capture_start());

    int16_t samples[SAMPLE_CHUNK_COUNT];
    const size_t samples_per_log = AUDIO_CAPTURE_SAMPLE_RATE_HZ * LEVEL_LOG_INTERVAL_MS / 1000;
    const size_t log_count = LEVEL_TEST_DURATION_MS / LEVEL_LOG_INTERVAL_MS;
    for (size_t log = 0; log < log_count; log++) {
        int64_t sum_of_squares = 0;
        int32_t peak = 0;
        size_t samples_read = 0;
        while (samples_read < samples_per_log) {
            const size_t remaining = samples_per_log - samples_read;
            const size_t chunk = remaining < SAMPLE_CHUNK_COUNT ? remaining : SAMPLE_CHUNK_COUNT;
            if (audio_capture_read(samples, chunk) != ESP_OK) {
                ESP_LOGE(TAG, "Stopping the microphone test");
                audio_capture_stop();
                return;
            }
            for (size_t index = 0; index < chunk; index++) {
                const int32_t value = samples[index];
                sum_of_squares += (int64_t)value * value;
                const int32_t magnitude = value < 0 ? -value : value;
                if (magnitude > peak) {
                    peak = magnitude;
                }
            }
            samples_read += chunk;
        }

        const int32_t root_mean_square =
            (int32_t)sqrtf((float)sum_of_squares / (float)samples_per_log);
        ESP_LOGI(TAG, "Microphone level: RMS %d, peak %d of 32767", (int)root_mean_square, (int)peak);
        if (log == 0) {
            ESP_LOGI(TAG, "Free heap while capturing: %u bytes", (unsigned)esp_get_free_heap_size());
        }
    }
    audio_capture_stop();
}

static void run_self_test(void *argument)
{
    play_tone();
    log_microphone_level();
    ESP_LOGI(TAG, "Self-test finished; free heap %u bytes", (unsigned)esp_get_free_heap_size());
    vTaskDelete(NULL);
}

#endif // CONFIG_AUDIO_SELF_TEST

void audio_self_test_start(void)
{
#if CONFIG_AUDIO_SELF_TEST
    const BaseType_t result = xTaskCreate(run_self_test, "audio_self_test", SELF_TEST_TASK_STACK_SIZE, NULL,
                                          SELF_TEST_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_FAIL);
#endif
}
