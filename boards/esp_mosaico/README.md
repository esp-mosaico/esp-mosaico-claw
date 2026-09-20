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
