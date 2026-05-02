/**
 * @file	esp32_wifi.h
 * @brief   ESP32 wireless bridge initialization via AT commands.
 *
 * The ESP32 is wired to the ECU's secondary UART and runs Espressif AT firmware
 * (or compatible software) to act as a UART-TCP proxy. rusEFI sends AT commands
 * once to configure it as a WiFi SoftAP and TCP server on port 17999, after which
 * TunerStudio can connect wirelessly over TCP.
 *
 * Architecture:
 *   [TunerStudio PC] <-- TCP:17999 --> [ESP32 WiFi] <-- UART --> [STM32 ECU]
 *
 * @author rusEFI contributors
 */

#pragma once
#include "global.h"
#include "tunerstudio_io.h"

// Silent-timeout before the ESP32 init sequence begins (same window as Bluetooth)
#define ESP32_WIFI_SILENT_TIMEOUT TIME_MS2I(3000)

/**
 * Request ESP32 WiFi bridge initialization using the UART channel.
 * Stores credentials and schedules the AT-command sequence to run after
 * the channel goes idle (same pattern as bluetoothStart()).
 *
 * Usage:   "esp32_wifi <ssid> <password>"
 * Example: "esp32_wifi rusEFI mypassword123"
 */
void esp32WifiStart(const char *ssid, const char *password);

/**
 * Called by tsProcessOne() when no data is received for a full timeout period
 * on the Bluetooth/secondary channel. Triggers the AT-command init sequence
 * if esp32WifiStart() was previously requested.
 */
void esp32WifiSoftwareDisconnectNotify(SerialTsChannelBase *tsChannel);
