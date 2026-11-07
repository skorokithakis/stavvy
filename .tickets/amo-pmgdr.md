---
id: amo-pmgdr
status: closed
deps: []
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Project skeleton and board bring-up

Ready for implementation.

Objective: an ESP-IDF v5.5.3 project for target esp32c6 that boots on the board, powers the peripherals, joins Wi-Fi and advertises itself over mDNS. Read AGENTS.md first for the hardware facts and pin map.

Scope:
- Project layout (CMakeLists, main component, idf_component.yml). Build with Docker: espressif/idf:v5.5.3, mount the repo, run as the host user so build files are not root-owned. Add the exact build and flash commands to AGENTS.md.
- sdkconfig.defaults: 16 MB flash, QIO, custom partition table. Partition table: one large factory app partition (4 MB or more) and a data partition named 'model' (about 1 MB or more) for ESP-SR wake word models, which a later task uses. No OTA partitions.
- .gitignore: build/, sdkconfig, sdkconfig.old, managed_components/.
- Kconfig menu in main: Wi-Fi SSID and password (no defaults that look real).
- Board module: one shared I2C master bus (SDA GPIO8, SCL GPIO7) that other modules get a handle to. AXP2101 (I2C 0x34) setup at boot: reproduce the register writes in Waveshare's XiaoZhi board file (main/boards/waveshare/esp32-c6-touch-amoled-2.16/esp32-c6-touch-amoled-2.16.cc in https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16), which turn on ALDO1 to ALDO4 at 3.3 V and set the charger. Raw register writes are fine; do not add a PMIC library.
- Wi-Fi station with automatic reconnect.
- mDNS: hostname home-assistant-voice-XXXXXX (last 3 MAC bytes, lowercase hex), service _esphomelib._tcp on port 6053. The server finds the device by this hostname prefix.
- Log free heap and minimum free heap at boot and after Wi-Fi connects.

Non-goals: Wi-Fi provisioning, OTA, the API server itself, audio, display.

## Acceptance Criteria

Builds in the Docker image with no warnings from project code. Committed files contain no Wi-Fi credentials.

