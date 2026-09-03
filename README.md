# ESP-MOSAICO-CLAW

ESP-MOSAICO-CLAW takes ESP-MOSAICO beyond connectivity and display, bringing AI agents, natural interaction, and rich peripheral capabilities to the ESP32-S31. Built on [ESP-Claw](https://github.com/espressif/esp-claw), this repository also carries a validated prebuilt ESP-Iris Factory Recovery bundle.

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
idf.py factory-bundle-validate
```

`ESP-Iris` and the repository-local Mosaico CLI are pinned as regular Git
submodules. Initialize them recursively before building firmware or operating
a device.

## Unified Mosaico Commands

Use the repository-level product CLI for routine host checks, device discovery,
Recovery, application installation, and retained logs:

```bash
python mosaico.py doctor
python mosaico.py list
python mosaico.py recover
python mosaico.py install
python mosaico.py monitor
```

The launcher delegates to the pinned `third-party/esp-mosaico-tools` checkout;
the CLI does not need to be installed globally. [`.mosaico.json`](.mosaico.json)
maps the Claw root application, board files, reviewed Recovery bundle, and
ESP-Iris checkout into that shared tool. To initialize only the CLI dependency,
run:

```bash
git submodule update --init third-party/esp-mosaico-tools
```

Run `doctor` first. It checks Python, ESP-IDF and ESP32-S31 support, ESP-Iris,
the host state directory, and currently visible USB devices without building or
writing firmware. `install` builds this root project and updates `ota_0` only
through the ESP-Iris Gateway. Before OTA begins, it compares the device's live
partition-table SHA-256 with the 4 KiB Flash-sector hash of the partition table
from the current application build and refuses mismatched layouts. `recover`
defaults to the checked-in, reviewed
Factory Recovery bundle; this repository intentionally does not offer an
unreviewed Recovery build, so use the default `--source reviewed` mode.

Recovery is local-only and obtains a maintenance lease from the local Gateway
before writing the bootloader, partition table, OTA data, and retained Factory
Recovery images. The command never erases the whole flash and does not write
credentials, device identity, application NVS, core dumps, or `ota_0`.

## Factory Provisioning

The normal Claw application belongs in `ota_0`; the retained `factory`
partition contains ESP-Iris Recovery. **Do not use `idf.py flash` or
`idf.py app-flash`**, because ESP-IDF would write `edge_agent.bin` into the
first app partition and overwrite Recovery.

For a new device or a partition-layout migration, stop the ESP-Iris Gateway,
put the intended device into ROM download mode if necessary, and run:

```bash
idf.py -p PORT factory-provision
```

This writes the bootloader, partition table, initial OTA data, Factory
Recovery, UI assets, and SYSTEM filesystem. It does not erase or write
`sysmeta`, application `nvs`, `coredump`, or `ota_0`. The device starts in
Factory Recovery afterward. Confirm the target port before running this
partition-specific operation.

## ESP-Iris Gateway, OTA, and Debug

Install the host-side Gateway in an isolated Python environment. Node.js/npm
is needed only to build the Web Workbench:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r third-party/esp-iris/components/esp_iris/tools/requirements.lock

cd third-party/esp-iris/components/esp_iris/tools/frontend
npm ci
npm run build
cd -
```

Start the Gateway. It automatically discovers ESP-Iris USB CDC0 devices and
serves the Workbench at `http://127.0.0.1:8443/`:

```bash
IRIS=third-party/esp-iris/components/esp_iris/tools/esp_iris.py
python "$IRIS" doctor
python "$IRIS" web
```

With the Gateway running in another terminal, discover the stable device ID
and install the built Claw application through Recovery:

```bash
python "$IRIS" ctl devices
python "$IRIS" ctl ota DEVICE_ID build/edge_agent.bin \
  --elf build/edge_agent.elf \
  --map build/edge_agent.map \
  --execution-mode recovery \
  --validation-mode elf_sha256 \
  --wait
```

To build and update `ota_0`, `ui_apps`, and `system` in one validated Recovery
transaction, use the unified command:

```bash
python mosaico.py system-update
```

Reuse a previously built default bundle, or select an explicit `.irisfw`
artifact, with:

```bash
python mosaico.py system-update --skip-build
python mosaico.py system-update \
  --bundle build/edge_agent-system-update.irisfw
```

The bundle authorizes only the fixed `ota_0`, `ui_apps`, and `system` ranges.
Recovery verifies every component SHA-256 and Flash readback before selecting
`ota_0`. It requires Factory Recovery `2.3.0-recovery` or newer. Restart the
Gateway after updating the ESP-Iris submodule so its 1 GiB artifact request
limit is active.

Advanced deployments may keep Recovery-side HTTP(S) or NAND pull mode by using
`--manifest-url` or `--manifest-path`. The native ESP-Iris `ctl system-update`
command remains available for lower-level Gateway testing.

Common diagnostics are available through the same native CLI:

```bash
python "$IRIS" ctl status DEVICE_ID
python "$IRIS" ctl logs --device DEVICE_ID --follow
python "$IRIS" ctl crash DEVICE_ID
python "$IRIS" ctl coredump DEVICE_ID device.coredump
python "$IRIS" ctl factory DEVICE_ID
```

USB CDC0 carries the framed ESP-Iris protocol rather than a text console.
Claw keeps its interactive ESP-IDF console on UART. Holding the physical Boot
AI button (GPIO7, active low) during reset selects Factory Recovery for that boot
without changing OTA data.

## Repository Layout

```text
boards/                  ESP-MOSAICO board definitions and driver adaptations
components/              UI, system services, and product components
fatfs_image/             Resources embedded in the SYSTEM filesystem
main/                    Application entry point and runtime service wiring
third-party/esp-claw/    ESP-Claw Git submodule
third-party/esp-iris/    Pinned ESP-Iris component, Gateway, and Workbench
prebuilt/recovery/       Reviewed Factory Recovery provisioning images
tools/                   Build helpers and CMake utilities
```
