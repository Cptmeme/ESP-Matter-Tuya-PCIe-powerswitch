
# Tuya PC Power Switch (Matter over Thread)

Matter-over-Thread firmware for a Tuya PC power-switch board (WT0132C6-S5 / ESP32-C6),
replacing the stock Tuya Wi-Fi module. Exposes a single **On/Off** endpoint:

- **On** taps the PC power button (Tuya DP1 write).
- **Off** is a no-op (the OS handles shutdown); the switch state always reflects the
  MCU's *sensed* PC power state, so a Windows shutdown shows up as Off on its own.

The Tuya MCU is on **UART0 (GPIO16/17)**; the ESP console is moved to USB-Serial-JTAG
so those pins are free (see `sdkconfig.defaults.esp32c6`). The Tuya serial handshake
(product / work-mode / network-status) and framing live in `main/tuya_driver.*`.

First flash must be wired over native USB (or with the MCU's TX line isolated), since
the MCU shares the UART0 programming pads. After that, Matter OTA works.

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for building and flashing.
