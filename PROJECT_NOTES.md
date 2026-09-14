# PC Switch — Matter over Thread (project notes / handoff)

Firmware that replaces the stock Tuya Wi‑Fi module in a **Tuya PC power‑switch** board
(the kind that taps the motherboard power‑button header) with a native
**Matter‑over‑Thread** On/Off device on an **ESP32‑C6 (WT0132C6‑S5)** module.

Sister project: the Tuya heater at `../Thermostat_Tuya` (same approach, decoded first).

---

## 1. Status

- [x] Protocol decoded via ESPHome (see §3).
- [x] Matter firmware written in `main/` and **builds clean** → `build/pc_switch.bin`.
- [ ] Flashed to hardware and commissioned over Thread.
- [ ] End‑to‑end tested (On boots PC; state tracks real PC power).

---

## 2. Hardware & wiring

- Module: **WT0132C6‑S5** (ESP32‑C6), 4 MB flash.
- Tuya MCU is on the board's dedicated **RX/TX port = UART0 = GPIO16 (TX0) / GPIO17 (RX0)**.
  - `ESP TX GPIO16 → MCU RX`, `ESP RX GPIO17 ← MCU TX`. 9600 8N1.
  - If you ever see only outgoing `>>>` and no `<<<`, swap the two pins.
- Because the MCU sits on UART0 (also the console/programming UART):
  - `sdkconfig.defaults.esp32c6` sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` so the ESP
    console leaves GPIO16/17 alone (logs go over USB instead of the Tuya line).
  - **First flash must be wired over native USB** (USB‑Serial‑JTAG), OR temporarily
    isolate the MCU's TX wire, because the MCU shares the UART0 programming pads and
    would otherwise corrupt esptool. After the first flash, use **Matter OTA**.

---

## 3. Decoded Tuya protocol (this device)

- **Protocol version 3** — MCU frames use version byte `0x03` (the heater used `0x01`).
  Our outgoing frames use version `0x00`; the MCU accepts that.
- Product info: `{"p":"na90jncjlvw5t0ba","v":"1.0.0","m":0}`.

### Frame format
`55 AA <ver> <cmd> <len_hi> <len_lo> <payload…> <checksum>`
checksum = sum of all preceding bytes mod 256.

Commands used:
| cmd | dir | meaning |
|----|-----|---------|
| `0x00` | both | heartbeat (module pings; MCU replies `55 AA 03 00 00 01 01 04`) |
| `0x01` | →MCU | product query |
| `0x02` | →MCU | working‑mode query |
| `0x03` | →MCU | report network status (payload `0x04` = online) |
| `0x06` | →MCU | set datapoint |
| `0x07` | MCU→ | datapoint status report |
| `0x08` | →MCU | query all datapoints |
| `0x1C` | both | get local time (we answer "time not obtained", like ESPHome without a clock) |

### Datapoints
Only **DP1** seen:
- **DP1 (bool) — PC power. BIDIRECTIONAL.**
  - WRITE `1` → board simulates a momentary power‑button press (turns PC on).
    `>>> 55 AA 00 06 00 05 01 01 00 01 01 0E`
  - READ → the MCU's **sensed** actual PC power state
    (tracked a Windows shutdown → OFF and a physical‑button press → ON).
    `<<< 55 AA 03 07 00 05 01 01 00 01 01 12` (ON) / `…00 11` (OFF)
- Note: the PC is configured to **ignore button presses while running**, so writing
  DP1=0 taps the button but does not shut the PC down (intentional). Off is via the OS;
  DP1 still senses it.

---

## 4. Desired behavior (implemented)

Single Matter **On/Off Plug‑in Unit** endpoint = "PC Power":
- **On** → `PressPowerOn()` writes DP1=1 (tap button → boot). Skipped if the PC is already sensed on.
- **Off** → **no command sent**; the OnOff attribute reverts to the sensed state.
- State always mirrors the MCU‑sensed DP1, so a Windows shutdown shows as Off on its own.
  Every DP1 report (incl. the 60 s query) re‑syncs the attribute, so an On that didn't
  boot the PC reverts to Off within a minute (same as the ESPHome template switch).
- MCU restart (heartbeat reply `0x00` while running) → handshake is redone.

---

## 5. Code map (`main/`)

- `tuya_driver.h/.cpp` — class **`TuyaPcSwitch`**:
  - `Service()` (call ~every 50 ms): reads UART + runs the handshake/keep‑alive state machine
    `TY_HEARTBEAT → TY_PRODUCT → TY_CONF → TY_WIFI_STATUS → TY_QUERY → TY_RUNNING`
    (advances on MCU replies, retries, force‑advances after ~5 s so it can't stall).
  - `PressPowerOn()` → write DP1=1. No "power off" method by design.
  - Parses `0x07` reports → updates `pcswitch_state_t.power` → state callback on every DP1 report.
- `app_main.cpp` — creates the single `on_off_plug_in_unit` endpoint; standard Matter/Thread bringup.
- `app_driver.cpp` — glue:
  - Matter On → `PressPowerOn()` (unless already on); Matter Off → no‑op + `report()` revert to sensed.
  - DP1 report → `report()` OnOff = sensed power when the attribute differs (uses `report()`,
    not `update()`, so it never re‑triggers the write callback = no loop).
  - Poll task calls `pcsw.Service()`.
- `app_priv.h` — `TUYA_TX_PIN 16`, `TUYA_RX_PIN 17`.
- `CMakeLists.txt` — project name `pc_switch`.
- `sdkconfig.defaults.esp32c6` — Thread on, Wi‑Fi off, console on USB‑Serial‑JTAG.

---

## 6. Build & flash

### One‑time toolchain fix (needed now)
System Python went 3.13 → 3.14, so `export.sh` looks for a `idf5.3_py3.14_env` that
doesn't exist. Re‑provision the venv:
```bash
cd ~/esp/esp-idf && ./install.sh esp32c6
```
(The old `idf5.3_py3.13_env` still exists if you'd rather force Python 3.13 on PATH.)
Without reinstalling, this works (verified 2026-09-14, clean build):
```bash
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.3_py3.13_env
```

### Build / flash
```bash
source ~/esp/esp-idf/export.sh
source ~/.espressif/esp-matter/export.sh
cd ~/esp/PC_Switch_Tuya
idf.py -DIDF_TARGET=esp32c6 build           # (target already set once build/ exists)
idf.py -p <port> flash monitor              # first flash over native USB; then OTA
```
Default commissioning code / discriminator are the esp‑matter example defaults unless
you change them. Needs a Thread Border Router + the pairing code; it advertises over BLE.

---

## 7. Test checklist (in `idf.py monitor`)

1. Handshake reaches `TUYA_PCSW: Tuya handshake complete -> RUNNING`, product info logged.
2. Commission over Thread; endpoint appears as On/Off.
3. With PC off, Matter **On** → `>>> …06…01…` goes out, PC boots, DP1 reports ON.
4. Shut down PC via Windows → OnOff flips to Off on its own (sensed).
5. Matter **Off** while PC on → nothing sent, switch stays/reverts to On.

Watch the `Work-mode reply` log line: if it says *"MCU expects status on a GPIO"* and the
device shows offline / rejects commands, we need to add GPIO network‑status handling
(paste the two bytes and the fix is quick). *"reports status over serial"* = we're fine.

---

## 8. Open items / next steps

- Fix toolchain, flash, commission, run §7.
- Confirm the MCU accepts our writes and completes the handshake on real hardware.
- (Optional) set a friendlier Matter product name / unique discriminator if running both
  this and the heater on the same fabric.
- If a fresh `Datapoint …` shows up we didn't map (e.g. a reset/restart DP), add it.
