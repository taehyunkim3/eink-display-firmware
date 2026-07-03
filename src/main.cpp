#include <Arduino.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
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

constexpr int SCREEN_WIDTH = 800;
constexpr int SCREEN_HEIGHT = 480;
constexpr int SCREEN_BYTES_PER_ROW = SCREEN_WIDTH / 8;
constexpr int SCREEN_BITMAP_BYTES = SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT;
static const char *deviceState = "booting";
static int lastErrorCode = 0;
static bool displayInitialized = false;
RTC_DATA_ATTR static int screenPage = 0;

static void setupDisplay();

struct DeviceTelemetry {
  String ssid;
  int32_t rssi;
  int32_t batteryPercent;
  float batteryVoltage;
};

class MemoryWriteStream : public Stream {
public:
  MemoryWriteStream(uint8_t *target, size_t capacity) : target_(target), capacity_(capacity) {}

  size_t write(uint8_t value) override {
    if (size_ >= capacity_) {
      overflowed_ = true;
      return 0;
    }

    target_[size_++] = value;
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    const size_t writable = min(size, capacity_ - size_);
    if (writable > 0) {
      memcpy(target_ + size_, buffer, writable);
      size_ += writable;
    }

    if (writable < size) {
      overflowed_ = true;
    }

    return writable;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t size() const { return size_; }
  bool overflowed() const { return overflowed_; }

private:
  uint8_t *target_;
  size_t capacity_;
  size_t size_ = 0;
  bool overflowed_ = false;
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

static String htmlEscape(const String &value) {
  String escaped;
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value.charAt(i);
    if (c == '&') {
      escaped += F("&amp;");
    } else if (c == '<') {
      escaped += F("&lt;");
    } else if (c == '>') {
      escaped += F("&gt;");
    } else if (c == '"') {
      escaped += F("&quot;");
    } else {
      escaped += c;
    }
  }
  return escaped;
}

static String storedWifiSsid() {
  Preferences preferences;
  preferences.begin("wifi", false);
  const String ssid = preferences.getString("ssid", "");
  preferences.end();
  return ssid;
}

static String storedWifiPassword() {
  Preferences preferences;
  preferences.begin("wifi", false);
  const String password = preferences.getString("password", "");
  preferences.end();
  return password;
}

static void saveWifiCredentials(const String &ssid, const String &password) {
  Preferences preferences;
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
}

static float readBatteryVoltage() {
  if (!ENABLE_BATTERY_ADC) {
    return -1.0f;
  }

  pinMode(BATTERY_ADC_ENABLE_PIN, OUTPUT);
  digitalWrite(BATTERY_ADC_ENABLE_PIN, HIGH);
  delay(10);

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
  digitalWrite(BATTERY_ADC_ENABLE_PIN, LOW);
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
  endpoint += "&page=" + String(screenPage);

  if (telemetry.batteryPercent >= 0) {
    endpoint += "&battery=" + String(telemetry.batteryPercent);
    endpoint += "&batteryVoltage=" + String(telemetry.batteryVoltage, 2);
  }

  return endpoint;
}

static String endpointWithTelemetry(const DeviceTelemetry &telemetry, bool forceServerRefresh) {
  String endpoint = endpointWithTelemetry(telemetry);
  if (forceServerRefresh) {
    endpoint += "&force=1";
    endpoint += "&nonce=" + String(millis());
  }
  return endpoint;
}

static void setupButtons() {
  if (!ENABLE_BUTTONS) {
    return;
  }

  pinMode(BUTTON_LEFT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_RIGHT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_REFRESH_PIN, INPUT_PULLUP);
}

static bool buttonPressed(int pin) {
  if (!ENABLE_BUTTONS) {
    return false;
  }

  if (digitalRead(pin) != LOW) {
    return false;
  }

  delay(30);
  return digitalRead(pin) == LOW;
}

static bool bothPageButtonsPressed() {
  if (!ENABLE_BUTTONS) {
    return false;
  }

  if (digitalRead(BUTTON_LEFT_PIN) != LOW || digitalRead(BUTTON_RIGHT_PIN) != LOW) {
    return false;
  }

  delay(30);
  return digitalRead(BUTTON_LEFT_PIN) == LOW && digitalRead(BUTTON_RIGHT_PIN) == LOW;
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

static void drawBootTest() {
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
    display.drawRect(8, 8, display.width() - 16, display.height() - 16, GxEPD_BLACK);
    display.drawLine(0, 0, display.width() - 1, display.height() - 1, GxEPD_BLACK);
    display.drawLine(display.width() - 1, 0, 0, display.height() - 1, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(32, 64);
    display.print("ESP32 e-ink dashboard");
    display.setFont(&FreeMono9pt7b);
    display.setCursor(32, 104);
    display.print("Display boot test");
    display.setCursor(32, 136);
    display.print("If this is visible, panel pins work.");
  } while (display.nextPage());
}

enum class WaitAction {
  None,
  Refresh,
  ForceRefresh,
  WifiSetup,
};

static WaitAction sleepOrWait(uint32_t seconds) {
  if (ENABLE_DEEP_SLEEP) {
    Serial.printf("Deep sleep for %lu seconds\n", static_cast<unsigned long>(seconds));
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.println("Deep sleep disabled. Waiting before next refresh.");
  const uint32_t waitStartedAt = millis();
  const uint32_t waitMs = seconds * 1000UL;
  while (millis() - waitStartedAt < waitMs) {
    Serial.printf("Heartbeat: waiting, state=%s, err=%d, uptime=%lu ms, wifi=%s\n",
                  deviceState,
                  lastErrorCode,
                  static_cast<unsigned long>(millis()),
                  WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");

    const uint32_t heartbeatStartedAt = millis();
    while (millis() - heartbeatStartedAt < DEBUG_HEARTBEAT_SECONDS * 1000UL) {
      if (bothPageButtonsPressed()) {
        const uint32_t pressedAt = millis();
        while (bothPageButtonsPressed()) {
          if (millis() - pressedAt >= 2000) {
            Serial.println("Button: Wi-Fi setup");
            while (digitalRead(BUTTON_LEFT_PIN) == LOW || digitalRead(BUTTON_RIGHT_PIN) == LOW) {
              delay(10);
            }
            return WaitAction::WifiSetup;
          }
          delay(50);
        }
      }

      if (buttonPressed(BUTTON_REFRESH_PIN)) {
        Serial.println("Button: refresh");
        while (digitalRead(BUTTON_REFRESH_PIN) == LOW) {
          delay(10);
        }
        return WaitAction::ForceRefresh;
      }

      if (buttonPressed(BUTTON_LEFT_PIN)) {
        screenPage = (screenPage + SCREEN_PAGE_COUNT - 1) % SCREEN_PAGE_COUNT;
        Serial.printf("Button: page left -> %d\n", screenPage);
        while (digitalRead(BUTTON_LEFT_PIN) == LOW) {
          delay(10);
        }
        return WaitAction::Refresh;
      }

      if (buttonPressed(BUTTON_RIGHT_PIN)) {
        screenPage = (screenPage + 1) % SCREEN_PAGE_COUNT;
        Serial.printf("Button: page right -> %d\n", screenPage);
        while (digitalRead(BUTTON_RIGHT_PIN) == LOW) {
          delay(10);
        }
        return WaitAction::Refresh;
      }

      delay(50);
    }
  }

  return WaitAction::None;
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  const String savedSsid = storedWifiSsid();
  const String savedPassword = storedWifiPassword();
  const String ssid = savedSsid.length() > 0 ? savedSsid : String(WIFI_SSID);
  const String password = savedSsid.length() > 0 ? savedPassword : String(WIFI_PASSWORD);

  Serial.printf("Connecting Wi-Fi: %s", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
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

static String wifiSetupPage(int networkCount, bool saved) {
  String body = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>E-ink Wi-Fi Setup</title>"
                  "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:24px;}"
                  "label,input,select,button{display:block;width:100%;box-sizing:border-box;font-size:18px;margin-top:10px;}"
                  "input,select{padding:10px}button{padding:12px;font-weight:700}.note{color:#555}</style>"
                  "</head><body><h1>E-ink Wi-Fi Setup</h1>");

  if (saved) {
    body += F("<p>Saved. Device will restart now.</p>");
  } else {
    body += F("<p class='note'>Choose a 2.4GHz Wi-Fi network.</p>"
              "<form method='POST' action='/save'><label>Network</label><select name='ssid'>");
    for (int i = 0; i < networkCount; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }
      body += F("<option value=\"");
      body += htmlEscape(ssid);
      body += F("\">");
      body += htmlEscape(ssid);
      body += F(" (");
      body += WiFi.RSSI(i);
      body += F(" dBm)</option>");
    }
    body += F("</select><label>Password</label><input name='password' type='password' autocomplete='current-password'>"
              "<button type='submit'>Save and Restart</button></form>"
              "<form method='GET' action='/'><button type='submit'>Rescan</button></form>");
  }

  body += F("</body></html>");
  return body;
}

static void startWifiSetupPortal() {
  deviceState = "wifi-setup";
  lastErrorCode = 0;

  const String suffix = WiFi.macAddress().substring(12);
  const String apName = "EINK-SETUP-" + suffix;
  const IPAddress apIP(192, 168, 4, 1);
  const IPAddress netmask(255, 255, 255, 0);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, netmask);
  WiFi.softAP(apName.c_str());

  const int networkCount = WiFi.scanNetworks();
  Serial.printf("Wi-Fi setup AP: %s, open http://192.168.4.1\n", apName.c_str());
  if (ENABLE_DISPLAY) {
    setupDisplay();
    drawStatus("Wi-Fi setup", "Connect to " + apName + " then open 192.168.4.1");
  }

  DNSServer dnsServer;
  WebServer server(80);
  bool saved = false;

  dnsServer.start(53, "*", apIP);
  server.on("/", HTTP_GET, [&]() {
    server.send(200, "text/html", wifiSetupPage(networkCount, saved));
  });
  server.on("/save", HTTP_POST, [&]() {
    const String ssid = server.arg("ssid");
    const String password = server.arg("password");
    if (ssid.length() == 0) {
      server.send(400, "text/plain", "SSID is required");
      return;
    }

    saveWifiCredentials(ssid, password);
    saved = true;
    server.send(200, "text/html", wifiSetupPage(networkCount, true));
    Serial.printf("Wi-Fi credentials saved: %s\n", ssid.c_str());
  });
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  const uint32_t startedAt = millis();
  while (!saved && millis() - startedAt < WIFI_SETUP_TIMEOUT_SECONDS * 1000UL) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.scanDelete();

  if (saved) {
    delay(800);
    ESP.restart();
  }

  Serial.println("Wi-Fi setup timed out");
  if (ENABLE_DISPLAY) {
    drawStatus("Wi-Fi setup", "Timed out; returning to dashboard");
  }
}

static bool fetchBitmap(const String &endpoint, std::unique_ptr<uint8_t[]> &buffer, int &size) {
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
  Serial.printf("Bitmap content length: %d\n", contentLength);
  if (contentLength > MAX_IMAGE_BYTES) {
    Serial.println("Bitmap response exceeds MAX_IMAGE_BYTES");
    http.end();
    return false;
  }

  const int bufferCapacity = contentLength > 0 ? contentLength : MAX_IMAGE_BYTES;
  buffer.reset(new (std::nothrow) uint8_t[bufferCapacity]);
  if (!buffer) {
    Serial.println("Bitmap buffer allocation failed");
    http.end();
    return false;
  }

  MemoryWriteStream bitmapStream(buffer.get(), bufferCapacity);
  const int written = http.writeToStream(&bitmapStream);

  http.end();

  if (written < 0 || bitmapStream.overflowed() || bitmapStream.size() == 0) {
    Serial.printf("Bitmap download failed: written=%d, received=%u, overflow=%s\n",
                  written,
                  static_cast<unsigned int>(bitmapStream.size()),
                  bitmapStream.overflowed() ? "yes" : "no");
    return false;
  }

  if (contentLength > 0 && static_cast<int>(bitmapStream.size()) != contentLength) {
    Serial.printf("Bitmap download incomplete: %u/%d\n",
                  static_cast<unsigned int>(bitmapStream.size()),
                  contentLength);
    return false;
  }

  size = static_cast<int>(bitmapStream.size());
  Serial.printf("Bitmap received bytes: %d\n", size);
  if (size != SCREEN_BITMAP_BYTES) {
    Serial.printf("Bitmap size mismatch: expected=%d, got=%d\n", SCREEN_BITMAP_BYTES, size);
    return false;
  }

  return true;
}

static bool renderBitmap(const uint8_t *buffer, int size) {
  if (size != SCREEN_BITMAP_BYTES) {
    return false;
  }

  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
      const int rowOffset = y * SCREEN_BYTES_PER_ROW;
      for (int x = 0; x < SCREEN_WIDTH; x++) {
        const bool black = buffer[rowOffset + (x >> 3)] & (0x80 >> (x & 7));
        display.drawPixel(x, y, black ? GxEPD_BLACK : GxEPD_WHITE);
      }
    }
  } while (display.nextPage());

  return true;
}

static void setupDisplay() {
  if (!ENABLE_DISPLAY || displayInitialized) {
    return;
  }

    SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
    Serial.println("Initializing e-paper display");
    display.init(115200, true, 2, false);
    Serial.println("Drawing boot test screen");
    drawBootTest();
    delay(BOOT_TEST_SECONDS * 1000UL);
    displayInitialized = true;
}

static bool refreshScreen(bool forceServerRefresh = false) {
  if (!ENABLE_DISPLAY) {
    deviceState = "display-disabled";
    Serial.println("Display disabled for serial/debug check");
  } else {
    setupDisplay();
  }

  if (!connectWifi()) {
    deviceState = "wifi-failed";
    if (!ENABLE_DISPLAY) {
      return false;
    }
    drawStatus("Wi-Fi failed", "Check WIFI_SSID / WIFI_PASSWORD");
    return false;
  }

  const DeviceTelemetry telemetry = readDeviceTelemetry();
  deviceState = "wifi-connected";
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

  std::unique_ptr<uint8_t[]> bitmapBuffer;
  int bitmapSize = 0;
  if (!fetchBitmap(endpointWithTelemetry(telemetry, forceServerRefresh), bitmapBuffer, bitmapSize)) {
    deviceState = "fetch-failed";
    if (!ENABLE_DISPLAY) {
      return false;
    }
    drawStatus("Fetch failed", "Check endpoint, token, and Vercel logs");
    return false;
  }
  deviceState = "bitmap-received";

  if (!ENABLE_DISPLAY) {
    Serial.println("Display disabled; skipping bitmap render");
    return true;
  }

  if (!renderBitmap(bitmapBuffer.get(), bitmapSize)) {
    deviceState = "render-failed";
    drawStatus("Render failed", "Check bitmap endpoint and size");
    return false;
  }

  deviceState = "screen-updated";
  Serial.println("Screen updated");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("ESP32 e-ink dashboard firmware");
  Serial.printf("Display enabled: %s\n", ENABLE_DISPLAY ? "yes" : "no");
  Serial.printf("Screen page: %d\n", screenPage);
  setupButtons();
  refreshScreen();
}

void loop() {
  const WaitAction action = sleepOrWait(FALLBACK_SLEEP_SECONDS);
  if (action == WaitAction::Refresh) {
    refreshScreen(false);
  } else if (action == WaitAction::ForceRefresh) {
    refreshScreen(true);
  } else if (action == WaitAction::WifiSetup) {
    startWifiSetupPortal();
    refreshScreen(true);
  }
}
