# ESP32-S31 Mosaico Subboard Support

This component owns the shared resources used by Mosaico expansion boards:

- Left and right slot EEPROM address selection.
- Shared I2C0 bus discovery and reuse.
- Shared 3.3 V subboard rail control.
- Left/right connector GPIO mapping.
- Camera DVP resource claim and release.
- Button/LED subboard pin resolution.
- USB Serial/JTAG pad arbitration for camera GPIO33.

Use `subboard_support/subboard.h` for the public API. Applications should
normally access these resources through a concrete subboard driver such as
`mosaico_camera` or through `hot_plug_register`.
