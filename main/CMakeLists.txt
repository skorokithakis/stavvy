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
        "faces_generated.c"
        "proto/esphome_api.pb.c"
        "wifi_station.c"
        "mdns_service.c"
        "trigger.c"
        "player.c"
        "touch.c"
        "voice_session.c"
        "wake_word.cpp"
    INCLUDE_DIRS "." "proto"
    # The face images are LVGL RGB565A8 data, which LVGL draws directly from
    # flash. Embedding them avoids a PNG decoder and the frame-sized buffer
    # that decoding would need on a chip without PSRAM.
    EMBED_FILES
        "faces/idle_default.bin"
        "faces/listening.bin"
        "faces/thinking.bin"
        "faces/speaking.bin"
        "faces/muted.bin"
        "faces/blink.bin"
        "faces/idle_happy_eyes.bin"
        "faces/idle_curious.bin"
        # The microWakeWord model is read in place by TFLite Micro, so it is
        # mapped from flash and never copied into the tightly constrained heap.
        "wake_word/stavrobot.tflite"
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
