#pragma once

// Copy this file to include/config.h and fill in your real values.

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Use the deployed Vercel 1bpp bitmap endpoint for Korean text and layout fidelity.
// Example: "https://your-app.vercel.app/api/screen.bin"
// Local testing from the ESP32 must use your Mac's LAN IP, not localhost:
// Example: "http://192.168.0.10:3000/api/screen.bin"
#define DEVICE_ENDPOINT "https://your-app.vercel.app/api/screen.bin"
#define DEVICE_AUTH_TOKEN "replace-with-the-same-token-as-eink-frontend"

// First upload/debug should stay false so Serial Monitor remains usable.
// Turn this on after the screen refresh works.
#define ENABLE_DEEP_SLEEP false
#define ENABLE_DISPLAY true
#define FALLBACK_SLEEP_SECONDS 1800
#define BOOT_TEST_SECONDS 0
#define DEBUG_HEARTBEAT_SECONDS 5
#define SCREEN_PAGE_COUNT 7
// Device setting defaults. Users can change these from the on-device settings
// menu; saved values are stored in ESP32 Preferences.
#define PAGE_FULL_REFRESH_INTERVAL 5
#define SETTINGS_REFRESH_SECONDS_MIN 300
#define SETTINGS_REFRESH_SECONDS_MAX 7200
#define WIFI_SETUP_TIMEOUT_SECONDS 300
#define WIFI_SETUP_MAX_NETWORKS 10
#define WIFI_BUTTON_PASSWORD_MAX_LENGTH 64
#define WIFI_BUTTON_SAVE_DOUBLE_PRESS_MS 900

// reTerminal E1001 top buttons from the official schematic.
#define ENABLE_BUTTONS true
#define BUTTON_LEFT_PIN 5
#define BUTTON_RIGHT_PIN 4
#define BUTTON_REFRESH_PIN 3
#define BUTTON_DEBOUNCE_MS 30
// Buttons are scanned as events instead of blocking waits. A short click is
// emitted when the button is released; left+right hold is emitted only after
// both buttons are down together for WIFI_SETUP_HOLD_MS.
#define BUTTON_SCAN_INTERVAL_MS 10
#define BUTTON_CLICK_MIN_MS 60
#define WIFI_SETUP_CHORD_GRACE_MS 700
#define WIFI_SETUP_HOLD_MS 1500

// reTerminal E1001 battery telemetry pins from the official schematic.
#define ENABLE_BATTERY_ADC true
#define BATTERY_ADC_PIN 1
#define BATTERY_ADC_ENABLE_PIN 21
#define BATTERY_VOLTAGE_MULTIPLIER 2.0f
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 3840

// reTerminal E1001 charger is connected over I2C1. Status is best-effort:
// if the charger does not answer, the firmware sends charge=unknown.
#define ENABLE_CHARGER_STATUS true
#define CHARGER_I2C_SDA 39
#define CHARGER_I2C_SCL 40
#define CHARGER_I2C_ADDRESS 0x6B
#define CHARGER_STATUS_REGISTER 0x0B
#define CHARGER_STATUS_SHIFT 3
#define CHARGER_STATUS_MASK 0x03

// 800 x 480 x 1bpp = 48,000 bytes.
#define MAX_IMAGE_BYTES 64000
#define DEVICE_FETCH_ATTEMPTS 3
#define DEVICE_HTTP_CONNECT_TIMEOUT_MS 15000
#define DEVICE_HTTP_TIMEOUT_MS 25000

// reTerminal E1001 screen pins from the official schematic.
#define EPD_BUSY 13
#define EPD_RST 12
#define EPD_DC 11
#define EPD_CS 10
#define EPD_SCK 7
#define EPD_MOSI 9

// 7.5 inch 800x480 black/white panel class used by reTerminal E1001.
#define EPD_MODEL GxEPD2_750_T7
