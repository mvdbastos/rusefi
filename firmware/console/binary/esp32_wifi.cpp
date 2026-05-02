/**
 * @file	esp32_wifi.cpp
 * @brief   ESP32 wireless bridge initialization via AT commands.
 *
 * The ESP32 runs Espressif AT firmware and acts as a UART-TCP proxy.
 * rusEFI sends AT commands over the secondary UART to put the ESP32 into
 * SoftAP mode, configure the SSID/password, and start a TCP server on
 * port 17999 so that TunerStudio can connect wirelessly.
 *
 * Command flow (Espressif AT firmware):
 *   AT                         -> OK  (probe / verify module alive)
 *   AT+CWMODE=2                -> OK  (SoftAP mode)
 *   AT+CWSAP="ssid","pass",1,3 -> OK  (configure AP)
 *   AT+CIPMUX=1                -> OK  (allow multiple connections)
 *   AT+CIPSERVER=1,17999       -> OK  (open TCP server on port 17999)
 *
 * After this sequence the ESP32 transparently bridges any incoming TCP
 * connection to its UART, so TunerStudio traffic flows straight through.
 */

#include "pch.h"

#if EFI_ESP32_WIFI

#include "esp32_wifi.h"
#include "tunerstudio_io.h"

#include <stdio.h>
#include <string.h>

#ifndef EFI_ESP32_WIFI_DEBUG
#define EFI_ESP32_WIFI_DEBUG TRUE
#endif

// TCP port TunerStudio (EFI Analytics) uses for remote connections
#define ESP32_TUNER_STUDIO_PORT 17999

// Timeout waiting for each AT response
static const int esp32Timeout = TIME_MS2I(2500);

static volatile bool esp32SetupIsRequested = false;

// Credentials written by esp32WifiStart() (console thread) and read by
// runEsp32Commands() (TS thread via esp32WifiSoftwareDisconnectNotify).
// Access is guarded by the fact that esp32SetupIsRequested is only set to
// true after credentials are written, and the TS thread only reads them after
// observing esp32SetupIsRequested == true. This mirrors the same pattern used
// by bluetooth.cpp (see btSetupIsRequested there).
static char esp32Ssid[WIFI_SSID_SIZE + 1];
static char esp32Password[WIFI_PASSWORD_SIZE + 1];

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

static void esp32Write(TsChannelBase *tsChannel, const char *str) {
#if EFI_ESP32_WIFI_DEBUG
	efiPrintf("ESP32 TX: %s", str);
#endif
	tsChannel->write((const uint8_t *)str, strlen(str));
}

/**
 * Read one line (terminated by '\n') from the channel.
 * Returns the number of bytes read, or -1 on timeout.
 */
static int esp32ReadLine(TsChannelBase *tsChannel, char *buf, size_t maxLen) {
	size_t len = 0;
	do {
		if (len >= maxLen) {
			efiPrintf("ESP32: reply too long");
			return -1;
		}
		if (tsChannel->readTimeout((uint8_t *)&buf[len], 1, esp32Timeout) != 1) {
			efiPrintf("ESP32: timeout after %d byte(s)", (int)len);
			return -1;
		}
	} while (buf[len++] != '\n');

	if (len < maxLen) {
		buf[len] = '\0';
	} else {
		buf[maxLen - 1] = '\0';
	}

#if EFI_ESP32_WIFI_DEBUG
	efiPrintf("ESP32 RX: %s", buf);
#endif

	return (int)len;
}

/**
 * Drain lines until we see "OK\r\n" or "OK\n".
 * The ESP32 AT firmware may prefix the OK with the echoed command or blank
 * lines, so we keep reading until we hit OK or timeout.
 * Returns 0 on success, -1 on timeout/error.
 */
static int esp32WaitOk(TsChannelBase *tsChannel) {
	char tmp[64];
	// Read up to 8 lines looking for OK
	for (int i = 0; i < 8; i++) {
		int len = esp32ReadLine(tsChannel, tmp, sizeof(tmp));
		if (len < 0) {
			return -1;
		}
		// Accept "OK\r\n" or "OK\n"
		if (strncmp(tmp, "OK", 2) == 0) {
			return 0;
		}
		// Also accept "\r\nOK\r\n" style (some firmware wraps with blank line)
		if (strstr(tmp, "OK") != nullptr) {
			return 0;
		}
	}
	efiPrintf("ESP32: no OK after 8 lines");
	return -1;
}

// ---------------------------------------------------------------------------
// Main AT command sequence
// ---------------------------------------------------------------------------

static bool runEsp32Commands(SerialTsChannelBase *tsChannel) {
	char tmp[80 + WIFI_SSID_SIZE + WIFI_PASSWORD_SIZE];

	// Step 1 – probe module at several baud rates
	static const uint32_t probeBauds[] = { 115200, 9600, 38400, 57600 };
	bool found = false;
	for (size_t i = 0; i < efi::size(probeBauds) && !found; i++) {
		tsChannel->stop();
		chThdSleepMilliseconds(50);
		tsChannel->start(probeBauds[i]);
		chThdSleepMilliseconds(50);

		esp32Write(tsChannel, "AT\r\n");
		if (esp32WaitOk(tsChannel) == 0) {
			efiPrintf("ESP32: found module at %lu baud", probeBauds[i]);
			found = true;
		}
	}

	if (!found) {
		efiPrintf("ESP32: module not found - check wiring and baud rate");
		// Restore configured baud before returning
		tsChannel->stop();
		tsChannel->start(engineConfiguration->tunerStudioSerialSpeed);
		return false;
	}

	// Step 2 – SoftAP mode
	esp32Write(tsChannel, "AT+CWMODE=2\r\n");
	if (esp32WaitOk(tsChannel) != 0) {
		efiPrintf("ESP32: CWMODE failed");
		return false;
	}

	// Step 3 – configure SoftAP (channel 1, WPA2)
	// AT+CWSAP="ssid","password",channel,encryption
	//   encryption: 0=open, 2=WPA, 3=WPA2, 4=WPA/WPA2
	int pwdLen = strlen(esp32Password);
	bool openNetwork = (pwdLen == 0);

	if (openNetwork) {
		chsnprintf(tmp, sizeof(tmp), "AT+CWSAP=\"%s\",\"\",1,0\r\n", esp32Ssid);
	} else {
		chsnprintf(tmp, sizeof(tmp), "AT+CWSAP=\"%s\",\"%s\",1,3\r\n",
		           esp32Ssid, esp32Password);
	}
	esp32Write(tsChannel, tmp);
	if (esp32WaitOk(tsChannel) != 0) {
		efiPrintf("ESP32: CWSAP failed");
		return false;
	}

	// Step 4 – enable multiple connections (required for TCP server)
	esp32Write(tsChannel, "AT+CIPMUX=1\r\n");
	if (esp32WaitOk(tsChannel) != 0) {
		efiPrintf("ESP32: CIPMUX failed");
		return false;
	}

	// Step 5 – start TCP server on TunerStudio port
	chsnprintf(tmp, sizeof(tmp), "AT+CIPSERVER=1,%d\r\n", ESP32_TUNER_STUDIO_PORT);
	esp32Write(tsChannel, tmp);
	if (esp32WaitOk(tsChannel) != 0) {
		efiPrintf("ESP32: CIPSERVER failed");
		return false;
	}

	efiPrintf("ESP32: WiFi bridge ready! SSID='%s' port=%d", esp32Ssid, ESP32_TUNER_STUDIO_PORT);
	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void esp32WifiStart(const char *ssid, const char *password) {
	static const char *usage = "Usage: esp32_wifi <ssid> <password>";

	if (ssid == nullptr || password == nullptr) {
		efiPrintf("%s", usage);
		return;
	}

	if (getBluetoothChannel() == nullptr) {
		efiPrintf("ESP32: no UART channel available [%s]", getTsSignature());
		return;
	}

	if (esp32SetupIsRequested) {
		efiPrintf("ESP32: init already in progress");
		return;
	}

	size_t ssidLen = strlen(ssid);
	if (ssidLen < 1 || ssidLen > WIFI_SSID_SIZE) {
		efiPrintf("ESP32: SSID must be 1-%d characters. %s", WIFI_SSID_SIZE, usage);
		return;
	}

	// password may be empty (open network)
	if (strlen(password) > WIFI_PASSWORD_SIZE) {
		efiPrintf("ESP32: password too long (max %d chars). %s", WIFI_PASSWORD_SIZE, usage);
		return;
	}

	strncpy(esp32Ssid, ssid, WIFI_SSID_SIZE);
	esp32Ssid[WIFI_SSID_SIZE] = '\0';
	strncpy(esp32Password, password, WIFI_PASSWORD_SIZE);
	esp32Password[WIFI_PASSWORD_SIZE] = '\0';

	esp32SetupIsRequested = true;
}

void esp32WifiSoftwareDisconnectNotify(SerialTsChannelBase *tsChannel) {
	if (!esp32SetupIsRequested) {
		return;
	}

	efiPrintf("*** ESP32 WiFi bridge setup procedure ***");
	efiPrintf("Waiting for channel idle...");

	// Wait for the line to be truly quiet before issuing AT commands
	uint8_t tmp[1];
	if (tsChannel->readTimeout(tmp, 1, ESP32_WIFI_SILENT_TIMEOUT) != 0) {
		efiPrintf("ESP32: setup cancelled - channel still active");
		esp32SetupIsRequested = false;
		return;
	}

	runEsp32Commands(tsChannel);
	esp32SetupIsRequested = false;
}

#endif /* EFI_ESP32_WIFI */
