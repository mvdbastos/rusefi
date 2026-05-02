# ESP32 WiFi Bridge for rusEFI

This guide explains how to use an ESP32 module as a wireless bridge between your rusEFI ECU and TunerStudio. The ESP32 connects to the ECU's secondary UART and exposes it as a TCP server over WiFi, so TunerStudio can tune the engine without a USB cable.

```
[TunerStudio PC]  ←——  TCP port 17999  ——→  [ESP32 SoftAP]  ←——  UART  ——→  [STM32 ECU]
```

---

## Table of Contents

1. [Hardware Requirements](#1-hardware-requirements)
2. [Flashing AT Firmware onto the ESP32](#2-flashing-at-firmware-onto-the-esp32)
3. [Wiring the ESP32 to the ECU](#3-wiring-the-esp32-to-the-ecu)
4. [Enabling the Feature in Firmware](#4-enabling-the-feature-in-firmware)
5. [Configuring with TunerStudio](#5-configuring-with-tunerstudio)
6. [One-Time Initialization via Console](#6-one-time-initialization-via-console)
7. [Auto-Init on Every Power Cycle](#7-auto-init-on-every-power-cycle)
8. [Connecting TunerStudio over WiFi](#8-connecting-tunerstudio-over-wifi)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. Hardware Requirements

| Item | Notes |
|------|-------|
| ESP32 module | Any ESP32 with AT firmware support (e.g. ESP32-WROOM-32, ESP32-WROVER, AI-Thinker ESP-32S) |
| 3.3 V supply | Most ECUs provide this; the ESP32 **must not** be powered from 5 V logic directly |
| Level shifter (optional) | If the ECU UART pins are 5 V tolerant you may connect directly; otherwise use a 3.3 V–5 V level converter on TX/RX |
| 4-wire connection | GND, 3.3 V, UART TX, UART RX |

> **Proteus F7**: The secondary UART is already enabled and the feature flag `EFI_ESP32_WIFI` is set to `TRUE` in the board's `board.mk`. The TX/RX pins are configured through TunerStudio (`binarySerialTxPin` / `binarySerialRxPin`).

---

## 2. Flashing AT Firmware onto the ESP32

The rusEFI ESP32 driver speaks the **Espressif AT command set**. You must flash the official ESP-AT firmware before connecting the module to the ECU.

### 2a. Download the firmware

1. Go to [https://github.com/espressif/esp-at/releases](https://github.com/espressif/esp-at/releases)
2. Select the release matching your module (e.g. `ESP32-WROOM-32` → `ESP32-AT-V3.x.x.x.zip`)
3. Extract the archive — you will find a `factory` folder containing a single merged binary (e.g. `factory_WROOM-32.bin`) as well as individual component binaries.

### 2b. Flash with esptool.py (recommended)

```bash
# Install esptool if not already present
pip install esptool

# Flash the factory image (single-file, simplest approach)
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
    write_flash -z 0x0 factory/factory_WROOM-32.bin
```

Replace `/dev/ttyUSB0` with the correct port for your OS (`COMx` on Windows).

### 2c. Flash with Espressif Flash Download Tool (Windows GUI)

1. Download from [https://www.espressif.com/en/support/download/other-tools](https://www.espressif.com/en/support/download/other-tools)
2. Select **ESP32** chip, **UART** mode
3. Add the `factory_WROOM-32.bin` at address **0x0**
4. Set baud rate to **921600**, click **START**

### 2d. Verify the firmware

After flashing, open a serial terminal (115200 8N1) on the module's UART and type:

```
AT
```

You should receive:

```
AT

OK
```

The module is ready to be connected to the ECU.

---

## 3. Wiring the ESP32 to the ECU

Connect the ESP32 to the ECU's **secondary UART** (the same physical connector/pins used for Bluetooth modules).

| ESP32 pin | ECU pin | Notes |
|-----------|---------|-------|
| GND | GND | Common ground |
| 3V3 | 3.3 V | ECU-supplied 3.3 V rail |
| IO1 / TXD | Secondary RX (`binarySerialRxPin`) | ECU receives data from ESP32 |
| IO3 / RXD | Secondary TX (`binarySerialTxPin`) | ESP32 receives data from ECU |

> **Note**: TX and RX are **crossed** — the ECU's TX goes to the ESP32's RX and vice versa.

The exact pin labels (`binarySerialTxPin` / `binarySerialRxPin`) are configurable in TunerStudio under **Controller → Connection → Calibration Secondary Serial**.

---

## 4. Enabling the Feature in Firmware

### Boards that already enable ESP32 WiFi

| Board | Status |
|-------|--------|
| Proteus F7 | ✅ Enabled (`EFI_ESP32_WIFI=TRUE` in `board.mk`) |

### Enabling on other boards

Add the following line to the board's `board.mk`:

```makefile
DDEFS += -DEFI_ESP32_WIFI=TRUE
```

The default in `firmware/config/stm32f4ems/efifeatures.h` is `FALSE`, so **all boards without this line have the feature compiled out** (zero flash/RAM cost).

---

## 5. Configuring with TunerStudio

Open TunerStudio and navigate to **Controller → ESP32 WiFi Bridge**.

| Field | Description |
|-------|-------------|
| **Enable auto-init on startup** | When checked, the ECU will initialize the ESP32 on every power cycle using the stored credentials |
| **WiFi SSID / AP Name** | The name of the WiFi hotspot the ESP32 will create (max 32 characters) |
| **WiFi Password** | The password for that hotspot (max 64 characters; leave empty for an open/unprotected network) |

After editing, **burn the tune** to save the values to flash. On the next power cycle the ECU will automatically configure the ESP32 if *Enable auto-init* is checked.

---

## 6. One-Time Initialization via Console

You can also initialize the ESP32 manually from the rusEFI console (e.g. via USB or the primary UART).

### Prerequisite

The secondary UART TX/RX pins must be assigned in TunerStudio (**Controller → Connection → Calibration Secondary Serial**) and the tune must be burned to flash before issuing the command.

### Command syntax

```
esp32_wifi <ssid> <password>
```

**Examples:**

```
# WPA2-protected hotspot
esp32_wifi rusEFI mypassword123

# Open (passwordless) hotspot
esp32_wifi rusEFI ""
```

### What happens next

1. The credentials are stored in RAM.
2. The next time the secondary UART channel goes idle for ≥ 3 seconds (e.g. TunerStudio disconnects or was never connected), the AT-command sequence runs automatically.
3. The console will print progress messages:

```
*** ESP32 WiFi bridge setup procedure ***
Waiting for channel idle...
ESP32 TX: AT
ESP32 RX: OK
ESP32 TX: AT+CWMODE=2
...
ESP32: WiFi bridge ready! SSID='rusEFI' port=17999
```

> **Tip**: If TunerStudio is currently connected via USB, the secondary UART is already idle — the setup will run within 3 seconds of issuing the command.

---

## 7. Auto-Init on Every Power Cycle

When **Enable auto-init on startup** is checked in TunerStudio and the tune is burned:

1. On boot, `initEfiWithConfig()` calls `esp32WifiStart()` with the stored SSID and password.
2. The AT-command sequence runs once the secondary channel is idle (within 3 seconds of startup, before TunerStudio has had a chance to connect over USB).
3. The ESP32 is fully configured and ready by the time TunerStudio would try to connect wirelessly.

This means **no console command is needed after the initial setup** — just power on the ECU and connect TunerStudio.

---

## 8. Connecting TunerStudio over WiFi

After the ESP32 has been configured:

1. On your PC/tablet, open **WiFi settings** and connect to the SSID you configured (e.g. `rusEFI`).
2. Enter the password you configured.
3. Open TunerStudio and select **Other → Connect to ECU**.
4. Choose **TCP/IP** as the connection type.
5. Enter the following:
   - **Host**: `192.168.4.1` (default IP of the ESP32 in SoftAP mode)
   - **Port**: `17999`
6. Click **Connect**.

TunerStudio should connect and begin receiving data from the ECU. From this point the connection is identical to a USB connection — you can read/write tables, monitor live data, and burn tunes wirelessly.

---

## 9. Troubleshooting

### "ESP32: module not found - check wiring and baud rate"

The firmware tried 115200, 9600, 38400, and 57600 baud and received no `OK` response.

| Check | Action |
|-------|--------|
| Wiring | Confirm TX and RX are **crossed** (ECU TX → ESP32 RX, ECU RX → ESP32 TX) |
| Power | Confirm the ESP32 is powered (3.3 V, not 5 V) and the power LED is on |
| AT firmware | Confirm the ESP32 has AT firmware flashed (open a serial terminal and type `AT`) |
| Pin assignment | Confirm `binarySerialTxPin` and `binarySerialRxPin` are set in TunerStudio and the tune is burned |
| Feature flag | Confirm `EFI_ESP32_WIFI=TRUE` is in the board's `board.mk` and the firmware was recompiled |

---

### "ESP32: CWMODE failed" / "ESP32: CWSAP failed"

The module replied but rejected the SoftAP configuration command.

| Check | Action |
|-------|--------|
| AT firmware version | Ensure you are running official Espressif AT firmware ≥ v2.x — older versions have different command syntax |
| SSID length | SSID must be 1–32 characters with no special characters in early firmware versions |
| Password length | WPA2 password must be 8–64 characters; either leave it empty (open AP) or use a valid length |

---

### "ESP32: CIPSERVER failed"

The TCP server could not be started, usually because `CIPMUX=1` did not apply or the server is already running.

| Check | Action |
|-------|--------|
| Duplicate init | If the ESP32 was already configured, run `AT+CIPSERVER=0` in a serial terminal to stop the old server, then retry the console command |
| Module stuck | Power-cycle the ESP32 (or the whole ECU) and try again |

---

### TunerStudio cannot connect (TCP timeout)

| Check | Action |
|-------|--------|
| Connected to correct WiFi | Confirm your PC is on the `rusEFI` network, not your home router |
| IP address | The ESP32 SoftAP always uses `192.168.4.1` by default; confirm with `AT+CIFSR` in a terminal |
| Port | Port must be **17999** in TunerStudio |
| Setup completed | Check the rusEFI console log for `"WiFi bridge ready!"` — if absent, the setup did not complete |
| Firewall | Temporarily disable OS firewall / antivirus to rule out a blocked port |

---

### Setup runs but then TunerStudio disconnects immediately

| Check | Action |
|-------|--------|
| Baud rate mismatch | After setup, the ESP32 runs at its detected baud rate. Confirm `tunerStudioSerialSpeed` in TunerStudio matches |
| Single connection | The Espressif TCP server supports a limited number of simultaneous connections; ensure no other client is connected to port 17999 |
| Passthrough mode | If using a custom ESP32 sketch instead of official AT firmware, confirm it is in transparent passthrough mode after initialization |

---

### Debug output

Enable extra debug output by adding to your board's `board.mk`:

```makefile
DDEFS += -DEFI_ESP32_WIFI_DEBUG=TRUE
```

Every AT command sent and every line received will be printed to the rusEFI console, making it easy to diagnose communication issues.

---

## Reference

| Item | Value |
|------|-------|
| TCP port | 17999 |
| Default SoftAP IP | 192.168.4.1 |
| SSID max length | 32 characters |
| Password max length | 64 characters |
| Probed baud rates | 115200, 9600, 38400, 57600 |
| Idle timeout before init | 3 seconds |
| Feature flag | `EFI_ESP32_WIFI` |
| Console command | `esp32_wifi <ssid> <password>` |
| AT firmware source | https://github.com/espressif/esp-at/releases |
