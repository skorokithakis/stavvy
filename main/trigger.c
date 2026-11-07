#include "trigger.h"

#include <stdbool.h>
#include <stdint.h>

#include "audio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wake_word.h"

static const char *TAG = "trigger";

#define KEY_BUTTON_GPIO 10

// The schematic puts a 10K pull-up (R18) on GPIO10 and the switch to ground,
// so a press reads low. The 100 nF debounce capacitor is not fitted, which is
// why the press has to be stable for a while before it is accepted.
#define KEY_BUTTON_PRESSED_LEVEL 0
#define KEY_BUTTON_DEBOUNCE_US (50 * 1000)

// KEY is polled by its own task rather than from the detection loop: it has to
// work while the wake word is muted and while a session owns the microphone,
// which are exactly the times the detection task is not reading frames. 20 ms
// is short enough to see a press that is held for the 50 ms debounce, and the
// task sleeps between polls.
#define KEY_BUTTON_POLL_INTERVAL_MS 20

// The detection task is blocked on its wake semaphore whenever the trigger is
// disabled, which can last for a whole reply. It has to wake at least once
// inside the watchdog timeout to prove it is still alive, so the wait is timed
// rather than indefinite. 100 ms is far below the timeout and costs nothing.
#define TRIGGER_IDLE_WAIT_MS 100

// The first frames after capture starts carry a large transient from the codec
// and the amplifier rails coming up. Feeding it to the model would either hide a
// real wake word or produce a false one, so it is read and discarded.
#define WARMUP_DISCARD_MS 100

// The inference itself runs in this task, on top of the audio read and the
// callback that posts to the session, so its stack has to cover TFLite Micro and
// the esp-nn kernels. 4 KB is the starting point; the detector logs the stack
// high-water mark once a minute so the real headroom can be seen.
#define TRIGGER_TASK_STACK_SIZE 4096
#define TRIGGER_TASK_PRIORITY 5

// The button task only reads a GPIO, checks a timer and posts a message, but
// the session callback builds its message on the caller's stack, so it needs
// more than a few hundred bytes.
#define BUTTON_TASK_STACK_SIZE 3072
#define BUTTON_TASK_PRIORITY 5

static trigger_callback_t trigger_callback;
static trigger_button_callback_t button_callback;

// One feature step of audio, which is the size the detector expects so that it
// sees a steady stream of slices rather than a burst per read.
static int16_t frame_samples[WAKE_WORD_FRAME_SAMPLES];

static SemaphoreHandle_t state_mutex;
static SemaphoreHandle_t transition_mutex;
static SemaphoreHandle_t wake_semaphore;
static SemaphoreHandle_t stopped_semaphore;

// enabled_request is what the caller asked for, capture_active is what the
// trigger task has actually opened. Keeping them apart lets disable tell the
// difference between "the task has not started yet" and "the task is running".
static bool enabled_request;
static bool capture_active;

static bool key_button_raw_pressed;
static bool key_button_stable_pressed;
static int64_t key_button_raw_changed_at;

static void lock_state(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
}

static void unlock_state(void)
{
    xSemaphoreGive(state_mutex);
}

static void reset_key_button_state(void)
{
    const bool pressed = gpio_get_level(KEY_BUTTON_GPIO) == KEY_BUTTON_PRESSED_LEVEL;
    key_button_raw_pressed = pressed;
    key_button_stable_pressed = pressed;
    key_button_raw_changed_at = esp_timer_get_time();
}

static void poll_key_button(void)
{
    const bool raw_pressed = gpio_get_level(KEY_BUTTON_GPIO) == KEY_BUTTON_PRESSED_LEVEL;
    const int64_t now = esp_timer_get_time();
    if (raw_pressed != key_button_raw_pressed) {
        key_button_raw_pressed = raw_pressed;
        key_button_raw_changed_at = now;
        return;
    }
    if (raw_pressed == key_button_stable_pressed) {
        return;
    }
    if (now - key_button_raw_changed_at < KEY_BUTTON_DEBOUNCE_US) {
        return;
    }
    key_button_stable_pressed = raw_pressed;
    // Only the press edge counts; holding the button must not keep toggling
    // mute, and the release edge is not a request to do anything.
    if (key_button_stable_pressed) {
        ESP_LOGI(TAG, "Key button pressed");
        button_callback();
    }
}

static bool detection_requested(void)
{
    lock_state();
    const bool requested = enabled_request;
    unlock_state();
    return requested;
}

static bool discard_startup_audio(void)
{
    const size_t samples_to_discard = AUDIO_CAPTURE_SAMPLE_RATE_HZ * WARMUP_DISCARD_MS / 1000;
    size_t discarded = 0;
    while (discarded < samples_to_discard) {
        esp_task_wdt_reset();
        const size_t remaining = samples_to_discard - discarded;
        const size_t chunk = remaining < WAKE_WORD_FRAME_SAMPLES ? remaining : WAKE_WORD_FRAME_SAMPLES;
        if (audio_capture_read(frame_samples, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "Could not discard the audio captured during startup");
            return false;
        }
        discarded += chunk;
    }
    return true;
}

static void run_detection(void)
{
    if (!discard_startup_audio()) {
        return;
    }

    // Detection restarts after every session, so whatever the model made of the
    // audio before the session has to go; otherwise a word said just before the
    // session could immediately trigger the next one.
    wake_word_reset();

    while (detection_requested()) {
        esp_task_wdt_reset();
        if (audio_capture_read(frame_samples, WAKE_WORD_FRAME_SAMPLES) != ESP_OK) {
            // A failed read means the I2S bus stopped, so there is nothing
            // useful left to detect until a later enable retries it.
            ESP_LOGE(TAG, "Microphone read failed, stopping detection");
            return;
        }

        if (wake_word_process(frame_samples, WAKE_WORD_FRAME_SAMPLES)) {
            ESP_LOGI(TAG, "Detected wake word");
            trigger_callback();
        }
    }
}

static void trigger_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        // A timed wait also covers the disabled state below, where the task
        // would otherwise sit on the semaphore for the length of a reply.
        if (xSemaphoreTake(wake_semaphore, pdMS_TO_TICKS(TRIGGER_IDLE_WAIT_MS)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }
        esp_task_wdt_reset();

        lock_state();
        const bool should_run = enabled_request;
        if (!should_run) {
            // A disable can wake the task before capture has started. It still
            // needs an acknowledgement, otherwise it would wait forever.
            xSemaphoreGive(stopped_semaphore);
            unlock_state();
            continue;
        }
        unlock_state();

        const esp_err_t start_result = audio_capture_start();
        if (start_result != ESP_OK) {
            ESP_LOGE(TAG, "Could not start capture for wake word detection (0x%x)", start_result);
            lock_state();
            enabled_request = false;
            xSemaphoreGive(stopped_semaphore);
            unlock_state();
            continue;
        }

        lock_state();
        capture_active = true;
        unlock_state();
        ESP_LOGI(TAG, "Detection started; free heap %u bytes, minimum free heap %u bytes",
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());

        run_detection();

        audio_capture_stop();
        lock_state();
        capture_active = false;
        // Clearing the request here, before acknowledging, means an enable
        // that was serialised behind a disable cannot be overwritten after it
        // returns.
        enabled_request = false;
        xSemaphoreGive(stopped_semaphore);
        unlock_state();
    }
}

static void button_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        poll_key_button();
        vTaskDelay(pdMS_TO_TICKS(KEY_BUTTON_POLL_INTERVAL_MS));
    }
}

void trigger_init(trigger_callback_t wake_word_callback, trigger_button_callback_t key_button_callback)
{
    ESP_ERROR_CHECK(wake_word_callback != NULL && key_button_callback != NULL ? ESP_OK : ESP_ERR_INVALID_ARG);
    trigger_callback = wake_word_callback;
    button_callback = key_button_callback;

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << KEY_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        // The board already has an external pull-up; the internal one is
        // enabled as well so the pin is defined even before that rail is up.
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
    reset_key_button_state();

    ESP_LOGI(TAG, "Free heap before loading the wake word model: %u bytes",
             (unsigned)esp_get_free_heap_size());

    wake_word_init();

    state_mutex = xSemaphoreCreateMutex();
    transition_mutex = xSemaphoreCreateMutex();
    wake_semaphore = xSemaphoreCreateBinary();
    stopped_semaphore = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(state_mutex != NULL && transition_mutex != NULL && wake_semaphore != NULL &&
                            stopped_semaphore != NULL
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);

    const BaseType_t task_result =
        xTaskCreate(trigger_task, "wake_word", TRIGGER_TASK_STACK_SIZE, NULL, TRIGGER_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_FAIL);

    // The button task is independent of detection so KEY keeps working when
    // the microphone is muted or a session has taken the capture device. It is
    // started last, after the mutexes and the wake word model exist, because a
    // press calls into the session task which may in turn call trigger_enable()
    // or trigger_disable().
    const BaseType_t button_task_result = xTaskCreate(button_task, "key_button", BUTTON_TASK_STACK_SIZE, NULL,
                                                      BUTTON_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(button_task_result == pdPASS ? ESP_OK : ESP_FAIL);
}

esp_err_t trigger_enable(void)
{
    xSemaphoreTake(transition_mutex, portMAX_DELAY);
    lock_state();
    if (!enabled_request) {
        enabled_request = true;
        xSemaphoreGive(wake_semaphore);
    }
    unlock_state();
    xSemaphoreGive(transition_mutex);
    return ESP_OK;
}

void trigger_disable(void)
{
    xSemaphoreTake(transition_mutex, portMAX_DELAY);
    lock_state();

    if (!enabled_request && !capture_active) {
        unlock_state();
        xSemaphoreGive(transition_mutex);
        return;
    }

    enabled_request = false;
    // Notifications from earlier cycles are stale. Dropping them means the
    // wait below can only be satisfied by this request finishing, not by a
    // leftover acknowledgement.
    while (xSemaphoreTake(stopped_semaphore, 0) == pdTRUE) {
    }
    xSemaphoreGive(wake_semaphore);
    unlock_state();

    xSemaphoreTake(stopped_semaphore, portMAX_DELAY);
    xSemaphoreGive(transition_mutex);
}
