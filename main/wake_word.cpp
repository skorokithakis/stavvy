// The streaming inference, op list, arena handling and sliding-window averaging
// here follow ESPHome's components/micro_wake_word (streaming_model.cpp and
// micro_wake_word.cpp) rather than the microWakeWord reference implementation.
// ESPHome's version is the one known to run on Espressif chips with esp-nn
// 1.1.2, which is the pairing this firmware uses.
//
// Adapted from ESPHome's micro_wake_word component:
// Copyright 2024 Kevin Ahrendt
// Licensed under the GNU General Public License v3.0, like the rest of this
// firmware (AGPL-3.0).

#include "wake_word.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <frontend.h>
#include <frontend_util.h>
#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <tensorflow/lite/micro/micro_utils.h>

#include "audio.h"

static const char *TAG = "wake_word";

// These four come from main/wake_word/stavrobot.json, the microWakeWord
// manifest that ships with the model. The cutoff is stored there as a
// probability and used here in the model's quantised uint8 form, which is how
// ESPHome converts it too (int(cutoff * 255)).
static constexpr uint8_t PROBABILITY_CUTOFF = 247; // 0.97
static constexpr size_t SLIDING_WINDOW_SIZE = 5;
static constexpr uint8_t FEATURE_STEP_SIZE_MS = 10;
static constexpr size_t TENSOR_ARENA_SIZE = 38000;

// The feature frontend's settings are fixed for every microWakeWord model, so
// they are not in the manifest. These values are what all of them are trained
// with, and they are copied from ESPHome's preprocessor_settings.h.
static constexpr uint8_t FEATURE_SIZE = 40;
static constexpr uint8_t FEATURE_DURATION_MS = 30;
static constexpr float FILTERBANK_LOWER_BAND_LIMIT = 125.0;
static constexpr float FILTERBANK_UPPER_BAND_LIMIT = 7500.0;
static constexpr uint8_t NOISE_REDUCTION_SMOOTHING_BITS = 10;
static constexpr float NOISE_REDUCTION_EVEN_SMOOTHING = 0.025;
static constexpr float NOISE_REDUCTION_ODD_SMOOTHING = 0.06;
static constexpr float NOISE_REDUCTION_MIN_SIGNAL_REMAINING = 0.05;
static constexpr bool PCAN_GAIN_CONTROL_ENABLE_PCAN = true;
static constexpr float PCAN_GAIN_CONTROL_STRENGTH = 0.95;
static constexpr float PCAN_GAIN_CONTROL_OFFSET = 80.0;
static constexpr uint8_t PCAN_GAIN_CONTROL_GAIN_BITS = 21;
static constexpr bool LOG_SCALE_ENABLE_LOG = true;
static constexpr uint8_t LOG_SCALE_SCALE_SHIFT = 6;

// ESPHome keeps the resource-variable arrays in their own small arena, apart
// from the tensor arena, because TFLite Micro refuses to put them in the same
// one. 1 KB and 20 variables are ESPHome's values and are sized for these
// streaming wake word models.
static constexpr size_t VARIABLE_ARENA_SIZE = 1024;
static constexpr size_t RESOURCE_VARIABLE_COUNT = 20;

// Detection stays blocked until the model has heard this many feature slices
// (10 ms each, so one second) below the cutoff. Counting slices rather than
// inferences keeps that second fixed whatever the model's stride is, and it
// stops one utterance from firing again as soon as detection is re-enabled.
static constexpr int16_t MIN_SLICES_BEFORE_DETECTION = 100;

// One line a minute is enough to compare the inference cost against the
// inference period without flooding the log while detection runs.
static constexpr int64_t STATS_INTERVAL_US = 60 * 1000 * 1000;

extern const uint8_t stavrobot_tflite[];

static tflite::MicroMutableOpResolver<20> op_resolver;

// The arenas and interpreter live for the whole uptime: detection is toggled
// many times, and reallocating them on every enable would fragment the heap for
// no benefit.
static uint8_t *tensor_arena;
static uint8_t *variable_arena;
static tflite::MicroAllocator *variable_allocator;
static tflite::MicroResourceVariables *resource_variables;
static tflite::MicroInterpreter *interpreter;

static struct FrontendConfig frontend_config;
static struct FrontendState frontend_state;

// The input tensor holds the model's stride number of feature slices. Each call
// fills one slice and only the last one in the stride runs Invoke().
static uint8_t model_stride;
static uint8_t current_stride_step;

// The model's output is a uint8 probability per inference. The sliding window
// average of these is what is compared with the cutoff.
static uint8_t recent_probabilities[SLIDING_WINDOW_SIZE];
static size_t next_probability_index;
static int16_t slices_below_cutoff;
// The most recent model output, 0 until the first inference. The cool-off
// counter needs it on the slices between inferences, when no new probability
// arrives.
static uint8_t latest_probability;

static int64_t stats_started_at_us;
static int64_t inference_total_us;
static int64_t inference_max_us;
static int64_t inference_period_us;
static uint32_t inference_count;

static bool register_ops(tflite::MicroMutableOpResolver<20> &resolver)
{
    return resolver.AddCallOnce() == kTfLiteOk && resolver.AddVarHandle() == kTfLiteOk &&
           resolver.AddReshape() == kTfLiteOk && resolver.AddReadVariable() == kTfLiteOk &&
           resolver.AddStridedSlice() == kTfLiteOk && resolver.AddConcatenation() == kTfLiteOk &&
           resolver.AddAssignVariable() == kTfLiteOk && resolver.AddConv2D() == kTfLiteOk &&
           resolver.AddMul() == kTfLiteOk && resolver.AddAdd() == kTfLiteOk && resolver.AddMean() == kTfLiteOk &&
           resolver.AddFullyConnected() == kTfLiteOk && resolver.AddLogistic() == kTfLiteOk &&
           resolver.AddQuantize() == kTfLiteOk && resolver.AddDepthwiseConv2D() == kTfLiteOk &&
           resolver.AddAveragePool2D() == kTfLiteOk && resolver.AddMaxPool2D() == kTfLiteOk &&
           resolver.AddPad() == kTfLiteOk && resolver.AddPack() == kTfLiteOk && resolver.AddSplitV() == kTfLiteOk;
}

static void configure_frontend(void)
{
    frontend_config.window.size_ms = FEATURE_DURATION_MS;
    frontend_config.window.step_size_ms = FEATURE_STEP_SIZE_MS;
    frontend_config.filterbank.num_channels = FEATURE_SIZE;
    frontend_config.filterbank.lower_band_limit = FILTERBANK_LOWER_BAND_LIMIT;
    frontend_config.filterbank.upper_band_limit = FILTERBANK_UPPER_BAND_LIMIT;
    frontend_config.noise_reduction.smoothing_bits = NOISE_REDUCTION_SMOOTHING_BITS;
    frontend_config.noise_reduction.even_smoothing = NOISE_REDUCTION_EVEN_SMOOTHING;
    frontend_config.noise_reduction.odd_smoothing = NOISE_REDUCTION_ODD_SMOOTHING;
    frontend_config.noise_reduction.min_signal_remaining = NOISE_REDUCTION_MIN_SIGNAL_REMAINING;
    frontend_config.pcan_gain_control.enable_pcan = PCAN_GAIN_CONTROL_ENABLE_PCAN;
    frontend_config.pcan_gain_control.strength = PCAN_GAIN_CONTROL_STRENGTH;
    frontend_config.pcan_gain_control.offset = PCAN_GAIN_CONTROL_OFFSET;
    frontend_config.pcan_gain_control.gain_bits = PCAN_GAIN_CONTROL_GAIN_BITS;
    frontend_config.log_scale.enable_log = LOG_SCALE_ENABLE_LOG;
    frontend_config.log_scale.scale_shift = LOG_SCALE_SCALE_SHIFT;

    ESP_ERROR_CHECK(FrontendPopulateState(&frontend_config, &frontend_state, AUDIO_CAPTURE_SAMPLE_RATE_HZ) == 1
                        ? ESP_OK
                        : ESP_FAIL);
}

static void log_inference_stats(void)
{
    const int64_t elapsed_us = esp_timer_get_time() - stats_started_at_us;
    const uint32_t average_us = inference_count > 0 ? (uint32_t)(inference_total_us / inference_count) : 0;

    // The stack high-water mark belongs in this line: the inference runs in the
    // trigger task, and this is the only way to see whether its stack is still
    // large enough.
    ESP_LOGI(TAG,
             "Inference: average %u us, maximum %u us over %u runs in %lld s; period %u us, stack free %u bytes, "
             "free heap %u bytes, minimum free heap %u bytes",
             (unsigned)average_us, (unsigned)inference_max_us, (unsigned)inference_count,
             (long long)(elapsed_us / 1000000), (unsigned)inference_period_us,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());

    stats_started_at_us = esp_timer_get_time();
    inference_total_us = 0;
    inference_max_us = 0;
    inference_count = 0;
}

// Returns true when the sliding window average of the probabilities is above
// the cutoff. The cool-off counter blocks detection until enough feature slices
// in a row have been below the cutoff, so a detection cannot immediately repeat.
static bool probability_window_reports_detection(void)
{
    if (slices_below_cutoff < 0) {
        return false;
    }

    uint32_t total = 0;
    for (size_t index = 0; index < SLIDING_WINDOW_SIZE; ++index) {
        total += recent_probabilities[index];
    }
    return total > PROBABILITY_CUTOFF * SLIDING_WINDOW_SIZE;
}

// Forgets everything the model has seen. Both wake_word_reset() and a positive
// detection use it: after a detection the window still holds the probabilities
// that caused it, and detection can stay enabled (the "No server" case) long
// enough for them to fire the callback again.
static void clear_probability_window(void)
{
    std::memset(recent_probabilities, 0, sizeof(recent_probabilities));
    next_probability_index = 0;
    slices_below_cutoff = -MIN_SLICES_BEFORE_DETECTION;
    latest_probability = 0;
}

static bool process_feature(const int8_t *features)
{
    TfLiteTensor *input = interpreter->input(0);
    std::memcpy(tflite::GetTensorData<int8_t>(input) + FEATURE_SIZE * current_stride_step, features, FEATURE_SIZE);
    ++current_stride_step;

    bool detected = false;
    if (current_stride_step >= model_stride) {
        current_stride_step = 0;

        const int64_t started_at_us = esp_timer_get_time();
        const TfLiteStatus invoke_status = interpreter->Invoke();
        const int64_t elapsed_us = esp_timer_get_time() - started_at_us;
        ESP_ERROR_CHECK(invoke_status == kTfLiteOk ? ESP_OK : ESP_FAIL);

        inference_total_us += elapsed_us;
        if (elapsed_us > inference_max_us) {
            inference_max_us = elapsed_us;
        }
        ++inference_count;
        if (esp_timer_get_time() - stats_started_at_us >= STATS_INTERVAL_US) {
            log_inference_stats();
        }

        latest_probability = interpreter->output(0)->data.uint8[0];
        recent_probabilities[next_probability_index] = latest_probability;
        next_probability_index = (next_probability_index + 1) % SLIDING_WINDOW_SIZE;
        detected = probability_window_reports_detection();
    }

    // The cool-off counts feature slices, not inferences, so a slice between two
    // inferences counts while the last probability was below the cutoff. That
    // makes the cool-off one second of audio regardless of the model's stride.
    if (latest_probability < PROBABILITY_CUTOFF && slices_below_cutoff < 0) {
        ++slices_below_cutoff;
    }

    if (detected) {
        clear_probability_window();
    }

    return detected;
}

void wake_word_init(void)
{
    configure_frontend();
    ESP_ERROR_CHECK((register_ops(op_resolver) ? ESP_OK : ESP_FAIL));

    const tflite::Model *model = tflite::GetModel(stavrobot_tflite);
    ESP_ERROR_CHECK(model->version() == TFLITE_SCHEMA_VERSION ? ESP_OK : ESP_FAIL);

    variable_arena = (uint8_t *)malloc(VARIABLE_ARENA_SIZE);
    ESP_ERROR_CHECK(variable_arena != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    variable_allocator = tflite::MicroAllocator::Create(variable_arena, VARIABLE_ARENA_SIZE);
    ESP_ERROR_CHECK(variable_allocator != NULL ? ESP_OK : ESP_FAIL);
    resource_variables = tflite::MicroResourceVariables::Create(variable_allocator, RESOURCE_VARIABLE_COUNT);
    ESP_ERROR_CHECK(resource_variables != NULL ? ESP_OK : ESP_FAIL);

    tensor_arena = (uint8_t *)malloc(TENSOR_ARENA_SIZE);
    ESP_ERROR_CHECK(tensor_arena != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    interpreter =
        new tflite::MicroInterpreter(model, op_resolver, tensor_arena, TENSOR_ARENA_SIZE, resource_variables);
    ESP_ERROR_CHECK(interpreter->AllocateTensors() == kTfLiteOk ? ESP_OK : ESP_FAIL);

    // A model that does not match these shapes would either be fed features it
    // cannot read or produce output this module cannot interpret, so treat it as
    // a setup failure rather than something to paper over while it runs.
    TfLiteTensor *input = interpreter->input(0);
    ESP_ERROR_CHECK(input->dims->size == 3 && input->dims->data[0] == 1 && input->dims->data[2] == FEATURE_SIZE &&
                            input->type == kTfLiteInt8
                        ? ESP_OK
                        : ESP_FAIL);
    TfLiteTensor *output = interpreter->output(0);
    ESP_ERROR_CHECK(output->dims->size == 2 && output->dims->data[0] == 1 && output->dims->data[1] == 1 &&
                            output->type == kTfLiteUInt8
                        ? ESP_OK
                        : ESP_FAIL);

    model_stride = input->dims->data[1];
    inference_period_us = (int64_t)model_stride * FEATURE_STEP_SIZE_MS * 1000;

    ESP_LOGI(TAG,
             "Model loaded: %u bytes of tensor arena in use, %u bytes allocated, input stride %u, "
             "inference period %u us, free heap %u bytes",
             (unsigned)interpreter->arena_used_bytes(), (unsigned)TENSOR_ARENA_SIZE, (unsigned)model_stride,
             (unsigned)inference_period_us, (unsigned)esp_get_free_heap_size());

    wake_word_reset();
}

void wake_word_reset(void)
{
    clear_probability_window();
    current_stride_step = 0;
    FrontendReset(&frontend_state);

    // The model keeps its streaming state (the resource variables) between
    // invocations, so the audio from before a session would otherwise still be
    // in it when detection resumes.
    ESP_ERROR_CHECK(interpreter->Reset() == kTfLiteOk ? ESP_OK : ESP_FAIL);

    stats_started_at_us = esp_timer_get_time();
    inference_total_us = 0;
    inference_max_us = 0;
    inference_count = 0;
}

bool wake_word_process(const int16_t *samples, size_t sample_count)
{
    size_t processed_samples = 0;
    const struct FrontendOutput output =
        FrontendProcessSamples(&frontend_state, samples, sample_count, &processed_samples);
    ESP_ERROR_CHECK(processed_samples == sample_count ? ESP_OK : ESP_FAIL);

    if (output.size == 0) {
        return false;
    }

    // The frontend's features are integers in roughly 0 to 670, which training
    // divided by 25.6 and then quantised into int8. Combining both scalings
    // gives feature * 256 / (25.6 * 26.0), with 666 as the rounded divisor.
    int8_t features[FEATURE_SIZE];
    for (size_t index = 0; index < output.size; ++index) {
        constexpr int32_t VALUE_SCALE = 256;
        constexpr int32_t VALUE_DIVISOR = 666;
        int32_t value = ((output.values[index] * VALUE_SCALE) + (VALUE_DIVISOR / 2)) / VALUE_DIVISOR;
        value += INT8_MIN;
        if (value < INT8_MIN) {
            value = INT8_MIN;
        } else if (value > INT8_MAX) {
            value = INT8_MAX;
        }
        features[index] = (int8_t)value;
    }

    return process_feature(features);
}
