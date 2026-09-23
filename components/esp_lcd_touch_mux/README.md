# ESP LCD Touch Mux

`esp_lcd_touch_mux` wraps a physical `esp_lcd_touch_handle_t` and presents one compatible handle that can replay injected touch frames or forward physical input.

The built-in wake backend sets the target GPIO interrupt-status W1TS register. It requires the consumer to register an interrupt callback through `esp_lcd_touch_register_interrupt_callback()` and supports targets exposing `GPIO_STATUS_W1TS_REG`. A project can provide a custom interrupt trigger in `esp_lcd_touch_mux_config_t` for other targets.

Injected coordinates use the same logical coordinate space returned by `esp_lcd_touch_get_data()` on the wrapped physical handle. The mux does not depend on LVGL, GSP, a display service, a fixed resolution, or a fixed GPIO.

Only one injected session may be active. A physical press prevents an injected session from starting, and physical reports are isolated while injection is active. Always finish with `esp_lcd_touch_mux_end()` or recover with `esp_lcd_touch_mux_cancel()`.

The caller controls ownership of the physical handle through `own_physical_handle`. Hardware validation is currently required per target even when the common register backend compiles successfully.

## Integration

Create the physical touch driver first, wrap its handle before registering an interrupt callback, and pass only the returned mux handle to every consumer. Register the existing interrupt callback on the mux handle through `esp_lcd_touch_register_interrupt_callback()` or `esp_lcd_touch_register_interrupt_callback_with_data()`.

```c
esp_lcd_touch_mux_config_t config = ESP_LCD_TOUCH_MUX_CONFIG_DEFAULT();
esp_lcd_touch_handle_t touch = NULL;
ESP_ERROR_CHECK(esp_lcd_touch_mux_new(physical_touch, &config, &touch));
```

An injected gesture is a `begin`, one or more complete frames, and `end`. `end` appends the release frame. Use `cancel` on every playback error path.

## Compatibility status

The independent test app in `test_apps/basic` compiles with ESP-IDF 6.2 for the targets below. Compilation verifies both low/high GPIO register branches where applicable; it is not a substitute for board validation of software-triggered GPIO interrupt delivery.

| Target | Compile | Hardware W1TS validation |
| --- | --- | --- |
| ESP32 | Passed | Not run |
| ESP32-S3 | Passed | Not run |
| ESP32-C3 | Passed | Not run |
| ESP32-C6 | Passed | Not run |
| ESP32-P4 | Passed | Not run |
| ESP32-S31 | Passed | Passed on esp_mosaico (GPIO6) |

Build the standalone test app with a target-specific sdkconfig path so one target does not overwrite another:

```sh
idf.py -B build_esp32s3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.esp32s3 build
```
