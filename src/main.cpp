#include <Arduino.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <new>

#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <GxEPD2_BW.h>

#include "config.h"

GxEPD2_BW<EPD_MODEL, EPD_MODEL::HEIGHT> display(
    EPD_MODEL(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

PNG png;
static uint16_t lineBuffer[800];

struct DeviceTelemetry {
  String ssid;
  int32_t rssi;
  int32_t batteryPercent;
  float batteryVoltage;
};

static bool isHttpsEndpoint() {
  String endpoint = DEVICE_ENDPOINT;
  endpoint.toLowerCase();
  return endpoint.startsWith("https://");
}

static String urlEncode(const String &value) {
  String encoded;
  const char *hex = "0123456789ABCDEF";

  for (size_t i = 0; i < value.length(); i++) {
    const char c = value.charAt(i);
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';

    if (safe) {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0f];
      encoded += hex[c & 0x0f];
    }
  }

  return encoded;
}

static float readBatteryVoltage() {
  if (!ENABLE_BATTERY_ADC) {
    return -1.0f;
  }

  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  delay(5);

  uint32_t total = 0;
  constexpr uint8_t sampleCount = 8;
  for (uint8_t i = 0; i < sampleCount; i++) {
    total += analogRead(BATTERY_ADC_PIN);
    delay(2);
  }

  const float raw = static_cast<float>(total) / sampleCount;
  const float pinVoltage = raw / 4095.0f * 3.3f;
  return pinVoltage * BATTERY_VOLTAGE_MULTIPLIER;
}

static int32_t batteryPercentFromVoltage(float voltage) {
  if (voltage <= 0) {
    return -1;
  }

  const float millivolts = voltage * 1000.0f;
  const float percent =
      (millivolts - BATTERY_EMPTY_MV) * 100.0f / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
  return constrain(static_cast<int32_t>(roundf(percent)), 0, 100);
}

static DeviceTelemetry readDeviceTelemetry() {
  const float voltage = readBatteryVoltage();

  return {
      WiFi.SSID(),
      WiFi.RSSI(),
      batteryPercentFromVoltage(voltage),
      voltage,
  };
}

static String endpointWithTelemetry(const DeviceTelemetry &telemetry) {
  String endpoint = DEVICE_ENDPOINT;
  endpoint += endpoint.indexOf('?') >= 0 ? '&' : '?';
  endpoint += "wifi=connected";
  endpoint += "&ssid=" + urlEncode(telemetry.ssid);
  endpoint += "&rssi=" + String(telemetry.rssi);

  if (telemetry.batteryPercent >= 0) {
    endpoint += "&battery=" + String(telemetry.batteryPercent);
    endpoint += "&batteryVoltage=" + String(telemetry.batteryVoltage, 2);
  }

  return endpoint;
}

static void drawStatus(const String &title, const String &detail) {
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(32, 72);
    display.print(title);
    display.setFont(&FreeMono9pt7b);
    display.setCursor(32, 112);
    display.print(detail);
  } while (display.nextPage());
}

static void sleepOrWait(uint32_t seconds) {
  if (ENABLE_DEEP_SLEEP) {
    Serial.printf("Deep sleep for %lu seconds\n", static_cast<unsigned long>(seconds));
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.println("Deep sleep disabled. Waiting before next refresh.");
  delay(seconds * 1000UL);
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting Wi-Fi: %s", WIFI_SSID);
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed");
    return false;
  }

  Serial.print("Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

static bool fetchPng(const String &endpoint, std::unique_ptr<uint8_t[]> &buffer, int &size) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;

  if (isHttpsEndpoint()) {
    // Vercel certificates rotate; keep first setup simple. For stricter security,
    // replace this with secureClient.setCACert(...).
    secureClient.setInsecure();
  }

  WiFiClient &client = isHttpsEndpoint()
                           ? static_cast<WiFiClient &>(secureClient)
                           : static_cast<WiFiClient &>(plainClient);

  Serial.printf("GET %s\n", endpoint.c_str());
  if (!http.begin(client, endpoint)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + DEVICE_AUTH_TOKEN);
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("HTTP failed: %d\n", statusCode);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  Serial.printf("PNG content length: %d\n", contentLength);
  if (contentLength <= 0 || contentLength > MAX_IMAGE_BYTES) {
    Serial.println("PNG response is missing length or exceeds MAX_IMAGE_BYTES");
    http.end();
    return false;
  }

  buffer.reset(new (std::nothrow) uint8_t[contentLength]);
  if (!buffer) {
    Serial.println("PNG buffer allocation failed");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int offset = 0;
  while (http.connected() && offset < contentLength) {
    const int available = stream->available();
    if (available <= 0) {
      delay(1);
      continue;
    }

    const int chunkSize = min(available, contentLength - offset);
    const int readBytes = stream->readBytes(buffer.get() + offset, chunkSize);
    if (readBytes <= 0) {
      break;
    }
    offset += readBytes;
  }

  http.end();

  if (offset != contentLength) {
    Serial.printf("PNG download incomplete: %d/%d\n", offset, contentLength);
    return false;
  }

  size = contentLength;
  return true;
}

static int pngDraw(PNGDRAW *draw) {
  png.getLineAsRGB565(draw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

  for (int x = 0; x < draw->iWidth; x++) {
    const uint16_t color = lineBuffer[x];
    const uint8_t r = ((color >> 11) & 0x1f) << 3;
    const uint8_t g = ((color >> 5) & 0x3f) << 2;
    const uint8_t b = (color & 0x1f) << 3;
    const uint16_t luminance = (r * 30 + g * 59 + b * 11) / 100;
    display.drawPixel(x, draw->y, luminance < 170 ? GxEPD_BLACK : GxEPD_WHITE);
  }

  return 1;
}

static bool renderPng(const uint8_t *buffer, int size) {
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();

  do {
    display.fillScreen(GxEPD_WHITE);
    const int result = png.openRAM(const_cast<uint8_t *>(buffer), size, pngDraw);
    if (result != PNG_SUCCESS) {
      Serial.printf("PNG open failed: %d\n", result);
      png.close();
      return false;
    }

    const int decoded = png.decode(nullptr, 0);
    png.close();
    if (decoded != PNG_SUCCESS) {
      Serial.printf("PNG decode failed: %d\n", decoded);
      return false;
    }
  } while (display.nextPage());

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("ESP32 e-ink dashboard firmware");

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);

  if (!connectWifi()) {
    drawStatus("Wi-Fi failed", "Check WIFI_SSID / WIFI_PASSWORD");
    sleepOrWait(FALLBACK_SLEEP_SECONDS);
    return;
  }

  const DeviceTelemetry telemetry = readDeviceTelemetry();
  Serial.printf("Wi-Fi SSID: %s, RSSI: %ld dBm\n",
                telemetry.ssid.c_str(),
                static_cast<long>(telemetry.rssi));
  if (telemetry.batteryPercent >= 0) {
    Serial.printf("Battery: %ld%% %.2fV\n",
                  static_cast<long>(telemetry.batteryPercent),
                  telemetry.batteryVoltage);
  } else {
    Serial.println("Battery telemetry disabled");
  }

  std::unique_ptr<uint8_t[]> pngBuffer;
  int pngSize = 0;
  if (!fetchPng(endpointWithTelemetry(telemetry), pngBuffer, pngSize)) {
    drawStatus("Fetch failed", "Check endpoint, token, and Vercel logs");
    sleepOrWait(FALLBACK_SLEEP_SECONDS);
    return;
  }

  if (!renderPng(pngBuffer.get(), pngSize)) {
    drawStatus("Render failed", "Check GxEPD2 model and panel pins");
    sleepOrWait(FALLBACK_SLEEP_SECONDS);
    return;
  }

  Serial.println("Screen updated");
  sleepOrWait(FALLBACK_SLEEP_SECONDS);
}

void loop() {}
