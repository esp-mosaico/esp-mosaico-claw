# Mosaico boot splash

This component owns the shared logo, the bootloader-to-application LCD
handoff, and both rendering paths.

- `bootloader/`: early non-OS panel initialization and splash rendering.
- `app/`: application fallback rendering when no bootloader handoff exists.
- `include/`: public API, generated logo data, and the handoff protocol.
- `assets/`: source Logo artwork.

The project registers this same directory through
`BOOTLOADER_EXTRA_COMPONENT_DIRS`; `CMakeLists.txt` selects the appropriate
source set using `BOOTLOADER_BUILD`.

Regenerate `include/mosaic_boot_logo.h` with `components/mosaico_boot_splash/tools/generate_boot_logo.py`
when changing `assets/mosaic_boot_logo.png`.
