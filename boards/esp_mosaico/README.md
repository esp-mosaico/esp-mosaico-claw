# ESP-Mosaico Hardware Overview

ESP-Mosaico is an **ESP32-S31** development board. The hardware revision is stored in eFuse `USER_DATA` and must be detected before peripheral initialization.

## Onboard Hardware

| Function | Device / specification |
| --- | --- |
| Display | CO5300, 480 × 480, QSPI, RGB565 |
| Touch | CST9217 capacitive touch controller |
| Audio | ES8311 with onboard microphone and speaker amplifier |
| Motion sensing | BMI270 6-axis IMU |
| Magnetic sensing | 2 × BMM150 |
| Battery monitoring | BQ27220 fuel gauge |
| Storage | SPI NAND flash |
| Input / feedback | AI and BOOT buttons, vibration motor; status LED on v1.0 only |
| Interfaces | USB OTG and two hot-pluggable expansion slots |

There is no onboard camera or SD card. A camera module can be connected through the left expansion slot.

The product cell is rated 3.7 V nominal / 65 mAh. Application gauge startup
preserves learned FCC when the stored design capacity matches. The BQ27220
driver is resolved through the BSP and used without a build-local source
replacement.

Qualified charge termination synchronizes remaining capacity with FCC (CSYNC).
`CONFIG_MOSAICO_BATTERY_OVERRIDE_CHARGE_TERMINATION` defaults off: nominal cell
voltage/capacity alone do not establish Charging Voltage, Taper Current or Taper
Voltage. Enable it only with characterized cell/charger values and charge-cycle
validation; the existing gauge thresholds are retained otherwise.

## Main Buses and GPIOs

All I2C addresses below are 7-bit addresses.

| Function | GPIO / address |
| --- | --- |
| Mainboard I2C0 | v1.0: SDA 0, SCL 1; v1.1/v1.2: SDA 56, SCL 3 |
| Touch | Mainboard I2C, INT 6 |
| ES8311 | I2C `0x19`; MCLK 54, BCLK 37, WS 49, DOUT 52, DIN 40, PA_EN 45 |
| BMI270 | I2C `0x69`, INT 2 |
| BMM150 | I2C `0x11` and `0x12`, shared INT 2 |
| BQ27220 | I2C `0x55` |
| LCD QSPI (SPI2) | CS 50, D0 36, D1 51, D2 35, D3 9, TE 43 |
| LCD clock / reset | v1.0: SCLK 44, RST 42; v1.1/v1.2: SCLK 42, RST 44 |
| SPI NAND (SPI3) | CLK 20, MOSI 21, MISO 22, CS 23, HOLD 24, WP 25 |
| Buttons / motor | AI 7 and BOOT 61 (active-low); motor 8 (active-high) |
| Power control | 3.3 V rail 60 (active-low), shutdown 57 (open-drain, active-low); v1.0 codec rail 56 (active-high) |
| Status LED | v1.0 only: GPIO3, active-low |

## Expansion Slots

- Both slots use SDA 0 and SCL 1. They share mainboard I2C0 on v1.0; v1.1/v1.2 use dedicated I2C1.
- The six general-purpose signals are `53, 48, 13, 12, 14, 4` on the left slot and `46, 47, 11, 10, 39, 5` on the right slot. The right slot is physically rotated by 180°.
- Standard module EEPROM addresses are `0x50` on the left (GPIO14=0) and `0x51` on the right (GPIO39=1).
- The slots share I2C and other multiplexed resources. A module manager must provide exclusive ownership. Camera modules are supported only in the left slot, and some DVP signals conflict with USB Serial/JTAG.

### Slot GPIO mapping

Expansion-module drivers use the left slot as the canonical pin definition and map signals to the right slot as follows. SDA and SCL are not mirrored because both slots share the same I2C bus.
When using the IO of the expansion module, you need to first confirm where the module is connected, and then determine the corresponding IO based on the position.

| Module signal index | Left slot | Right slot |
| --- | --- | --- |
| H2 | GPIO53 | GPIO46 |
| H4 | GPIO48 | GPIO47 |
| H6 | GPIO13 | GPIO11 |
| H8 | GPIO12 | GPIO10 |
| H10 / standard EEPROM A0 | GPIO14 | GPIO39 |
| H12 | GPIO4 | GPIO5 |
| Extended 1 | GPIO16 | GPIO40 |
| Extended 2 | GPIO15 | GPIO38 |
| Extended 3 | GPIO17 | GPIO37 |
| Extended 4 | GPIO18 | GPIO54 |
| Extended 5 | GPIO19 | GPIO52 |
| Extended 6 | GPIO55 | GPIO49 |
| I2C SDA | GPIO0 | GPIO0 |
| I2C SCL | GPIO1 | GPIO1 |

### Supported expansion modules

Standard modules carry an AT24C02 descriptor EEPROM. Unless stated otherwise, the EEPROM is addressed at `0x50` in the left slot and `0x51` in the right slot. The following modules have concrete drivers in the ESP-Mosaico BSP revision used by this project; other EEPROM board-type values are identifiers only and do not imply runtime driver support.

#### Camera module

Provides DVP image capture using an OV3640 or SC101IOT sensor, sensor power-down control, reset, and an active-low flash LED. It is supported in the left slot only because the DVP bus includes dedicated core-board routes that do not have a usable right-slot equivalent. Its descriptor EEPROM has a fixed address of `0x50` and does not use an A0 selection signal.

| Signal | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| SCCB SDA | Camera control bus | GPIO0 | Not supported |
| SCCB SCL | Camera control bus | GPIO1 | Not supported |
| D0 | DVP data bit 0 | GPIO16 | Not supported |
| D1 | DVP data bit 1 | GPIO15 | Not supported |
| D2 | DVP data bit 2 | GPIO33 | Not supported |
| D3 | DVP data bit 3 | GPIO4 | Not supported |
| D4 | DVP data bit 4 | GPIO14 | Not supported |
| D5 | DVP data bit 5 | GPIO12 | Not supported |
| D6 | DVP data bit 6 | GPIO18 | Not supported |
| D7 | DVP data bit 7 | GPIO13 | Not supported |
| VSYNC | Frame synchronization | GPIO55 | Not supported |
| HREF / DE | Line/data enable | GPIO19 | Not supported |
| PCLK | Pixel clock | GPIO17 | Not supported |
| RESET | Sensor reset | GPIO53 | Not supported |
| PWDN | Sensor power-down, high to sleep | GPIO48 | Not supported |
| FLASH | Flash LED, active-low | GPIO34 | Not supported |
| Descriptor EEPROM | Fixed AT24C02 address | `0x50` | Not supported |

#### Interaction module

Provides two mechanical or capacitive-touch inputs, a PIR motion sensor, an LDR ambient-light input, six WS2812 RGB LEDs, and a 38 kHz NEC infrared transmitter.

| Signal | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| LDR | Ambient-light ADC input | GPIO53 | GPIO46 |
| IR | Infrared transmitter output | GPIO48 | GPIO47 |
| KEY_L | Left key / touch input | GPIO13 | GPIO11 |
| KEY_R | Right key / touch input | GPIO12 | GPIO10 |
| PIR | Motion input | GPIO4 | GPIO5 |
| WS2812 | Six-LED data output | GPIO15 | GPIO38 |
| EEPROM A0 | Slot address selection | GPIO14, low | GPIO39, high |
| Descriptor EEPROM | AT24C02 | `0x50` | `0x51` |

#### Joystick module

Provides a two-axis analog joystick and five digital buttons named B, 1, 2, 3, and 4. Button B is active-high; buttons 1–4 are active-low.

| Signal | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| X | Joystick X-axis ADC input | GPIO48 | GPIO47 |
| Y | Joystick Y-axis ADC input | GPIO53 | GPIO46 |
| B | Joystick press button, active-high | GPIO16 | GPIO40 |
| Button 1 | Digital input, active-low | GPIO4 | GPIO5 |
| Button 2 | Digital input, active-low | GPIO15 | GPIO38 |
| Button 3 | Digital input, active-low | GPIO12 | GPIO10 |
| Button 4 | Digital input, active-low | GPIO13 | GPIO11 |
| EEPROM A0 | Slot address selection | GPIO14, low | GPIO39, high |
| Descriptor EEPROM | AT24C02 | `0x50` | `0x51` |

#### ToF distance module

Provides laser time-of-flight ranging. The standard hardware uses a VL53L4CDV0DH/1; VL53L0CXV0DH/1 is also supported. Both sensors start at I2C address `0x29`, and the BSP selects the driver from the detected silicon ID. Two modules can be used together only when the application holds one sensor in XSHUT and assigns a unique address to the other before releasing it.

| Signal | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| SDA | Sensor and EEPROM I2C data | GPIO0 | GPIO0 |
| SCL | Sensor and EEPROM I2C clock | GPIO1 | GPIO1 |
| XSHUT | Sensor shutdown/reset | GPIO48 | GPIO47 |
| INT | Sensor interrupt | Not used | Not used |
| ToF sensor | VL53L4CD or VL53L0X | `0x29` by default | `0x29` by default |
| EEPROM A0 | Slot address selection | GPIO14, low | GPIO39, high |
| Descriptor EEPROM | AT24C02 | `0x50` | `0x51` |

#### Thermal imaging module

Provides a 32 × 24 contactless temperature image using an MLX90640. The sensor has a fixed I2C address of `0x33`, so only one thermal module may be used on the shared expansion bus at a time.

| Signal | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| SDA | MLX90640 and EEPROM I2C data | GPIO0 | GPIO0 |
| SCL | MLX90640 and EEPROM I2C clock | GPIO1 | GPIO1 |
| Thermal sensor | MLX90640, fixed address | `0x33` | `0x33` |
| EEPROM A0 | Slot address selection | GPIO14, low | GPIO39, high |
| Descriptor EEPROM | AT24C02 | `0x50` | `0x51` |

#### Matrix LED module

Provides an 8 × 8 matrix of 64 WS2812-compatible RGB LEDs. Pixels use GRB ordering and are driven as one serial chain through RMT.

| Signal | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| LED data | 64-LED WS2812 serial output | GPIO48 | GPIO47 |
| EEPROM A0 | Slot address selection | GPIO14, low | GPIO39, high |
| Descriptor EEPROM | AT24C02 | `0x50` | `0x51` |

#### Relay module

Provides four active-high relay-control channels. The driver presets every channel low before enabling its output, so all relays start in the off state. The GPIO level confirms only the control signal; the module has no relay-contact feedback.

| Channel | Hardware / function | Left slot | Right slot |
| --- | --- | --- | --- |
| CH1 | Relay 1 control, active-high | GPIO48 | GPIO47 |
| CH2 | Relay 2 control, active-high | GPIO53 | GPIO46 |
| CH3 | Relay 3 control, active-high | GPIO13 | GPIO11 |
| CH4 | Relay 4 control, active-high | GPIO12 | GPIO10 |
| EEPROM A0 | Slot address selection | GPIO14, low | GPIO39, high |
| Descriptor EEPROM | AT24C02 | `0x50` | `0x51` |
