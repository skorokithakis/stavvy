#include <stdint.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
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
#include "player.h"
#include "trigger.h"
#include "wifi_station.h"

static const char *TAG = "main";

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

static void handle_trigger(trigger_source_t source)
{
    ESP_LOGI(TAG, "Trigger raised by %s (free heap %u bytes, minimum free heap %u bytes)",
             source == TRIGGER_SOURCE_WAKE_WORD ? "wake word" : "key button",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting");
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
    // lwIP is up as soon as esp_netif is, so the listener can exist before
    // Wi-Fi associates; nothing arrives until the interface has an address.
    esphome_api_start();

#if CONFIG_AUDIO_SELF_TEST
    audio_self_test_start();
#endif

    // There is no session layer yet, so the trigger is always enabled and the
    // callback only logs. The model is loaded after the network is up so the
    // heap report below reflects the memory WakeNet actually takes from the
    // firmware's steady state.
    trigger_init(handle_trigger);
    trigger_enable();

    // There is no session task yet, so show every state once as a hardware
    // check. It runs last so the network, wake word and display are all up
    // before the demo holds the main task for about nine seconds.
    display_run_boot_demo();

    if (player_test_network_ready != NULL) {
        const BaseType_t test_task_result =
            xTaskCreate(run_player_test_task, "player_test", 4096, NULL, 4, NULL);
        ESP_ERROR_CHECK(test_task_result == pdPASS ? ESP_OK : ESP_FAIL);
    }
}
