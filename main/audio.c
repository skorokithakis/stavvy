#include "audio.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_i2c.h"

static const char *TAG = "audio";

#define AUDIO_I2S_MCLK_PIN 19
#define AUDIO_I2S_BCLK_PIN 20
#define AUDIO_I2S_WS_PIN 22
#define AUDIO_I2S_DATA_OUT_PIN 23 // To the ES8311 DAC.
#define AUDIO_I2S_DATA_IN_PIN 21  // From the ES7210 ADC.

// The codec drivers take the 8-bit form (address plus the read/write bit) and
// the control interface shifts it down before talking to the bus.
#define ES8311_I2C_ADDRESS 0x30
#define ES7210_I2C_ADDRESS 0x80

// The ES7210 outputs all four of its time slots once three or more microphones
// are enabled, because that is the only way it switches to TDM. The board has
// two microphones, so two slots are silent, and only the first is read.
#define AUDIO_CAPTURE_TDM_SLOT_COUNT 4
#define AUDIO_CAPTURE_MIC_CHANNEL 0

// The amplifier on this board has no enable pin, so this is the only volume
// control there is. 80 % is loud enough to check audio without driving the
// small speaker into distortion.
#define AUDIO_PLAYBACK_VOLUME_PERCENT 80

// The ES7210 gain is in 0.5 dB steps up to 37.5 dB; 30 dB is what Waveshare's
// XiaoZhi firmware uses on this board, so normal speech lands in the useful
// part of the 16-bit range.
#define AUDIO_CAPTURE_GAIN_DB 30

static esp_codec_dev_handle_t capture_device;
static esp_codec_dev_handle_t playback_device;
static bool capture_running;
static bool playback_running;

// Guards the running flags and the bus re-clocking that a direction change
// performs. Both come from different tasks (the wake word task reads, the reply
// player writes), and letting them overlap would leave the shared I2S bus at
// one rate while the codec expects the other.
static SemaphoreHandle_t audio_mutex;

static void lock_audio(void)
{
    // portMAX_DELAY means this can only fail if the mutex was never created.
    ESP_ERROR_CHECK(xSemaphoreTake(audio_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL);
}

static void unlock_audio(void)
{
    xSemaphoreGive(audio_mutex);
}

static void init_i2s_channels(i2s_chan_handle_t *playback_channel, i2s_chan_handle_t *capture_channel)
{
    i2s_chan_config_t channel_configuration = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Clear the transmit buffer after the caller stops feeding it, otherwise the
    // DMA keeps repeating the last buffer and the speaker holds whatever level
    // the audio ended on.
    channel_configuration.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_configuration, playback_channel, capture_channel));

    // Both configurations only have to be valid enough for the channel to be
    // created: esp_codec_dev reprograms slots and clocks to the format that the
    // caller asks for whenever a direction is opened.
    const i2s_std_config_t playback_configuration = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws = AUDIO_I2S_WS_PIN,
            .dout = AUDIO_I2S_DATA_OUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(*playback_channel, &playback_configuration));

    const i2s_tdm_slot_mask_t microphone_slots =
        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3;
    i2s_tdm_config_t capture_configuration = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        microphone_slots),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws = AUDIO_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_DATA_IN_PIN,
        },
    };
    capture_configuration.slot_cfg.total_slot = AUDIO_CAPTURE_TDM_SLOT_COUNT;
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(*capture_channel, &capture_configuration));

    // The transmit channel owns the clock for the duplex pair, and the codecs
    // are configured over I2C while it runs. This is also what the esp_codec_dev
    // test firmware does, so the receive clock cannot end up without a source.
    ESP_ERROR_CHECK(i2s_channel_enable(*playback_channel));
}

static void init_codecs(i2s_chan_handle_t playback_channel, i2s_chan_handle_t capture_channel)
{
    audio_codec_i2s_cfg_t i2s_configuration = {
        .port = I2S_NUM_0,
        .tx_handle = playback_channel,
        .rx_handle = capture_channel,
    };
    // The esp_codec_dev constructors report failure with a NULL pointer rather
    // than an error code, and no part of the audio path works without them, so
    // they are checked like any other fatal setup call.
    const audio_codec_data_if_t *data_interface = audio_codec_new_i2s_data(&i2s_configuration);
    ESP_ERROR_CHECK(data_interface != NULL ? ESP_OK : ESP_FAIL);

    const audio_codec_gpio_if_t *gpio_interface = audio_codec_new_gpio();
    ESP_ERROR_CHECK(gpio_interface != NULL ? ESP_OK : ESP_FAIL);

    audio_codec_i2c_cfg_t i2c_configuration = {
        .port = I2C_NUM_0,
        .addr = ES8311_I2C_ADDRESS,
        .bus_handle = board_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *playback_control = audio_codec_new_i2c_ctrl(&i2c_configuration);
    ESP_ERROR_CHECK(playback_control != NULL ? ESP_OK : ESP_FAIL);

    es8311_codec_cfg_t es8311_configuration = {
        // The ES7210 owns the ADC side of the shared bus, so the ES8311 is only
        // ever a DAC. Leaving it in "both" mode would make it reconfigure the
        // bus for recording as well.
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = playback_control,
        .gpio_if = gpio_interface,
        .pa_pin = -1, // The speaker amplifier has no enable pin on this board.
        .use_mclk = true,
        // The amplifier runs from 5 V and the codec from 3.3 V, which is what
        // the volume curve needs to avoid overdriving the speaker. These are
        // the values Waveshare's XiaoZhi firmware uses for this board.
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    const audio_codec_if_t *playback_codec = es8311_codec_new(&es8311_configuration);
    ESP_ERROR_CHECK(playback_codec != NULL ? ESP_OK : ESP_FAIL);

    esp_codec_dev_cfg_t playback_configuration = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = playback_codec,
        .data_if = data_interface,
    };
    playback_device = esp_codec_dev_new(&playback_configuration);
    ESP_ERROR_CHECK(playback_device != NULL ? ESP_OK : ESP_FAIL);

    i2c_configuration.addr = ES7210_I2C_ADDRESS;
    const audio_codec_ctrl_if_t *capture_control = audio_codec_new_i2c_ctrl(&i2c_configuration);
    ESP_ERROR_CHECK(capture_control != NULL ? ESP_OK : ESP_FAIL);

    es7210_codec_cfg_t es7210_configuration = {
        .ctrl_if = capture_control,
        // Four selected microphones put the ES7210 into TDM mode, which is what
        // the receive channel is set up for. Two of the slots carry the actual
        // microphones and the rest are unused.
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *capture_codec = es7210_codec_new(&es7210_configuration);
    ESP_ERROR_CHECK(capture_codec != NULL ? ESP_OK : ESP_FAIL);

    esp_codec_dev_cfg_t capture_configuration = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = capture_codec,
        .data_if = data_interface,
    };
    capture_device = esp_codec_dev_new(&capture_configuration);
    ESP_ERROR_CHECK(capture_device != NULL ? ESP_OK : ESP_FAIL);
}

void audio_init(void)
{
    audio_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(audio_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    i2s_chan_handle_t playback_channel;
    i2s_chan_handle_t capture_channel;
    init_i2s_channels(&playback_channel, &capture_channel);
    init_codecs(playback_channel, capture_channel);

    ESP_LOGI(TAG, "Audio ready: capture %d Hz mono, playback 16-bit PCM", AUDIO_CAPTURE_SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "Free heap after audio initialisation: %u bytes", (unsigned)esp_get_free_heap_size());
}

static void stop_playback_locked(void)
{
    if (!playback_running) {
        return;
    }
    ESP_ERROR_CHECK(esp_codec_dev_close(playback_device) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    playback_running = false;
}

static void stop_capture_locked(void)
{
    if (!capture_running) {
        return;
    }
    ESP_ERROR_CHECK(esp_codec_dev_close(capture_device) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    capture_running = false;
}

esp_err_t audio_capture_start(void)
{
    lock_audio();
    if (capture_running) {
        unlock_audio();
        return ESP_OK;
    }
    // The transmit channel has to be closed first so that the bus can be
    // re-clocked to the capture rate without the two directions disagreeing.
    stop_playback_locked();

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = AUDIO_CAPTURE_TDM_SLOT_COUNT,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(AUDIO_CAPTURE_MIC_CHANNEL),
        .sample_rate = AUDIO_CAPTURE_SAMPLE_RATE_HZ,
    };
    const int open_result = esp_codec_dev_open(capture_device, &format);
    if (open_result != ESP_CODEC_DEV_OK) {
        // A failed open can leave the device marked as opened, which would make
        // the next attempt a silent no-op, so reset it before giving up.
        esp_codec_dev_close(capture_device);
        unlock_audio();
        ESP_LOGE(TAG, "Could not start capture (0x%x)", open_result);
        return ESP_FAIL;
    }
    const int gain_result =
        esp_codec_dev_set_in_channel_gain(capture_device, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(AUDIO_CAPTURE_MIC_CHANNEL),
                                          AUDIO_CAPTURE_GAIN_DB);
    if (gain_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Could not set the microphone gain (0x%x)", gain_result);
    }

    capture_running = true;
    unlock_audio();
    ESP_LOGI(TAG, "Capture started at %d Hz, microphone channel %d, %d dB gain", AUDIO_CAPTURE_SAMPLE_RATE_HZ,
             AUDIO_CAPTURE_MIC_CHANNEL, AUDIO_CAPTURE_GAIN_DB);
    return ESP_OK;
}

esp_err_t audio_capture_read(int16_t *samples, size_t sample_count)
{
    lock_audio();
    if (!capture_running) {
        unlock_audio();
        return ESP_ERR_INVALID_STATE;
    }
    const int read_result = esp_codec_dev_read(capture_device, samples, (int)(sample_count * sizeof(int16_t)));
    unlock_audio();
    if (read_result != ESP_CODEC_DEV_OK) {
        // A timeout here means the bus stopped clocking, which the caller needs
        // to know about, but it is not worth rebooting over.
        ESP_LOGE(TAG, "Microphone read failed (0x%x)", read_result);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_capture_stop(void)
{
    lock_audio();
    stop_capture_locked();
    unlock_audio();
}

esp_err_t audio_playback_start(uint32_t sample_rate_hz, uint8_t channel_count)
{
    if (channel_count != 1 && channel_count != 2) {
        ESP_LOGE(TAG, "Unsupported playback channel count %d", channel_count);
        return ESP_ERR_INVALID_ARG;
    }

    lock_audio();
    // Reopening is how esp_codec_dev changes the format, so an already running
    // stream is stopped even when the new format happens to match.
    stop_playback_locked();
    stop_capture_locked();

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = channel_count,
        .channel_mask = 0,
        .sample_rate = sample_rate_hz,
    };
    const int open_result = esp_codec_dev_open(playback_device, &format);
    if (open_result != ESP_CODEC_DEV_OK) {
        // A failed open can leave the device marked as opened, which would make
        // the next attempt a silent no-op, so reset it before giving up.
        esp_codec_dev_close(playback_device);
        unlock_audio();
        ESP_LOGE(TAG, "Could not start playback at %u Hz with %d channel(s) (0x%x)", (unsigned)sample_rate_hz,
                 channel_count, open_result);
        return ESP_FAIL;
    }
    const int volume_result = esp_codec_dev_set_out_vol(playback_device, AUDIO_PLAYBACK_VOLUME_PERCENT);
    if (volume_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Could not set the playback volume (0x%x)", volume_result);
    }

    playback_running = true;
    unlock_audio();
    ESP_LOGI(TAG, "Playback started at %u Hz, %d channel(s)", (unsigned)sample_rate_hz, channel_count);
    return ESP_OK;
}

esp_err_t audio_playback_write(const int16_t *samples, size_t sample_count)
{
    lock_audio();
    if (!playback_running) {
        unlock_audio();
        return ESP_ERR_INVALID_STATE;
    }
    // esp_codec_dev does not modify the samples, but its interface takes a
    // mutable pointer because it can apply a software volume in place.
    const int write_result =
        esp_codec_dev_write(playback_device, (void *)samples, (int)(sample_count * sizeof(int16_t)));
    unlock_audio();
    if (write_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Speaker write failed (0x%x)", write_result);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_playback_stop(void)
{
    lock_audio();
    stop_playback_locked();
    unlock_audio();
}
