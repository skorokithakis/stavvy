#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "audio.h"
#include "audio_self_test.h"
#include "board_i2c.h"
#include "board_power.h"
#include "display.h"
#include "esphome_api.h"
#include "mdns_service.h"
#include "motion.h"
#include "player.h"
#include "touch.h"
#include "trigger.h"
#include "voice_session.h"
#include "wifi_station.h"

static const char *TAG = "main";

// One line a minute is enough to see a trend without filling the log during a
// long run. The task is not subscribed to the watchdog: it sleeps for a minute
// at a time by design, and a hang in it would not stop the rest of the board.
#define DIAGNOSTICS_HEARTBEAT_INTERVAL_MS 60000
#define DIAGNOSTICS_HEARTBEAT_STACK_BYTES 4096
#define DIAGNOSTICS_HEARTBEAT_PRIORITY 2

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "efuse error";
    case ESP_RST_PWR_GLITCH:
        return "power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "CPU lockup";
    }
    return "unrecognised";
}

static void log_heartbeat(void)
{
    wifi_ap_record_t access_point;
    const bool wifi_connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;

    char rssi_text[16];
    if (wifi_connected) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", access_point.rssi);
    } else {
        snprintf(rssi_text, sizeof(rssi_text), "n/a");
    }

    const int panel_power_mode = display_last_power_mode();
    char panel_text[16];
    if (panel_power_mode >= 0) {
        snprintf(panel_text, sizeof(panel_text), "0x%02X", panel_power_mode);
    } else {
        snprintf(panel_text, sizeof(panel_text), "unknown");
    }

    board_power_log_status();

    ESP_LOGI(TAG,
             "Heartbeat: uptime %lu s, free heap %u bytes, minimum free heap %u bytes, largest free block %u "
             "bytes, Wi-Fi %s (RSSI %s), API client %s, session %s, microphone %s, panel power mode %s",
             (unsigned long)(esp_timer_get_time() / 1000000), (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), wifi_connected ? "connected" : "down",
             rssi_text, esphome_api_client_connected() ? "connected" : "none", voice_session_state_name(),
             voice_session_is_muted() ? "muted" : "unmuted", panel_text);
}

static void heartbeat_task(void *argument)
{
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAGNOSTICS_HEARTBEAT_INTERVAL_MS));
        log_heartbeat();
    }
}

// The boot test only exists to check the speaker path on real hardware. The
// Kconfig string is empty in a normal build, in which case neither the
// semaphore nor the test task is created.
static SemaphoreHandle_t player_test_network_ready;

static void log_heap_usage(const char *moment)
{
    ESP_LOGI(TAG, "Free heap %s: %u bytes, minimum free heap: %u bytes", moment,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

static void handle_player_test_finished(esp_err_t result)
{
    ESP_LOGI(TAG, "Player boot test finished (0x%x); free heap %u bytes, minimum free heap %u bytes", result,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

static void run_player_test_task(void *argument)
{
    xSemaphoreTake(player_test_network_ready, portMAX_DELAY);
    // The IP event fires as soon as DHCP completes, while the AP and the route
    // to the rest of the LAN may still be settling. Give them a moment so this
    // test measures the player rather than the network's start-up.
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Player boot test: playing %s", CONFIG_PLAYER_TEST_URL);

    // The player and the wake word share the I2S bus, and starting playback
    // stops capture anyway. Disabling the trigger first releases the
    // microphone explicitly, so the test exercises the same sequence the
    // session layer will use.
    trigger_disable();
    const esp_err_t result = player_play_url(CONFIG_PLAYER_TEST_URL, handle_player_test_finished);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not queue the player boot test (0x%x)", result);
    }
    vTaskDelete(NULL);
}

static void handle_network_connected(void *handler_argument, esp_event_base_t event_base, int32_t event_id,
                                     void *event_data)
{
    log_heap_usage("after Wi-Fi connect");
    if (player_test_network_ready != NULL) {
        xSemaphoreGive(player_test_network_ready);
    }
}

// Runs in the motion task. display_set_moving() only takes LVGL's lock and
// changes state, so it never blocks on the session or the audio path.
static void handle_motion(bool moving)
{
    display_set_moving(moving);
}

void app_main(void)
{
    // Read before anything else can reset or overwrite the reason. Logging the
    // raw number as well makes a reason this switch does not know still
    // recognisable in the log.
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Booting; reset reason %s (%d)", reset_reason_name(reset_reason), (int)reset_reason);
    log_heap_usage("at boot");

    // The Wi-Fi driver stores calibration data and the station configuration in
    // NVS, so it has to be initialised before esp_wifi_init().
    ESP_ERROR_CHECK(nvs_flash_init());

    board_i2c_init();
    board_power_init();
    // Audio goes after the PMIC so the codecs have their 3.3 V rails, and it
    // uses the same I2C bus as the PMIC.
    audio_init();
    // The player needs the audio module, and it can be ready before the network
    // so the first reply URL never arrives before there is something to play.
    player_init();
    // The AMOLED is powered from ALDO3, so it needs the PMIC. This also creates
    // the SPI2 bus it shares with the SD card.
    display_init();
    // After the display, because the callback draws on it, and after the PMIC,
    // which leaves the 3.3 V rail the IMU runs from on.
    motion_init(handle_motion);
    // Touch comes after the display because a touch asks the display module to
    // re-initialise the panel.
    touch_init();

    // The default event loop has to exist before a handler can subscribe to it,
    // and the network stack has to be up before Wi-Fi starts.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // The test waits for the IP event, so the semaphore has to exist before
    // Wi-Fi can deliver it, but the test itself must not run before the trigger
    // has been initialised.
    if (CONFIG_PLAYER_TEST_URL[0] != '\0') {
        player_test_network_ready = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(player_test_network_ready != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    }

    // Subscribe before Wi-Fi starts so the first connection is not missed.
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handle_network_connected, NULL, NULL));

    wifi_station_connect();
    mdns_service_start();

    // The session queue has to exist before either the button task or the API
    // callbacks can post to it. It comes before trigger_init() because the
    // button task starts polling KEY as soon as it exists.
    voice_session_init();

    // Load the wake word model and configure KEY and BOOT. Detection stays off
    // until the session task applies the boot state below; the buttons work
    // from here on. display_init() has already run, which the brightness
    // callback needs.
    trigger_init(voice_session_handle_wake_word, voice_session_handle_key_button, display_cycle_brightness);

    // lwIP is up as soon as esp_netif is, so the listener can exist before
    // Wi-Fi associates; nothing arrives until the interface has an address.
    esphome_api_start();

    // The session task applies the boot state (unmuted, detection on) itself.
    // Enabling the trigger here would race a KEY press that happened while the
    // board was still booting and could override the mute.
    voice_session_start();

#if CONFIG_AUDIO_SELF_TEST
    audio_self_test_start();
#endif

    if (player_test_network_ready != NULL) {
        const BaseType_t test_task_result =
            xTaskCreate(run_player_test_task, "player_test", 4096, NULL, 4, NULL);
        ESP_ERROR_CHECK(test_task_result == pdPASS ? ESP_OK : ESP_FAIL);
    }

    const BaseType_t heartbeat_task_result =
        xTaskCreate(heartbeat_task, "heartbeat", DIAGNOSTICS_HEARTBEAT_STACK_BYTES, NULL,
                    DIAGNOSTICS_HEARTBEAT_PRIORITY, NULL);
    ESP_ERROR_CHECK(heartbeat_task_result == pdPASS ? ESP_OK : ESP_FAIL);
}
