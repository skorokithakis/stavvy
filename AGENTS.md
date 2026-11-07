# Hardware: Waveshare ESP32-C6-Touch-AMOLED-2.16

All firmware in this repository targets this one board. The project goal is not decided yet.

Toolchain: ESP-IDF (C). Do not use Arduino, ESPHome or MicroPython. Waveshare's ESP-IDF examples target ESP-IDF v5.5.3.

- SKU 34202: ESP32-C6-Touch-AMOLED-2.16 (with lithium battery).
- SKU 34201: ESP32-C6-Touch-AMOLED-2.16-EN (without battery).

## Links

- Wiki: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16
- Resources (schematic, datasheets): https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/Resources-And-Documents
- Arduino guide: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/Arduino
- ESP-IDF guide: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/ESP-IDF
- FAQ: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/FAQ
- Example code (ESP-IDF v5.5.3, Arduino core v3.3.3, XiaoZhi voice assistant): https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16
- Schematic: https://files.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-2.16/ESP32-C6-Touch-AMOLED-2.16-Schematic.pdf
- Product page: https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm
- ESP32-C6 datasheet: https://documentation.espressif.com/esp32-c6_datasheet_en.pdf
- ESP32-C6 technical reference manual: https://documentation.espressif.com/esp32-c6_technical_reference_manual_en.pdf
- Datasheets: [CO5300](https://files.waveshare.com/wiki/common/Co5300_Datasheet.pdf), [AXP2101](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf), [ES8311](https://files.waveshare.com/wiki/common/ES8311.DS.pdf), [QMI8658](https://files.waveshare.com/wiki/common/QMI8658C_datasheet_rev_0.9.pdf), [PCF85063](https://files.waveshare.com/wiki/common/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf)

## Specs

| Part | Chip | Notes |
| --- | --- | --- |
| SoC | ESP32-C6 | Single-core RISC-V, 160 MHz. Wi-Fi 6 (2.4 GHz), BLE 5, 802.15.4 (Zigbee, Thread). |
| RAM | On-chip | 512 KB HP SRAM, 16 KB LP SRAM. **No PSRAM, and the C6 cannot use PSRAM.** |
| Flash | NOR | 16 MB external. |
| Display | CO5300 driver | 2.16 inch AMOLED, 480 × 480, 16.7M colours, 600 cd/m², QSPI. No backlight pin (AMOLED; brightness is a panel command, 0x51). |
| Touch | CST9220 | Capacitive, I2C. |
| Power | AXP2101 PMIC | I2C address 0x34. Battery charge/discharge, rails (ALDO1 to ALDO4 power the peripherals, see the Waveshare examples). Drives the PWR button. |
| IMU | QMI8658 | 3-axis accelerometer and 3-axis gyroscope, I2C. |
| RTC | PCF85063 | I2C. AXP2101 backs it up from the battery. |
| Audio out | ES8311 codec + speaker amplifier | 2-pin speaker pads. |
| Audio in | ES7210 ADC | Dual microphone array, echo-cancellation reference. |
| Storage | TF (microSD) slot | FAT32. SPI mode. |
| Battery | MX1.25 2-pin | 3.7 V lithium. |
| Antenna | Onboard chip antenna | IPEX1 connector is available after you move an onboard resistor. |
| USB | Type-C | Native ESP32-C6 USB (USB-Serial-JTAG) for flashing and logs. |
| Buttons | BOOT, PWR, KEY | BOOT: hold at power-on for download mode. PWR: long press off, short press on (board powers on when power connects). KEY: user button. |
| Expansion | Solder pads | 1× I2C, 1× UART, 1× USB. |

## Pin map

Verified against the schematic unless marked otherwise.

| Function | GPIO |
| --- | --- |
| Display QSPI CLK | 0 |
| Display QSPI D0 to D3 | 1, 2, 3, 4 |
| Display CS | 15 |
| Display reset | Not connected to a GPIO |
| Touch INT | 5 |
| Touch RST | 11 |
| Shared I2C SDA / SCL (PMIC, touch, IMU, RTC, codecs) | 8 / 7 |
| IMU INT1 / INT2 | 16 / 17 |
| I2S MCLK / BCLK / WS / DOUT (to ES8311) / DIN (from ES7210) | 19 / 20 / 22 / 23 / 21 |
| SD card CLK / MOSI / MISO (shared with display QSPI bus) | 0 / 1 / 2 |
| SD card CS | 6 |
| BOOT button | 9 |
| KEY button | 10 |
| PWR button (readable state; the AXP2101 also handles power on and off) | 18 |
| USB D- / D+ | 12 / 13 |
| Speaker amplifier enable | Not a GPIO in Waveshare's code (`GPIO_NUM_NC`). Not confirmed how it is enabled. |

## Build and flash

The ESP-IDF toolchain is not installed on the host. Use Espressif's v5.5.3 Docker image and run it as the host user, so `build/` and `sdkconfig` are not left owned by root:

```sh
docker pull espressif/idf:v5.5.3

docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
    espressif/idf:v5.5.3 idf.py build
```

The first build downloads the mDNS component from the ESP component registry, so it needs network access.

Flash and watch the log over the board's USB port. `--group-add dialout` lets the container open the port, and the device may enumerate as something other than `ttyACM0`:

```sh
docker run --rm -it --device=/dev/ttyACM0 --group-add dialout -v "$PWD":/project -w /project \
    -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5.3 \
    idf.py -p /dev/ttyACM0 flash monitor
```

Wi-Fi credentials are set at build time. `menuconfig` writes them to the git-ignored `sdkconfig`, so they never enter the repository:

```sh
docker run --rm -it -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
    espressif/idf:v5.5.3 idf.py menuconfig
```

## ESPHome native API server

The firmware runs a plaintext ESPHome native API server on TCP port 6053, limited to the messages `havpe-server` uses for its voice pipeline. The wire protocol lives in `main/proto/esphome_api.proto`, a reduced copy of ESPHome's `api.proto` with identical field numbers.

nanopb is used without dynamic allocation. Its runtime is vendored in `components/nanopb` (0.4.9.1, zlib licence). The generated `main/proto/esphome_api.pb.c` and `.pb.h` are committed because the ESP-IDF Docker image has no protobuf toolchain; regenerate them on the host after editing the `.proto`:

```sh
./main/proto/regenerate.sh
```

The script creates a virtualenv in `$TMPDIR` and installs `nanopb==0.4.9.1` (which bundles protoc), so `python3` is the only host requirement.

## Wake word

The trigger module (`main/trigger.c`) runs Alexa wake word detection with
WakeNet9s (`wn9s_alexa`) from ESP-SR, without the AFE because the AFE needs PSRAM.
The model lives in the `model` data partition. ESP-SR's build step packs the
selected models into `build/srmodels/srmodels.bin` and adds it to the `flash`
target, so the normal `idf.py flash` writes both the application and the model.
`build/flash_args` lists the model at the partition's offset.

ESP-SR's Kconfig does not offer `wn9s_alexa` even though the model ships in the
component. Its packer selects models by scanning `sdkconfig` for
`CONFIG_SR_WN_<model name>`, so `main/Kconfig.projbuild` declares
`CONFIG_SR_WN_WN9S_ALEXA` itself. Keep it the only enabled `SR_WN` symbol so the
model image stays small.

The KEY button is on GPIO10. The schematic shows a 10K pull-up (R18) to 3.3 V
and the switch to ground, so it reads low when pressed. The 100 nF debounce
capacitor (C22) is marked not fitted, so the firmware debounces the press. KEY
toggles the microphone mute; it is polled by its own task in `trigger.c` rather
than by the detection loop, because it has to work while the microphone is
muted and while a session owns the capture device.

## Voice session

`main/voice_session.c` connects the trigger, the API server, the microphone and
the player into the flow `havpe-server` drives. One session task owns all of it:
the trigger, API and player callbacks only post messages to its queue, because
`trigger_disable()` and `player_stop()` wait for those callbacks' tasks and
calling them from a callback would stall or deadlock that task.

States are idle, awaiting response, streaming, thinking and speaking. A trigger
with no subscribed API client shows "No server" and leaves the trigger enabled.
Otherwise the trigger is disabled, `VoiceAssistantRequest(start=true)` goes out
and the screen shows Listening. The client answers with port 0, after which the
session reads 512 samples (32 ms, 1024 bytes) at a time and sends each as one
`VoiceAssistantAudio` frame; it handles its own queue between chunks.
`STT_VAD_END`, `ERROR` and `RUN_END` stop the stream. `TTS_END` plays the URL and
the player completion returns to idle; `RUN_END` alone returns to idle once it
arrives, unless playback is still running. A client disconnect stops streaming
and playback. Streaming is cancelled with `VoiceAssistantRequest(start=false)`
after 20 seconds without a stop event; the response and thinking waits have
their own shorter timeouts so a wedged client cannot keep the trigger disabled.

KEY toggles the microphone mute and does not start a session. Muting disables
detection and releases the capture device; the idle screen shows a red "Muted"
line. Unmuting re-enables detection. A press during a session only records the
requested state, which `return_to_idle()` applies when the session ends, so the
reply is never cut off. Mute starts false on every boot and is not stored.

`CONFIG_SESSION_SIMULATED_TRIGGER` raises a simulated wake word trigger three
seconds after an API client connects, for bench checks without speaking to the
board. It is off by default and is ignored while muted.

## Gotchas

- **Freeing the serial port.** If flashing fails because `/dev/ttyACM0` is busy, find the holder with `fuser -v /dev/ttyACM0` (or `docker ps` for a leftover IDF container) and stop only that process. Never use `pkill -f` with a short pattern: `pkill -f tio` once killed the user's whole graphical session.

- **Console is USB only.** ESP-IDF's default UART0 console uses GPIO16/GPIO17, which are the IMU INT1/INT2 lines here. `sdkconfig.defaults` moves the console to the USB-Serial-JTAG port. An existing `sdkconfig` overrides `sdkconfig.defaults`, so after changing defaults, change `sdkconfig` too or delete it (you then have to set Wi-Fi credentials again).

- **Frame buffer does not fit in RAM.** One 480 × 480 frame at 16 bits per pixel is about 460 KB, nearly all of the 512 KB SRAM. Draw in partial strips (for example, LVGL partial render mode with a buffer of a few tens of lines). Full-screen animation and video will be slow.
- **Waveshare's ESP-IDF and Arduino examples swap two pins.** Their `user_config.h` sets LCD CS = GPIO5 and touch INT = GPIO15. The schematic and Waveshare's XiaoZhi board config (`main/boards/waveshare/esp32-c6-touch-amoled-2.16/config.h`) use LCD CS = GPIO15 and touch INT = GPIO5. Trust the schematic.
- **SD card and display share one SPI bus** (GPIO0 to GPIO2). Use one SPI host with two devices, and do not access both at the same time without bus locking.
- **The AXP2101 must be set up before peripherals work.** The Waveshare examples enable its ALDO rails at boot. The display, microphones and other parts may not have power until then.
- **Display connector wiring (J4).** `LCD_RESET` is not on a GPIO: it is pulled up to the ALDO3 rail through R16 (10K), so the panel only resets on an ALDO3 power-up or a dip/noise on that line. The schematic's summary table says GPIO10 = `LCD_RESET`; that is wrong (pressing KEY on GPIO10 does not affect the panel). `LCD_TE` goes only to test pad TP20, not to a GPIO. `DSI_PWR_EN` is pulled up to 3.3 V (always on).
- **Unsolved: the screen sometimes goes black while idle,** a few minutes after boot, while everything else keeps running. Touching the screen (which re-sends the panel init list) brings it back, so the panel has lost its configuration. Leading hypothesis: a short ALDO3 dip or noise on the reset line, possibly from the charger cycling at its 4.1 V end point (the battery sits at about 4.09 V in CV charging). Not proven; see the open ticket.
- **The CO5300 does not answer DCS reads over QSPI on this board.** Reading 0x0A (Read Display Power Mode) through `esp_lcd_panel_io_rx_param` with the 32-bit form `{0x03, 0x00, command, 0x00}` returns 0xFF. Reading SIO0 in 3-wire mode, reading MISO, multi-byte reads with dummy bytes and a 1 MHz clock all returned 0xFF, so the panel is not driving either line. Do not base a display health check on register reads without proving one works first.
- Unverified: the CO5300 init sequence in the XiaoZhi board file sets a column window of 0x16 to 0x1AF (410 columns). This looks copied from the 2.06 inch board. Check it against the panel if the image is offset or clipped.
