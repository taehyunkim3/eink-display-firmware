#pragma once

// Copy this file to include/config.h and fill in your real values.

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Use the deployed Vercel PNG endpoint for Korean text and layout fidelity.
// Example: "https://your-app.vercel.app/api/screen.png"
// Local testing from the ESP32 must use your Mac's LAN IP, not localhost:
// Example: "http://192.168.0.10:3000/api/screen.png"
#define DEVICE_ENDPOINT "https://your-app.vercel.app/api/screen.png"
#define DEVICE_AUTH_TOKEN "replace-with-the-same-token-as-eink-frontend"

// First upload/debug should stay false so Serial Monitor remains usable.
// Turn this on after the screen refresh works.
#define ENABLE_DEEP_SLEEP false
#define FALLBACK_SLEEP_SECONDS 1800

// Battery telemetry is board-specific. Leave disabled until the product
// schematic/wiki confirms the battery ADC pin and voltage divider ratio.
#define ENABLE_BATTERY_ADC false
#define BATTERY_ADC_PIN A0
#define BATTERY_VOLTAGE_MULTIPLIER 2.0f
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200

// Keep this comfortably above the PNG response size, but below available RAM.
#define MAX_IMAGE_BYTES 320000

// Default pin mapping for XIAO-style ePaper panels. If the display stays blank,
// check the product wiki/schematic and adjust these first.
#define EPD_BUSY D2
#define EPD_RST D0
#define EPD_DC D3
#define EPD_CS D1
#define EPD_SCK D8
#define EPD_MOSI D10

// Common 7.5 inch 800x480 black/white panel class in GxEPD2.
// If compilation says this class is unknown, try GxEPD2_750_T7.
#define EPD_MODEL GxEPD2_750_GDEY075T7
