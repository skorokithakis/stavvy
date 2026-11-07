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

## Gotchas

- **Console is USB only.** ESP-IDF's default UART0 console uses GPIO16/GPIO17, which are the IMU INT1/INT2 lines here. `sdkconfig.defaults` moves the console to the USB-Serial-JTAG port. An existing `sdkconfig` overrides `sdkconfig.defaults`, so after changing defaults, change `sdkconfig` too or delete it (you then have to set Wi-Fi credentials again).

- **Frame buffer does not fit in RAM.** One 480 × 480 frame at 16 bits per pixel is about 460 KB, nearly all of the 512 KB SRAM. Draw in partial strips (for example, LVGL partial render mode with a buffer of a few tens of lines). Full-screen animation and video will be slow.
- **Waveshare's ESP-IDF and Arduino examples swap two pins.** Their `user_config.h` sets LCD CS = GPIO5 and touch INT = GPIO15. The schematic and Waveshare's XiaoZhi board config (`main/boards/waveshare/esp32-c6-touch-amoled-2.16/config.h`) use LCD CS = GPIO15 and touch INT = GPIO5. Trust the schematic.
- **SD card and display share one SPI bus** (GPIO0 to GPIO2). Use one SPI host with two devices, and do not access both at the same time without bus locking.
- **The AXP2101 must be set up before peripherals work.** The Waveshare examples enable its ALDO rails at boot. The display, microphones and other parts may not have power until then.
- Unverified: the CO5300 init sequence in the XiaoZhi board file sets a column window of 0x16 to 0x1AF (410 columns). This looks copied from the 2.06 inch board. Check it against the panel if the image is offset or clipped.
