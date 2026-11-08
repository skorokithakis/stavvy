idf_component_register(
    SRCS
        "main.c"
        "audio.c"
        "audio_self_test.c"
        "board_i2c.c"
        "board_power.c"
        "display.c"
        "esphome_api.c"
        "faces.c"
        "fonts/dm_sans_semibold_26.c"
        "fonts/dm_sans_italic_24.c"
        "fonts/dm_sans_italic_18.c"
        "proto/esphome_api.pb.c"
        "wifi_station.c"
        "mdns_service.c"
        "motion.c"
        "trigger.c"
        "player.c"
        "touch.c"
        "voice_session.c"
        "wake_word.cpp"
    INCLUDE_DIRS "." "proto"
    EMBED_FILES
        # The microWakeWord model is read in place by TFLite Micro, so it is
        # mapped from flash and never copied into the tightly constrained heap.
        "wake_word/stavrobot.tflite"
        # The wake tone is a few tens of kilobytes, far too much to keep in RAM
        # for a cosmetic sound, so it is read from flash as it plays.
        "sounds/wake_word_triggered.pcm"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_spi
        esp_event
        esp_http_client
        esp_lcd
        esp_netif
        esp_timer
        esp_wifi
        esp_codec_dev
        espressif__esp_audio_codec
        espressif__esp_lcd_sh8601
        espressif__esp_lvgl_port
        espressif__esp-tflite-micro
        esphome__esp-micro-speech-features
        lvgl__lvgl
        lwip
        mdns
        nanopb
        nvs_flash
)
