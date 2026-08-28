# ESP-MOSAICO-CLAW

ESP-MOSAICO-CLAW takes ESP-MOSAICO beyond connectivity and display, bringing AI agents, natural interaction, and rich peripheral capabilities to the ESP32-S31. Built on [ESP-Claw](https://github.com/espressif/esp-claw), this repository also contains the source code for the ESP-MOSAICO factory firmware.

## Features

- Integrates the ESP-Claw Agent Runtime with conversations, memory, skills, Lua extensions, and MCP client/server support.
- Provides a Mosaic UI optimized for the 480 × 480 round touchscreen, with settings, weather, camera, album, and Bluetooth audio applications.
- Supports Wi-Fi provisioning, web-based configuration, instant messaging, speech recognition, and firmware update checks.
- Integrates ESP-MOSAICO hardware including audio, camera, IMU, magnetometer, battery management, and external storage.

## Quick Start

Install Python 3 and an ESP-IDF 6.x environment with ESP32-S31 support. Then run the following commands from the repository root:

```bash
source /path/to/esp-idf/export.sh
python -m pip install -r requirements.txt
git submodule update --init --recursive
idf.py bmgr -c ./boards -b esp_mosaico
idf.py build
```

Connect the device, then flash the firmware and open the serial monitor:

```bash
idf.py -p PORT flash monitor
```

## Repository Layout

```text
boards/                  ESP-MOSAICO board definitions and driver adaptations
components/              UI, system services, and product components
fatfs_image/             Resources embedded in the SYSTEM filesystem
main/                    Application entry point and runtime service wiring
third-party/esp-claw/    ESP-Claw Git submodule
tools/                   Build helpers and CMake utilities
```
