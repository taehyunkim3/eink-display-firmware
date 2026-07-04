#include <Arduino.h>
#include <DNSServer.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <memory>
#include <new>

#include <ArduinoJson.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "config.h"
#include "generated/galmuri_7_bitmap.h"
#include "generated/galmuri_9_bitmap.h"
#include "generated/galmuri_11_bitmap.h"
#include "generated/galmuri_11_bold_bitmap.h"
#include "generated/galmuri_14_bitmap.h"

#ifndef BUTTON_DEBOUNCE_MS
#define BUTTON_DEBOUNCE_MS 30
#endif

#ifndef BUTTON_SCAN_INTERVAL_MS
#define BUTTON_SCAN_INTERVAL_MS 10
#endif

#ifndef BUTTON_CLICK_MIN_MS
#define BUTTON_CLICK_MIN_MS 30
#endif

#ifndef WIFI_SETUP_CHORD_GRACE_MS
#define WIFI_SETUP_CHORD_GRACE_MS 700
#endif

#ifndef WIFI_SETUP_HOLD_MS
#define WIFI_SETUP_HOLD_MS 1500
#endif

#ifndef ENABLE_CHARGER_STATUS
#define ENABLE_CHARGER_STATUS false
#endif

#ifndef CHARGER_I2C_SDA
#define CHARGER_I2C_SDA 39
#endif

#ifndef CHARGER_I2C_SCL
#define CHARGER_I2C_SCL 40
#endif

#ifndef CHARGER_I2C_ADDRESS
#define CHARGER_I2C_ADDRESS 0x6B
#endif

#ifndef CHARGER_STATUS_REGISTER
#define CHARGER_STATUS_REGISTER 0x0B
#endif

#ifndef CHARGER_STATUS_SHIFT
#define CHARGER_STATUS_SHIFT 3
#endif

#ifndef CHARGER_STATUS_MASK
#define CHARGER_STATUS_MASK 0x03
#endif

#ifndef ENABLE_SD_ASSETS
#define ENABLE_SD_ASSETS true
#endif

#ifndef SD_SCK
#define SD_SCK EPD_SCK
#endif

#ifndef SD_MISO
#define SD_MISO 8
#endif

#ifndef SD_MOSI
#define SD_MOSI EPD_MOSI
#endif

#ifndef SD_CS
#define SD_CS 14
#endif

#ifndef SD_EN
#define SD_EN 16
#endif

#ifndef SD_DET
#define SD_DET 15
#endif

#ifndef WIFI_BUTTON_PASSWORD_MAX_LENGTH
#define WIFI_BUTTON_PASSWORD_MAX_LENGTH 64
#endif

#ifndef WIFI_BUTTON_SAVE_DOUBLE_PRESS_MS
#define WIFI_BUTTON_SAVE_DOUBLE_PRESS_MS 900
#endif

#ifndef WIFI_SETUP_MAX_NETWORKS
#define WIFI_SETUP_MAX_NETWORKS 10
#endif

#ifndef PAGE_FULL_REFRESH_INTERVAL
#define PAGE_FULL_REFRESH_INTERVAL 5
#endif

#ifndef SETTINGS_REFRESH_SECONDS_MIN
#define SETTINGS_REFRESH_SECONDS_MIN 300
#endif

#ifndef SETTINGS_REFRESH_SECONDS_MAX
#define SETTINGS_REFRESH_SECONDS_MAX 7200
#endif

#ifndef DEVICE_HTTP_CONNECT_TIMEOUT_MS
#define DEVICE_HTTP_CONNECT_TIMEOUT_MS 15000
#endif

#ifndef DEVICE_HTTP_TIMEOUT_MS
#define DEVICE_HTTP_TIMEOUT_MS 25000
#endif

#ifndef DEVICE_FETCH_ATTEMPTS
#define DEVICE_FETCH_ATTEMPTS 3
#endif

#ifndef MAX_JSON_BYTES
#define MAX_JSON_BYTES 64000
#endif

static const char WIFI_PASSWORD_CHARSET[] =
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!@#$%^&*()-_=+[]{}:;,.?/\\|~`'\" ";

GxEPD2_BW<EPD_MODEL, EPD_MODEL::HEIGHT> display(
    EPD_MODEL(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX koreanFonts;
static uint16_t koreanTextForeground = GxEPD_BLACK;
static uint16_t koreanTextBackground = GxEPD_WHITE;

constexpr int SCREEN_WIDTH = 800;
constexpr int SCREEN_HEIGHT = 480;
constexpr int SCREEN_BYTES_PER_ROW = SCREEN_WIDTH / 8;
constexpr int SCREEN_BITMAP_BYTES = SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT;
static const char *deviceState = "booting";
static int lastErrorCode = 0;
static int lastContentLength = 0;
static int lastReceivedBytes = 0;
static int lastFetchAttempt = 0;
static String lastErrorDetail = "";
static bool displayInitialized = false;
static bool sdAssetsInitialized = false;
static bool sdAssetsAvailable = false;
RTC_DATA_ATTR static int screenPage = 0;
RTC_DATA_ATTR static uint32_t pageTransitionRefreshCount = 0;

enum class TextSize {
  Micro,
  Tiny,
  Small,
  Bold,
  Large,
};

static void setupDisplay();
static void drawKorean(int16_t x, int16_t y, const String &text, TextSize size = TextSize::Small);

static void setLastError(int code, const String &detail) {
  lastErrorCode = code;
  lastErrorDetail = detail;
}

static String fetchFailureDetail() {
  String detail = lastErrorDetail.length() > 0 ? lastErrorDetail : String("알 수 없는 오류");
  detail += "\nerr=" + String(lastErrorCode) + " try=" + String(lastFetchAttempt);

  if (lastContentLength > 0 || lastReceivedBytes > 0) {
    detail += " bytes=" + String(lastReceivedBytes) + "/" + String(lastContentLength);
  }

  return detail;
}

struct DeviceTelemetry {
  String ssid;
  int32_t rssi;
  int32_t batteryPercent;
  float batteryVoltage;
  String batteryChargeState;
};

struct DeviceSettings {
  uint32_t fullRefreshInterval;
  uint32_t refreshSeconds;
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
  const String ssid = preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  preferences.end();
  return ssid;
}

static String storedWifiPassword() {
  Preferences preferences;
  preferences.begin("wifi", false);
  const String password = preferences.isKey("password") ? preferences.getString("password", "") : "";
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

static uint32_t clampSetting(uint32_t value, uint32_t minimum, uint32_t maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

static DeviceSettings defaultDeviceSettings() {
  return {
      clampSetting(PAGE_FULL_REFRESH_INTERVAL, 2, 20),
      clampSetting(FALLBACK_SLEEP_SECONDS, SETTINGS_REFRESH_SECONDS_MIN, SETTINGS_REFRESH_SECONDS_MAX),
  };
}

static DeviceSettings loadDeviceSettings() {
  const DeviceSettings defaults = defaultDeviceSettings();
  Preferences preferences;
  preferences.begin("settings", false);
  const uint32_t fullRefreshInterval =
      preferences.getUInt("fullEvery", defaults.fullRefreshInterval);
  const uint32_t refreshSeconds = preferences.getUInt("refreshSec", defaults.refreshSeconds);
  preferences.end();

  const uint32_t normalizedFullRefreshInterval =
      fullRefreshInterval < 2 ? defaults.fullRefreshInterval : fullRefreshInterval;

  return {
      clampSetting(normalizedFullRefreshInterval, 2, 20),
      clampSetting(refreshSeconds, SETTINGS_REFRESH_SECONDS_MIN, SETTINGS_REFRESH_SECONDS_MAX),
  };
}

static void saveDeviceSettings(const DeviceSettings &settings) {
  Preferences preferences;
  preferences.begin("settings", false);
  preferences.putUInt("fullEvery", clampSetting(settings.fullRefreshInterval, 2, 20));
  preferences.putUInt("refreshSec",
                      clampSetting(settings.refreshSeconds,
                                   SETTINGS_REFRESH_SECONDS_MIN,
                                   SETTINGS_REFRESH_SECONDS_MAX));
  preferences.end();
}

static int loadSavedScreenPage() {
  Preferences preferences;
  preferences.begin("state", false);
  const int savedPage = preferences.getInt("page", 0);
  preferences.end();
  return ((savedPage % SCREEN_PAGE_COUNT) + SCREEN_PAGE_COUNT) % SCREEN_PAGE_COUNT;
}

static void saveScreenPage() {
  Preferences preferences;
  preferences.begin("state", false);
  preferences.putInt("page", ((screenPage % SCREEN_PAGE_COUNT) + SCREEN_PAGE_COUNT) % SCREEN_PAGE_COUNT);
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
  if (millivolts >= BATTERY_FULL_MV) {
    return 100;
  }

  const float percent =
      (millivolts - BATTERY_EMPTY_MV) * 100.0f / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
  return constrain(static_cast<int32_t>(roundf(percent)), 0, 100);
}

static bool readChargerRegister(uint8_t reg, uint8_t &value) {
  if (!ENABLE_CHARGER_STATUS) {
    return false;
  }

  static bool wireStarted = false;
  if (!wireStarted) {
    Wire.begin(CHARGER_I2C_SDA, CHARGER_I2C_SCL);
    Wire.setClock(100000);
    wireStarted = true;
  }

  Wire.beginTransmission(CHARGER_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(static_cast<uint8_t>(CHARGER_I2C_ADDRESS), static_cast<uint8_t>(1)) != 1) {
    return false;
  }

  value = Wire.read();
  return true;
}

static String readBatteryChargeState() {
  uint8_t status = 0;
  if (!readChargerRegister(CHARGER_STATUS_REGISTER, status)) {
    return "unknown";
  }

  const uint8_t chargeStatus = (status >> CHARGER_STATUS_SHIFT) & CHARGER_STATUS_MASK;
  if (chargeStatus == 1 || chargeStatus == 2) {
    return "charging";
  }
  if (chargeStatus == 3) {
    return "full";
  }
  return "not_charging";
}

static DeviceTelemetry readDeviceTelemetry() {
  const float voltage = readBatteryVoltage();

  return {
      WiFi.SSID(),
      WiFi.RSSI(),
      batteryPercentFromVoltage(voltage),
      voltage,
      readBatteryChargeState(),
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
  endpoint += "&charge=" + telemetry.batteryChargeState;

  return endpoint;
}

static String dashboardJsonEndpoint() {
  String endpoint = DEVICE_ENDPOINT;
  endpoint.replace("/api/screen.bin", "/api/screen.json");
  endpoint.replace("/api/screen.png", "/api/screen.json");
  return endpoint;
}

static String endpointWithTelemetry(const String &baseEndpoint, const DeviceTelemetry &telemetry) {
  String endpoint = baseEndpoint;
  endpoint += endpoint.indexOf('?') >= 0 ? '&' : '?';
  endpoint += "wifi=connected";
  endpoint += "&ssid=" + urlEncode(telemetry.ssid);
  endpoint += "&rssi=" + String(telemetry.rssi);
  endpoint += "&page=" + String(screenPage);

  if (telemetry.batteryPercent >= 0) {
    endpoint += "&battery=" + String(telemetry.batteryPercent);
    endpoint += "&batteryVoltage=" + String(telemetry.batteryVoltage, 2);
  }
  endpoint += "&charge=" + telemetry.batteryChargeState;

  return endpoint;
}

static String endpointWithTelemetry(const DeviceTelemetry &telemetry, bool forceServerRefresh) {
  String endpoint = endpointWithTelemetry(dashboardJsonEndpoint(), telemetry);
  if (forceServerRefresh) {
    endpoint += "&force=1";
    endpoint += "&nonce=" + String(millis());
  }
  return endpoint;
}

enum class ButtonEvent {
  None,
  LeftClick,
  RightClick,
  RefreshClick,
  LeftRightHold,
};

static uint32_t absDiff(uint32_t a, uint32_t b) {
  return a >= b ? a - b : b - a;
}

static bool rawButtonDown(int pin) {
  return digitalRead(pin) == LOW;
}

// Keep all physical button interpretation here. Short clicks are emitted only
// on release, while the Wi-Fi chord is emitted only after left+right are held
// together. This avoids delaying every single click just to check for a chord.
class ButtonManager {
public:
  void begin() {
    reset();
  }

  void reset() {
    const uint32_t now = millis();
    left_.reset(BUTTON_LEFT_PIN, now);
    right_.reset(BUTTON_RIGHT_PIN, now);
    refresh_.reset(BUTTON_REFRESH_PIN, now);
    leftRightHoldEmitted_ = false;
    bothDownStartedAt_ = 0;
  }

  ButtonEvent update() {
    if (!ENABLE_BUTTONS) {
      return ButtonEvent::None;
    }

    const uint32_t now = millis();
    left_.update(now);
    right_.update(now);
    refresh_.update(now);

    ButtonEvent event = ButtonEvent::None;
    bool resetLeftRightAfterEvent = false;
    const bool bothDown = left_.down && right_.down;
    if (bothDown) {
      if (bothDownStartedAt_ == 0) {
        bothDownStartedAt_ = now;
      }

      const bool joinedInWindow =
          absDiff(left_.downAt, right_.downAt) <= WIFI_SETUP_CHORD_GRACE_MS;
      if (joinedInWindow && !leftRightHoldEmitted_ &&
          now - bothDownStartedAt_ >= WIFI_SETUP_HOLD_MS) {
        leftRightHoldEmitted_ = true;
        event = ButtonEvent::LeftRightHold;
      }
    } else if (!left_.down && !right_.down) {
      resetLeftRightAfterEvent = true;
    }

    if (event == ButtonEvent::None && left_.released && left_.releasedAfter >= BUTTON_CLICK_MIN_MS &&
        !leftRightHoldEmitted_) {
      event = ButtonEvent::LeftClick;
    }
    if (event == ButtonEvent::None && right_.released &&
        right_.releasedAfter >= BUTTON_CLICK_MIN_MS && !leftRightHoldEmitted_) {
      event = ButtonEvent::RightClick;
    }
    if (event == ButtonEvent::None && refresh_.released &&
        refresh_.releasedAfter >= BUTTON_CLICK_MIN_MS) {
      event = ButtonEvent::RefreshClick;
    }

    if (resetLeftRightAfterEvent) {
      leftRightHoldEmitted_ = false;
      bothDownStartedAt_ = 0;
    }

    return event;
  }

  void waitForAllReleased() {
    while (rawButtonDown(BUTTON_LEFT_PIN) || rawButtonDown(BUTTON_RIGHT_PIN) ||
           rawButtonDown(BUTTON_REFRESH_PIN)) {
      delay(BUTTON_SCAN_INTERVAL_MS);
    }
    reset();
  }

private:
  struct DebouncedButton {
    bool raw = false;
    bool down = false;
    bool released = false;
    uint32_t rawChangedAt = 0;
    uint32_t downAt = 0;
    uint32_t releasedAfter = 0;

    void reset(int pin, uint32_t now) {
      pin_ = pin;
      raw = rawButtonDown(pin_);
      down = raw;
      released = false;
      rawChangedAt = now;
      downAt = raw ? now : 0;
      releasedAfter = 0;
    }

    void update(uint32_t now) {
      released = false;
      releasedAfter = 0;
      const bool currentRaw = rawButtonDown(pin_);
      if (currentRaw != raw) {
        raw = currentRaw;
        rawChangedAt = now;
      }

      if (raw != down && now - rawChangedAt >= BUTTON_DEBOUNCE_MS) {
        down = raw;
        if (down) {
          downAt = now;
        } else {
          released = true;
          releasedAfter = downAt == 0 ? 0 : now - downAt;
        }
      }
    }

    int pin_ = -1;
  };

  DebouncedButton left_;
  DebouncedButton right_;
  DebouncedButton refresh_;
  bool leftRightHoldEmitted_ = false;
  uint32_t bothDownStartedAt_ = 0;
};

static ButtonManager buttonManager;

static void setupButtons() {
  if (!ENABLE_BUTTONS) {
    return;
  }

  pinMode(BUTTON_LEFT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_RIGHT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_REFRESH_PIN, INPUT_PULLUP);
  Serial.printf("Buttons: left=GPIO%d right=GPIO%d refresh=GPIO%d\n",
                BUTTON_LEFT_PIN,
                BUTTON_RIGHT_PIN,
                BUTTON_REFRESH_PIN);
  buttonManager.begin();
}

static void drawStatus(const String &title, const String &detail, bool partialRefresh = false) {
  display.setRotation(0);
  if (partialRefresh) {
    display.setPartialWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  } else {
    display.setFullWindow();
  }
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.drawRect(10, 10, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, GxEPD_BLACK);
    drawKorean(32, 72, title);
    int16_t y = 116;
    int start = 0;
    while (start <= detail.length() && y <= SCREEN_HEIGHT - 44) {
      const int end = detail.indexOf('\n', start);
      const String line = end >= 0 ? detail.substring(start, end) : detail.substring(start);
      drawKorean(32, y, line);
      if (end < 0) {
        break;
      }
      start = end + 1;
      y += 34;
    }
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
    drawKorean(32, 64, "전자잉크 대시보드");
    drawKorean(32, 104, "부팅 중입니다.");
    drawKorean(32, 136, "와이파이 연결과 화면 데이터를 준비합니다.");
  } while (display.nextPage());
}

enum class SettingsStage {
  Menu,
  NetworkSelect,
  PasswordInput,
  DisplaySettings,
  Saved,
};

struct WifiNetworkEntry {
  String ssid;
  int32_t rssi;
};

static int32_t wifiSignalPercentFromRssi(int32_t rssi) {
  return constrain(static_cast<int32_t>(roundf((rssi + 90) * 100.0f / 60.0f)), 0, 100);
}

static uint8_t wifiSignalBarsFromRssi(int32_t rssi) {
  const int32_t percent = wifiSignalPercentFromRssi(rssi);
  if (percent >= 75) {
    return 4;
  }
  if (percent >= 50) {
    return 3;
  }
  if (percent >= 25) {
    return 2;
  }
  if (percent > 0) {
    return 1;
  }
  return 0;
}

static void drawWifiSignalIcon(int16_t x, int16_t y, int32_t rssi, uint16_t color = GxEPD_BLACK) {
  const uint8_t bars = wifiSignalBarsFromRssi(rssi);
  for (uint8_t i = 0; i < 4; i++) {
    const int16_t barHeight = 5 + i * 4;
    const int16_t barX = x + i * 8;
    const int16_t barY = y - barHeight;
    display.drawRect(barX, barY, 5, barHeight, color);
    if (i < bars) {
      display.fillRect(barX + 1, barY + 1, 3, barHeight - 2, color);
    }
  }
}

static int buildWifiNetworkList(int scanCount, WifiNetworkEntry *networks, int capacity) {
  int count = 0;

  for (int scanIndex = 0; scanIndex < scanCount; scanIndex++) {
    const String ssid = WiFi.SSID(scanIndex);
    if (ssid.length() == 0) {
      continue;
    }

    const int32_t rssi = WiFi.RSSI(scanIndex);
    bool replacedDuplicate = false;
    for (int i = 0; i < count; i++) {
      if (networks[i].ssid == ssid) {
        if (rssi > networks[i].rssi) {
          networks[i].rssi = rssi;
        }
        replacedDuplicate = true;
        break;
      }
    }

    if (!replacedDuplicate && count < capacity) {
      networks[count++] = {ssid, rssi};
    } else if (!replacedDuplicate && count > 0 && rssi > networks[count - 1].rssi) {
      networks[count - 1] = {ssid, rssi};
    }

    for (int i = 1; i < count; i++) {
      WifiNetworkEntry current = networks[i];
      int j = i - 1;
      while (j >= 0 && networks[j].rssi < current.rssi) {
        networks[j + 1] = networks[j];
        j--;
      }
      networks[j + 1] = current;
    }
  }

  return count;
}

static String wifiPasswordChoiceLabel(int index) {
  const int charsetLength = strlen(WIFI_PASSWORD_CHARSET);
  if (index < charsetLength) {
    const char c = WIFI_PASSWORD_CHARSET[index];
    if (c == ' ') {
      return "공백";
    }
    return String(c);
  }
  if (index == charsetLength) {
    return "삭제";
  }
  return "취소";
}

static const uint32_t FULL_REFRESH_OPTIONS[] = {2, 3, 5, 10, 20};
static const uint32_t WEB_REFRESH_OPTIONS[] = {300, 600, 1800, 3600, 7200};

static uint8_t optionIndexForValue(const uint32_t *options,
                                   uint8_t optionCount,
                                   uint32_t value,
                                   uint8_t fallbackIndex) {
  for (uint8_t i = 0; i < optionCount; i++) {
    if (options[i] == value) {
      return i;
    }
  }
  return fallbackIndex < optionCount ? fallbackIndex : 0;
}

static String refreshSecondsLabel(uint32_t seconds) {
  if (seconds < 60) {
    return String(seconds) + "초";
  }

  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    return String(minutes) + "분";
  }

  const uint32_t hours = minutes / 60;
  return String(hours) + "시간";
}

static String fullRefreshIntervalLabel(uint32_t interval) {
  return String(interval) + "번 전환마다";
}

static void setKoreanTextColors(uint16_t foreground, uint16_t background) {
  koreanTextForeground = foreground;
  koreanTextBackground = background;
  koreanFonts.setForegroundColor(foreground);
  koreanFonts.setBackgroundColor(background);
}

static void setKoreanFont() {
  koreanFonts.setFont(u8g2_font_unifont_t_korean2);
  koreanFonts.setFontMode(1);
  koreanFonts.setFontDirection(0);
  koreanFonts.setForegroundColor(koreanTextForeground);
  koreanFonts.setBackgroundColor(koreanTextBackground);
}

static bool readUtf8Codepoint(const char *text, size_t length, size_t &index, uint32_t &codepoint) {
  if (index >= length) {
    return false;
  }

  const uint8_t first = static_cast<uint8_t>(text[index++]);
  if (first < 0x80) {
    codepoint = first;
    return true;
  }

  uint8_t expected = 0;
  if ((first & 0xE0) == 0xC0) {
    codepoint = first & 0x1F;
    expected = 1;
  } else if ((first & 0xF0) == 0xE0) {
    codepoint = first & 0x0F;
    expected = 2;
  } else if ((first & 0xF8) == 0xF0) {
    codepoint = first & 0x07;
    expected = 3;
  } else {
    codepoint = first;
    return true;
  }

  for (uint8_t i = 0; i < expected; i++) {
    if (index >= length) {
      return false;
    }
    const uint8_t next = static_cast<uint8_t>(text[index++]);
    if ((next & 0xC0) != 0x80) {
      codepoint = first;
      return true;
    }
    codepoint = (codepoint << 6) | (next & 0x3F);
  }
  return true;
}

static void encodeUtf8(uint32_t codepoint, char *buffer) {
  if (codepoint < 0x80) {
    buffer[0] = static_cast<char>(codepoint);
    buffer[1] = '\0';
  } else if (codepoint < 0x800) {
    buffer[0] = static_cast<char>(0xC0 | (codepoint >> 6));
    buffer[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    buffer[2] = '\0';
  } else if (codepoint < 0x10000) {
    buffer[0] = static_cast<char>(0xE0 | (codepoint >> 12));
    buffer[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    buffer[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    buffer[3] = '\0';
  } else {
    buffer[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    buffer[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    buffer[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    buffer[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    buffer[4] = '\0';
  }
}

struct BitmapFont {
  uint16_t asciiStart;
  uint16_t asciiCount;
  uint16_t hangulStart;
  uint16_t hangulCount;
  uint8_t cellHeight;
  uint8_t cellAscent;
  uint8_t glyphBytesPerRow;
  uint8_t glyphBytes;
  const uint8_t *asciiWidths;
  const uint8_t *asciiBitmaps;
  const uint8_t *bitmaps;
  uint8_t hangulAdvance;
};

static BitmapFont bitmapFontForSize(TextSize size) {
  if (size == TextSize::Large) {
    return {
        GALMURI14_ASCII_START,
        GALMURI14_ASCII_COUNT,
        GALMURI14_HANGUL_START,
        GALMURI14_HANGUL_COUNT,
        GALMURI14_CELL_HEIGHT,
        GALMURI14_CELL_ASCENT,
        GALMURI14_GLYPH_BYTES_PER_ROW,
        GALMURI14_GLYPH_BYTES,
        GALMURI14_ASCII_WIDTHS,
        GALMURI14_ASCII_BITMAPS,
        GALMURI14_HANGUL_BITMAPS,
        GALMURI14_HANGUL_ADVANCE,
    };
  }
  if (size == TextSize::Bold) {
    return {
        GALMURI11BOLD_ASCII_START,
        GALMURI11BOLD_ASCII_COUNT,
        GALMURI11BOLD_HANGUL_START,
        GALMURI11BOLD_HANGUL_COUNT,
        GALMURI11BOLD_CELL_HEIGHT,
        GALMURI11BOLD_CELL_ASCENT,
        GALMURI11BOLD_GLYPH_BYTES_PER_ROW,
        GALMURI11BOLD_GLYPH_BYTES,
        GALMURI11BOLD_ASCII_WIDTHS,
        GALMURI11BOLD_ASCII_BITMAPS,
        GALMURI11BOLD_HANGUL_BITMAPS,
        GALMURI11BOLD_HANGUL_ADVANCE,
    };
  }
  if (size == TextSize::Tiny) {
    return {
        GALMURI9_ASCII_START,
        GALMURI9_ASCII_COUNT,
        GALMURI9_HANGUL_START,
        GALMURI9_HANGUL_COUNT,
        GALMURI9_CELL_HEIGHT,
        GALMURI9_CELL_ASCENT,
        GALMURI9_GLYPH_BYTES_PER_ROW,
        GALMURI9_GLYPH_BYTES,
        GALMURI9_ASCII_WIDTHS,
        GALMURI9_ASCII_BITMAPS,
        GALMURI9_HANGUL_BITMAPS,
        GALMURI9_HANGUL_ADVANCE,
    };
  }
  if (size == TextSize::Micro) {
    return {
        GALMURI7_ASCII_START,
        GALMURI7_ASCII_COUNT,
        GALMURI7_HANGUL_START,
        GALMURI7_HANGUL_COUNT,
        GALMURI7_CELL_HEIGHT,
        GALMURI7_CELL_ASCENT,
        GALMURI7_GLYPH_BYTES_PER_ROW,
        GALMURI7_GLYPH_BYTES,
        GALMURI7_ASCII_WIDTHS,
        GALMURI7_ASCII_BITMAPS,
        GALMURI7_HANGUL_BITMAPS,
        GALMURI7_HANGUL_ADVANCE,
    };
  }

  return {
      GALMURI11_ASCII_START,
      GALMURI11_ASCII_COUNT,
      GALMURI11_HANGUL_START,
      GALMURI11_HANGUL_COUNT,
      GALMURI11_CELL_HEIGHT,
      GALMURI11_CELL_ASCENT,
      GALMURI11_GLYPH_BYTES_PER_ROW,
      GALMURI11_GLYPH_BYTES,
      GALMURI11_ASCII_WIDTHS,
      GALMURI11_ASCII_BITMAPS,
      GALMURI11_HANGUL_BITMAPS,
      GALMURI11_HANGUL_ADVANCE,
  };
}

static bool isBitmapAscii(uint32_t codepoint, const BitmapFont &font) {
  return codepoint >= font.asciiStart &&
         codepoint < font.asciiStart + font.asciiCount;
}

static bool isBitmapHangul(uint32_t codepoint, const BitmapFont &font) {
  return codepoint >= font.hangulStart &&
         codepoint < font.hangulStart + font.hangulCount;
}

static uint8_t bitmapAsciiWidth(uint32_t codepoint, const BitmapFont &font) {
  return pgm_read_byte(&font.asciiWidths[codepoint - font.asciiStart]);
}

static void drawBitmapGlyph(int16_t x,
                            int16_t baseline,
                            uint32_t glyphIndex,
                            const uint8_t *bitmaps,
                            const BitmapFont &font) {
  const uint32_t offset = glyphIndex * font.glyphBytes;
  const int16_t top = baseline - font.cellAscent;

  for (uint8_t row = 0; row < font.cellHeight; row++) {
    // Draw contiguous set-bit runs as horizontal lines instead of pixels.
    int16_t runStart = -1;
    const uint8_t totalCols = font.glyphBytesPerRow * 8;
    for (uint8_t col = 0; col <= totalCols; col++) {
      bool on = false;
      if (col < totalCols) {
        const uint8_t bits = pgm_read_byte(&bitmaps[offset + row * font.glyphBytesPerRow + col / 8]);
        on = (bits & (0x80 >> (col % 8))) != 0;
      }
      if (on && runStart < 0) {
        runStart = col;
      } else if (!on && runStart >= 0) {
        display.drawFastHLine(x + runStart, top + row, col - runStart, koreanTextForeground);
        runStart = -1;
      }
    }
  }
}

static void drawBitmapAscii(int16_t x, int16_t baseline, uint32_t codepoint, const BitmapFont &font) {
  drawBitmapGlyph(x, baseline, codepoint - font.asciiStart, font.asciiBitmaps, font);
}

static void drawBitmapHangul(int16_t x, int16_t baseline, uint32_t codepoint, const BitmapFont &font) {
  drawBitmapGlyph(x, baseline, codepoint - font.hangulStart, font.bitmaps, font);
}

static int16_t measureKorean(const String &text, TextSize size) {
  const BitmapFont font = bitmapFontForSize(size);
  int16_t width = 0;
  size_t index = 0;
  const char *raw = text.c_str();
  const size_t length = text.length();

  while (index < length) {
    uint32_t codepoint = 0;
    if (!readUtf8Codepoint(raw, length, index, codepoint) || codepoint == '\n') {
      break;
    }
    if (isBitmapHangul(codepoint, font)) {
      width += font.hangulAdvance;
    } else if (isBitmapAscii(codepoint, font)) {
      width += bitmapAsciiWidth(codepoint, font);
    } else {
      width += font.hangulAdvance;
    }
  }
  return width;
}

static void drawKorean(int16_t x, int16_t y, const String &text, TextSize size) {
  setKoreanFont();
  const BitmapFont font = bitmapFontForSize(size);
  int16_t cursorX = x;
  size_t index = 0;
  const char *raw = text.c_str();
  const size_t length = text.length();

  while (index < length) {
    uint32_t codepoint = 0;
    if (!readUtf8Codepoint(raw, length, index, codepoint)) {
      break;
    }
    if (codepoint == '\n') {
      break;
    }
    if (isBitmapHangul(codepoint, font)) {
      drawBitmapHangul(cursorX, y, codepoint, font);
      cursorX += font.hangulAdvance;
      continue;
    }
    if (isBitmapAscii(codepoint, font)) {
      if (codepoint != ' ') {
        drawBitmapAscii(cursorX, y, codepoint, font);
      }
      cursorX += bitmapAsciiWidth(codepoint, font);
      continue;
    }

    char utf8[5] = {};
    encodeUtf8(codepoint, utf8);
    koreanFonts.drawUTF8(cursorX, y, utf8);
    const int16_t width = koreanFonts.getUTF8Width(utf8);
    cursorX += width > 0 ? width : 8;
  }
}

static void drawSettingsFrame(const String &apName) {
  display.fillScreen(GxEPD_WHITE);
  display.drawRect(10, 10, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, GxEPD_BLACK);
  display.drawLine(28, 104, SCREEN_WIDTH - 28, 104, GxEPD_BLACK);
  display.drawLine(28, SCREEN_HEIGHT - 104, SCREEN_WIDTH - 28, SCREEN_HEIGHT - 104, GxEPD_BLACK);

  drawKorean(28, 48, "기기 설정");
  drawKorean(28, 82, "좌/우 버튼으로 이동하고 확인 버튼으로 선택합니다.");
  drawKorean(28, SCREEN_HEIGHT - 86, "취소: 상단 버튼 2개를 길게 누르세요.");
  drawKorean(28, SCREEN_HEIGHT - 58, "휴대폰 설정도 가능: 와이파이에서 " + apName + " 연결");
  drawKorean(28, SCREEN_HEIGHT - 30, "브라우저에서 http://192.168.4.1 을 여세요.");
}

static void drawSelectionRow(int16_t x,
                             int16_t y,
                             int16_t width,
                             const String &text,
                             bool selected) {
  if (selected) {
    display.fillRect(x - 6, y - 20, width, 26, GxEPD_BLACK);
    setKoreanTextColors(GxEPD_WHITE, GxEPD_BLACK);
  } else {
    setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
  }
  drawKorean(x, y, text);
  setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
}

static void drawSettingsScreen(SettingsStage stage,
                               const String &apName,
                               const WifiNetworkEntry *networks,
                               int networkCount,
                               int selectedNetwork,
                               const String &password,
                               int passwordChoiceIndex,
                               int menuSelection,
                               const DeviceSettings &settings,
                               int displaySelection,
                               bool partialRefresh) {
  if (!ENABLE_DISPLAY) {
    return;
  }

  display.setRotation(0);
  if (partialRefresh) {
    display.setPartialWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  } else {
    display.setFullWindow();
  }
  display.firstPage();
  do {
    drawSettingsFrame(apName);
    display.setTextColor(GxEPD_BLACK);

    if (stage == SettingsStage::Menu) {
      drawKorean(28, 145, "설정 메뉴");
      drawKorean(28, 174, "좌/우: 메뉴 이동    확인: 들어가기");
      drawSelectionRow(46, 232, SCREEN_WIDTH - 92, "와이파이 설정", menuSelection == 0);
      drawSelectionRow(46, 282, SCREEN_WIDTH - 92, "화면 설정", menuSelection == 1);
    } else if (stage == SettingsStage::NetworkSelect) {
      drawKorean(28, 145, "1. 와이파이 선택");
      drawKorean(28, 174, "좌/우: 목록 이동    확인: 선택");

      if (networkCount <= 0 || selectedNetwork < 0 || selectedNetwork >= networkCount ||
          networks[selectedNetwork].ssid.length() == 0) {
        drawKorean(28, 230, "검색된 와이파이가 없습니다. 아래 휴대폰 설정을 사용하세요.");
      } else {
        constexpr int16_t rowTop = 192;
        constexpr int16_t rowHeight = 18;
        for (int i = 0; i < networkCount; i++) {
          const int16_t y = rowTop + i * rowHeight;
          if (i == selectedNetwork) {
            display.fillRect(22, y - 15, SCREEN_WIDTH - 52, 17, GxEPD_BLACK);
            setKoreanTextColors(GxEPD_WHITE, GxEPD_BLACK);
          } else {
            setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
          }
          drawKorean(32, y, networks[i].ssid.substring(0, 22));
          display.setTextColor(i == selectedNetwork ? GxEPD_WHITE : GxEPD_BLACK);
          display.setFont(&FreeMono9pt7b);
          display.setCursor(604, y);
          display.print(wifiSignalPercentFromRssi(networks[i].rssi));
          display.print("%");
          drawWifiSignalIcon(690,
                             y - 2,
                             networks[i].rssi,
                             i == selectedNetwork ? GxEPD_WHITE : GxEPD_BLACK);
          display.setTextColor(GxEPD_BLACK);
          setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
        }
      }
    } else if (stage == SettingsStage::PasswordInput) {
      drawKorean(28, 145, "2. 비밀번호 입력");
      drawKorean(28, 178, "좌/우: 문자 변경    확인: 현재 문자 입력");
      drawKorean(28, 210, "확인을 빠르게 두 번 누르면 저장합니다.");
      const String ssid =
          selectedNetwork >= 0 && selectedNetwork < networkCount ? networks[selectedNetwork].ssid : "";
      drawKorean(28, 246, String("와이파이: ") + ssid.substring(0, 28));
      drawKorean(28, 280, String("입력값: ") + password + " (" +
                          String(password.length()) + "글자)");
      drawKorean(28, 330, String("현재 문자: [ ") + wifiPasswordChoiceLabel(passwordChoiceIndex) +
                          " ]");
    } else if (stage == SettingsStage::DisplaySettings) {
      drawKorean(28, 145, "화면 설정");
      drawKorean(28, 174, "좌/우: 항목 이동    확인: 값 변경/저장");
      drawSelectionRow(46,
                       226,
                       SCREEN_WIDTH - 92,
                       String("전체 refresh: ") +
                           fullRefreshIntervalLabel(settings.fullRefreshInterval),
                       displaySelection == 0);
      drawSelectionRow(46,
                       276,
                       SCREEN_WIDTH - 92,
                       String("웹 데이터 갱신: ") + refreshSecondsLabel(settings.refreshSeconds),
                       displaySelection == 1);
      drawSelectionRow(46, 326, SCREEN_WIDTH - 92, "저장하고 나가기", displaySelection == 2);
    } else {
      drawKorean(28, 176, "저장했습니다. 기기를 다시 시작합니다.");
    }
  } while (display.nextPage());
}

enum class WaitAction {
  None,
  PageRefresh,
  ForceRefresh,
  WifiSetup,
};

static bool shouldUsePartialRefreshForScreenTransition() {
  const DeviceSettings settings = loadDeviceSettings();
  if (settings.fullRefreshInterval <= 1) {
    pageTransitionRefreshCount = 0;
    return false;
  }

  pageTransitionRefreshCount++;
  if (pageTransitionRefreshCount >= settings.fullRefreshInterval) {
    pageTransitionRefreshCount = 0;
    return false;
  }

  return true;
}

static WaitAction sleepOrWait(uint32_t seconds) {
  if (ENABLE_DEEP_SLEEP) {
    Serial.printf("Deep sleep for %lu seconds\n", static_cast<unsigned long>(seconds));
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.println("Deep sleep disabled. Waiting before next refresh.");
  buttonManager.reset();
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
      const ButtonEvent buttonEvent = buttonManager.update();
      if (buttonEvent == ButtonEvent::RefreshClick) {
        Serial.printf("Button: refresh GPIO%d\n", BUTTON_REFRESH_PIN);
        return WaitAction::ForceRefresh;
      } else if (buttonEvent == ButtonEvent::LeftRightHold) {
        Serial.println("Button: settings chord hold");
        buttonManager.waitForAllReleased();
        return WaitAction::WifiSetup;
      } else if (buttonEvent == ButtonEvent::LeftClick) {
        screenPage = (screenPage + SCREEN_PAGE_COUNT - 1) % SCREEN_PAGE_COUNT;
        saveScreenPage();
        Serial.printf("Button: page left GPIO%d -> %d\n", BUTTON_LEFT_PIN, screenPage);
        return WaitAction::PageRefresh;
      } else if (buttonEvent == ButtonEvent::RightClick) {
        screenPage = (screenPage + 1) % SCREEN_PAGE_COUNT;
        saveScreenPage();
        Serial.printf("Button: page right GPIO%d -> %d\n", BUTTON_RIGHT_PIN, screenPage);
        return WaitAction::PageRefresh;
      }

      delay(BUTTON_SCAN_INTERVAL_MS);
    }
  }

  Serial.println("Refresh interval elapsed");
  return WaitAction::ForceRefresh;
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(200);

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
    setLastError(static_cast<int>(WiFi.status()), String("Wi-Fi 연결 실패\nSSID: ") + ssid);
    Serial.println("Wi-Fi connection failed");
    return false;
  }

  setLastError(0, "");
  Serial.print("Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

static String wifiSetupPage(const WifiNetworkEntry *networks, int networkCount, bool saved) {
  String body = F("<!doctype html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>전자잉크 와이파이 설정</title>"
                  "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:24px;}"
                  "label,input,select,button{display:block;width:100%;box-sizing:border-box;font-size:18px;margin-top:10px;}"
                  "input,select{padding:10px}button{padding:12px;font-weight:700}.note{color:#555}</style>"
                  "</head><body><h1>전자잉크 와이파이 설정</h1>");

  if (saved) {
    body += F("<p>저장했습니다. 기기를 다시 시작합니다.</p>");
  } else {
    body += F("<p class='note'>2.4GHz 와이파이를 선택하고 비밀번호를 입력하세요. "
              "기기 버튼으로도 입력할 수 있습니다.</p>"
              "<form method='POST' action='/save'><label>와이파이 신호 강한 순서 최대 10개</label><select name='ssid'>");
    for (int i = 0; i < networkCount; i++) {
      const String ssid = networks[i].ssid;
      if (ssid.length() == 0) {
        continue;
      }
      body += F("<option value=\"");
      body += htmlEscape(ssid);
      body += F("\">");
      body += htmlEscape(ssid);
      body += F(" (");
      body += wifiSignalPercentFromRssi(networks[i].rssi);
      body += F("%)</option>");
    }
    body += F("</select><label>비밀번호</label><input name='password' type='password' autocomplete='current-password'>"
              "<button type='submit'>저장하고 다시 시작</button></form>"
              "<form method='GET' action='/'><button type='submit'>다시 검색</button></form>");
  }

  body += F("</body></html>");
  return body;
}

static void startSettingsPortal() {
  deviceState = "settings";
  lastErrorCode = 0;

  const String suffix = WiFi.macAddress().substring(12);
  const String apName = "EINK-SETUP-" + suffix;
  const IPAddress apIP(192, 168, 4, 1);
  const IPAddress netmask(255, 255, 255, 0);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, netmask);
  WiFi.softAP(apName.c_str());

  const int scanCount = WiFi.scanNetworks();
  WifiNetworkEntry wifiNetworks[WIFI_SETUP_MAX_NETWORKS];
  const int networkCount = buildWifiNetworkList(scanCount, wifiNetworks, WIFI_SETUP_MAX_NETWORKS);
  Serial.printf("Settings AP: %s, open http://192.168.4.1\n", apName.c_str());
  Serial.printf("Wi-Fi setup networks: scanned=%d, shown=%d\n", scanCount, networkCount);
  setupDisplay();

  DNSServer dnsServer;
  WebServer server(80);
  bool wifiSaved = false;
  bool displaySettingsSaved = false;
  bool exitRequested = false;
  SettingsStage buttonStage = SettingsStage::Menu;
  String buttonPassword;
  DeviceSettings displaySettings = loadDeviceSettings();
  int selectedNetwork = 0;
  int menuSelection = 0;
  int displaySelection = 0;
  uint8_t fullRefreshOptionIndex =
      optionIndexForValue(FULL_REFRESH_OPTIONS,
                          sizeof(FULL_REFRESH_OPTIONS) / sizeof(FULL_REFRESH_OPTIONS[0]),
                          displaySettings.fullRefreshInterval,
                          2);
  uint8_t webRefreshOptionIndex =
      optionIndexForValue(WEB_REFRESH_OPTIONS,
                          sizeof(WEB_REFRESH_OPTIONS) / sizeof(WEB_REFRESH_OPTIONS[0]),
                          displaySettings.refreshSeconds,
                          2);
  int passwordChoiceIndex = 0;
  bool uiDirty = true;
  int lastPasswordConfirmIndex = -1;
  uint32_t lastPasswordConfirmAt = 0;

  auto hasVisibleNetwork = [&]() {
    return networkCount > 0;
  };

  auto moveNetworkSelection = [&](int delta) {
    if (!hasVisibleNetwork()) {
      return;
    }

    selectedNetwork = (selectedNetwork + delta + networkCount) % networkCount;
  };

  dnsServer.start(53, "*", apIP);
  server.on("/", HTTP_GET, [&]() {
    server.send(200, "text/html", wifiSetupPage(wifiNetworks, networkCount, wifiSaved));
  });
  server.on("/save", HTTP_POST, [&]() {
    const String ssid = server.arg("ssid");
    const String password = server.arg("password");
    if (ssid.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "와이파이를 선택해야 합니다.");
      return;
    }

    saveWifiCredentials(ssid, password);
    wifiSaved = true;
    server.send(200, "text/html", wifiSetupPage(wifiNetworks, networkCount, true));
    Serial.printf("Wi-Fi credentials saved: %s\n", ssid.c_str());
  });
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  buttonManager.reset();
  const uint32_t startedAt = millis();
  while (!wifiSaved && !exitRequested && millis() - startedAt < WIFI_SETUP_TIMEOUT_SECONDS * 1000UL) {
    dnsServer.processNextRequest();
    server.handleClient();

    const ButtonEvent buttonEvent = buttonManager.update();
    if (buttonEvent == ButtonEvent::LeftRightHold) {
      exitRequested = true;
      Serial.println("Settings canceled by left+right hold");
      buttonManager.waitForAllReleased();
      continue;
    }

    if (uiDirty) {
      const bool partialUiRefresh = shouldUsePartialRefreshForScreenTransition();
      Serial.printf("Settings UI redraw: stage=%d, partial=%s\n",
                    static_cast<int>(buttonStage),
                    partialUiRefresh ? "yes" : "no");
      drawSettingsScreen(buttonStage,
                         apName,
                         wifiNetworks,
                         networkCount,
                         selectedNetwork,
                         buttonPassword,
                         passwordChoiceIndex,
                         menuSelection,
                         displaySettings,
                         displaySelection,
                         partialUiRefresh);
      uiDirty = false;
    }

    if (buttonEvent == ButtonEvent::LeftClick) {
      if (buttonStage == SettingsStage::Menu) {
        menuSelection = (menuSelection + 1) % 2;
        Serial.printf("Settings menu selection: %d\n", menuSelection);
      } else if (buttonStage == SettingsStage::NetworkSelect) {
        moveNetworkSelection(-1);
        if (hasVisibleNetwork()) {
          Serial.printf("Wi-Fi setup button network: %s (%ld%%)\n",
                        wifiNetworks[selectedNetwork].ssid.c_str(),
                        static_cast<long>(wifiSignalPercentFromRssi(wifiNetworks[selectedNetwork].rssi)));
        }
      } else if (buttonStage == SettingsStage::PasswordInput) {
        const int choiceCount = strlen(WIFI_PASSWORD_CHARSET) + 2;
        passwordChoiceIndex = (passwordChoiceIndex + choiceCount - 1) % choiceCount;
        Serial.printf("Wi-Fi setup password choice: %s\n",
                      wifiPasswordChoiceLabel(passwordChoiceIndex).c_str());
        lastPasswordConfirmIndex = -1;
      } else if (buttonStage == SettingsStage::DisplaySettings) {
        displaySelection = (displaySelection + 2) % 3;
        Serial.printf("Display setting selection: %d\n", displaySelection);
      }
      uiDirty = true;
    } else if (buttonEvent == ButtonEvent::RightClick) {
      if (buttonStage == SettingsStage::Menu) {
        menuSelection = (menuSelection + 1) % 2;
        Serial.printf("Settings menu selection: %d\n", menuSelection);
      } else if (buttonStage == SettingsStage::NetworkSelect) {
        moveNetworkSelection(1);
        if (hasVisibleNetwork()) {
          Serial.printf("Wi-Fi setup button network: %s (%ld%%)\n",
                        wifiNetworks[selectedNetwork].ssid.c_str(),
                        static_cast<long>(wifiSignalPercentFromRssi(wifiNetworks[selectedNetwork].rssi)));
        }
      } else if (buttonStage == SettingsStage::PasswordInput) {
        const int choiceCount = strlen(WIFI_PASSWORD_CHARSET) + 2;
        passwordChoiceIndex = (passwordChoiceIndex + 1) % choiceCount;
        Serial.printf("Wi-Fi setup password choice: %s\n",
                      wifiPasswordChoiceLabel(passwordChoiceIndex).c_str());
        lastPasswordConfirmIndex = -1;
      } else if (buttonStage == SettingsStage::DisplaySettings) {
        displaySelection = (displaySelection + 1) % 3;
        Serial.printf("Display setting selection: %d\n", displaySelection);
      }
      uiDirty = true;
    } else if (buttonEvent == ButtonEvent::RefreshClick) {
      if (buttonStage == SettingsStage::Menu) {
        if (menuSelection == 0) {
          buttonStage = SettingsStage::NetworkSelect;
          Serial.println("Settings menu: Wi-Fi setup");
        } else {
          buttonStage = SettingsStage::DisplaySettings;
          Serial.println("Settings menu: display settings");
        }
      } else if (buttonStage == SettingsStage::NetworkSelect) {
        if (hasVisibleNetwork() && selectedNetwork < networkCount &&
            wifiNetworks[selectedNetwork].ssid.length() > 0) {
          buttonStage = SettingsStage::PasswordInput;
          buttonPassword = "";
          passwordChoiceIndex = 0;
          lastPasswordConfirmIndex = -1;
          lastPasswordConfirmAt = 0;
          Serial.printf("Wi-Fi setup button selected SSID: %s\n",
                        wifiNetworks[selectedNetwork].ssid.c_str());
        }
      } else if (buttonStage == SettingsStage::PasswordInput) {
        const int charsetLength = strlen(WIFI_PASSWORD_CHARSET);
        if (passwordChoiceIndex < charsetLength) {
          const uint32_t now = millis();
          const bool doubleConfirm =
              lastPasswordConfirmIndex == passwordChoiceIndex &&
              now - lastPasswordConfirmAt <= WIFI_BUTTON_SAVE_DOUBLE_PRESS_MS &&
              buttonPassword.length() > 0;

          if (doubleConfirm) {
            const String ssid = wifiNetworks[selectedNetwork].ssid;
            if (ssid.length() > 0) {
              saveWifiCredentials(ssid, buttonPassword);
              wifiSaved = true;
              buttonStage = SettingsStage::Saved;
              Serial.printf("Wi-Fi credentials saved from buttons: %s\n", ssid.c_str());
            }
          } else if (buttonPassword.length() < WIFI_BUTTON_PASSWORD_MAX_LENGTH) {
            buttonPassword += WIFI_PASSWORD_CHARSET[passwordChoiceIndex];
            lastPasswordConfirmIndex = passwordChoiceIndex;
            lastPasswordConfirmAt = now;
          }
        } else if (passwordChoiceIndex == charsetLength) {
          if (buttonPassword.length() > 0) {
            buttonPassword.remove(buttonPassword.length() - 1);
          }
          lastPasswordConfirmIndex = -1;
        } else {
          exitRequested = true;
          Serial.println("Wi-Fi setup button exit");
        }
      } else if (buttonStage == SettingsStage::DisplaySettings) {
        if (displaySelection == 0) {
          fullRefreshOptionIndex =
              (fullRefreshOptionIndex + 1) %
              (sizeof(FULL_REFRESH_OPTIONS) / sizeof(FULL_REFRESH_OPTIONS[0]));
          displaySettings.fullRefreshInterval = FULL_REFRESH_OPTIONS[fullRefreshOptionIndex];
          pageTransitionRefreshCount = 0;
          Serial.printf("Display setting full refresh interval: %lu\n",
                        static_cast<unsigned long>(displaySettings.fullRefreshInterval));
        } else if (displaySelection == 1) {
          webRefreshOptionIndex =
              (webRefreshOptionIndex + 1) %
              (sizeof(WEB_REFRESH_OPTIONS) / sizeof(WEB_REFRESH_OPTIONS[0]));
          displaySettings.refreshSeconds = WEB_REFRESH_OPTIONS[webRefreshOptionIndex];
          Serial.printf("Display setting web refresh seconds: %lu\n",
                        static_cast<unsigned long>(displaySettings.refreshSeconds));
        } else {
          saveDeviceSettings(displaySettings);
          displaySettingsSaved = true;
          exitRequested = true;
          pageTransitionRefreshCount = 0;
          Serial.println("Display settings saved");
        }
      }
      uiDirty = true;
    }

    delay(BUTTON_SCAN_INTERVAL_MS);
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.scanDelete();

  if (wifiSaved) {
    drawSettingsScreen(SettingsStage::Saved,
                       apName,
                       wifiNetworks,
                       networkCount,
                       selectedNetwork,
                       buttonPassword,
                       passwordChoiceIndex,
                       menuSelection,
                       displaySettings,
                       displaySelection,
                       shouldUsePartialRefreshForScreenTransition());
    delay(800);
    ESP.restart();
  }

  if (exitRequested) {
    Serial.println("Settings exited by button");
    if (ENABLE_DISPLAY) {
      const bool partialStatusRefresh = shouldUsePartialRefreshForScreenTransition();
      if (displaySettingsSaved) {
        drawStatus("화면 설정", "저장했습니다. 대시보드로 돌아갑니다.", partialStatusRefresh);
      } else {
        drawStatus("기기 설정", "취소했습니다. 대시보드로 돌아갑니다.", partialStatusRefresh);
      }
    }
    return;
  }

  Serial.println("Settings timed out");
  if (ENABLE_DISPLAY) {
    drawStatus("기기 설정",
               "시간이 초과되었습니다. 대시보드로 돌아갑니다.",
               shouldUsePartialRefreshForScreenTransition());
  }
}

static bool fetchBitmapOnce(const String &endpoint, std::unique_ptr<uint8_t[]> &buffer, int &size) {
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
    setLastError(-1001, "HTTP 시작 실패\n주소 또는 TLS 확인");
    Serial.println("HTTP begin failed");
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + DEVICE_AUTH_TOKEN);
  http.setConnectTimeout(DEVICE_HTTP_CONNECT_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setTimeout(DEVICE_HTTP_TIMEOUT_MS);

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    String detail = String("HTTP ") + String(statusCode);
    if (statusCode == HTTP_CODE_UNAUTHORIZED) {
      detail += "\n토큰 불일치";
    } else if (statusCode >= 500) {
      detail += "\nVercel 서버 오류";
    } else if (statusCode < 0) {
      detail += "\nWi-Fi/TLS/타임아웃";
    } else {
      detail += "\n서버 응답 확인";
    }
    setLastError(statusCode, detail);
    Serial.printf("HTTP failed: %d\n", statusCode);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  lastContentLength = contentLength;
  Serial.printf("Bitmap content length: %d\n", contentLength);
  if (contentLength > MAX_IMAGE_BYTES) {
    setLastError(-1002, "응답 크기 초과\nMAX_IMAGE_BYTES 확인");
    Serial.println("Bitmap response exceeds MAX_IMAGE_BYTES");
    http.end();
    return false;
  }

  const int bufferCapacity = contentLength > 0 ? contentLength : MAX_IMAGE_BYTES;
  buffer.reset(new (std::nothrow) uint8_t[bufferCapacity]);
  if (!buffer) {
    setLastError(-1003, "메모리 할당 실패\n버퍼 크기 확인");
    Serial.println("Bitmap buffer allocation failed");
    http.end();
    return false;
  }

  MemoryWriteStream bitmapStream(buffer.get(), bufferCapacity);
  const int written = http.writeToStream(&bitmapStream);

  http.end();

  if (written < 0 || bitmapStream.overflowed() || bitmapStream.size() == 0) {
    lastReceivedBytes = static_cast<int>(bitmapStream.size());
    setLastError(written < 0 ? written : -1004, "비트맵 다운로드 실패\nWi-Fi 신호/타임아웃 확인");
    Serial.printf("Bitmap download failed: written=%d, received=%u, overflow=%s\n",
                  written,
                  static_cast<unsigned int>(bitmapStream.size()),
                  bitmapStream.overflowed() ? "yes" : "no");
    return false;
  }

  if (contentLength > 0 && static_cast<int>(bitmapStream.size()) != contentLength) {
    lastReceivedBytes = static_cast<int>(bitmapStream.size());
    setLastError(-1005, "비트맵 일부만 수신\nWi-Fi 신호 확인");
    Serial.printf("Bitmap download incomplete: %u/%d\n",
                  static_cast<unsigned int>(bitmapStream.size()),
                  contentLength);
    return false;
  }

  size = static_cast<int>(bitmapStream.size());
  lastReceivedBytes = size;
  Serial.printf("Bitmap received bytes: %d\n", size);
  if (size != SCREEN_BITMAP_BYTES) {
    setLastError(-1006, "비트맵 크기 불일치\nscreen.bin 확인");
    Serial.printf("Bitmap size mismatch: expected=%d, got=%d\n", SCREEN_BITMAP_BYTES, size);
    return false;
  }

  setLastError(0, "");
  return true;
}

static bool fetchBitmap(const String &endpoint, std::unique_ptr<uint8_t[]> &buffer, int &size) {
  for (int attempt = 1; attempt <= DEVICE_FETCH_ATTEMPTS; attempt++) {
    lastFetchAttempt = attempt;
    Serial.printf("Bitmap fetch attempt %d/%d\n", attempt, DEVICE_FETCH_ATTEMPTS);
    if (fetchBitmapOnce(endpoint, buffer, size)) {
      return true;
    }

    buffer.reset();
    size = 0;
    if (attempt < DEVICE_FETCH_ATTEMPTS) {
      delay(1200 * attempt);
    }
  }

  return false;
}

static bool fetchJsonOnce(const String &endpoint, std::unique_ptr<uint8_t[]> &buffer, int &size) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;

  if (isHttpsEndpoint()) {
    secureClient.setInsecure();
  }

  WiFiClient &client = isHttpsEndpoint()
                           ? static_cast<WiFiClient &>(secureClient)
                           : static_cast<WiFiClient &>(plainClient);

  Serial.printf("GET %s\n", endpoint.c_str());
  if (!http.begin(client, endpoint)) {
    setLastError(-1101, "JSON HTTP 시작 실패\n주소 또는 TLS 확인");
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + DEVICE_AUTH_TOKEN);
  http.setConnectTimeout(DEVICE_HTTP_CONNECT_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setTimeout(DEVICE_HTTP_TIMEOUT_MS);

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    String detail = String("JSON HTTP ") + String(statusCode);
    if (statusCode == HTTP_CODE_UNAUTHORIZED) {
      detail += "\n토큰 불일치";
    } else if (statusCode >= 500) {
      detail += "\nVercel 서버 오류";
    } else if (statusCode < 0) {
      detail += "\nWi-Fi/TLS/타임아웃";
    } else {
      detail += "\n서버 응답 확인";
    }
    setLastError(statusCode, detail);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  lastContentLength = contentLength;
  Serial.printf("JSON content length: %d\n", contentLength);
  if (contentLength > MAX_JSON_BYTES) {
    setLastError(-1102, "JSON 응답 크기 초과\nMAX_JSON_BYTES 확인");
    http.end();
    return false;
  }

  const int bufferCapacity = contentLength > 0 ? contentLength : MAX_JSON_BYTES;
  buffer.reset(new (std::nothrow) uint8_t[bufferCapacity]);
  if (!buffer) {
    setLastError(-1103, "JSON 메모리 할당 실패\n버퍼 크기 확인");
    http.end();
    return false;
  }

  MemoryWriteStream jsonStream(buffer.get(), bufferCapacity);
  const int written = http.writeToStream(&jsonStream);
  http.end();

  lastReceivedBytes = static_cast<int>(jsonStream.size());
  if (written < 0 || jsonStream.overflowed() || jsonStream.size() == 0) {
    setLastError(written < 0 ? written : -1104, "JSON 다운로드 실패\nWi-Fi 신호/타임아웃 확인");
    return false;
  }

  if (contentLength > 0 && static_cast<int>(jsonStream.size()) != contentLength) {
    setLastError(-1105, "JSON 일부만 수신\nWi-Fi 신호 확인");
    return false;
  }

  size = static_cast<int>(jsonStream.size());
  setLastError(0, "");
  return true;
}

static bool fetchJson(const String &endpoint, std::unique_ptr<uint8_t[]> &buffer, int &size) {
  for (int attempt = 1; attempt <= DEVICE_FETCH_ATTEMPTS; attempt++) {
    lastFetchAttempt = attempt;
    Serial.printf("JSON fetch attempt %d/%d\n", attempt, DEVICE_FETCH_ATTEMPTS);
    if (fetchJsonOnce(endpoint, buffer, size)) {
      return true;
    }

    buffer.reset();
    size = 0;
    if (attempt < DEVICE_FETCH_ATTEMPTS) {
      delay(1200 * attempt);
    }
  }

  return false;
}

static bool renderBitmap(const uint8_t *buffer, int size, bool partialRefresh) {
  if (size != SCREEN_BITMAP_BYTES) {
    return false;
  }

  display.setRotation(0);
  if (partialRefresh) {
    display.setPartialWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  } else {
    display.setFullWindow();
  }
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

struct CivilDate {
  int year;
  int month;
  int day;
};

static const char *WEEKDAY_LABELS[] = {"일", "월", "화", "수", "목", "금", "토"};

static int daysFromCivil(int year, unsigned month, unsigned day);
static CivilDate civilFromDays(int days);

static String utf8Prefix(const String &value, int maxChars) {
  String output;
  int chars = 0;
  for (int i = 0; i < value.length() && chars < maxChars;) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    int bytes = 1;
    if ((c & 0x80) == 0) {
      bytes = 1;
    } else if ((c & 0xe0) == 0xc0) {
      bytes = 2;
    } else if ((c & 0xf0) == 0xe0) {
      bytes = 3;
    } else if ((c & 0xf8) == 0xf0) {
      bytes = 4;
    }

    if (i + bytes > value.length()) {
      break;
    }
    output += value.substring(i, i + bytes);
    i += bytes;
    chars++;
  }
  return output;
}

static String jsonString(JsonVariantConst value, const String &fallback = "") {
  if (value.isNull()) {
    return fallback;
  }
  if (value.is<const char *>()) {
    return String(value.as<const char *>());
  }
  if (value.is<float>() || value.is<double>()) {
    return String(value.as<float>(), 1);
  }
  if (value.is<int>()) {
    return String(value.as<int>());
  }
  return fallback;
}

static String displaySuffix(const String &suffix) {
  return suffix == "C" ? "°C" : suffix;
}

static String twoDigit(int value) {
  return value < 10 ? "0" + String(value) : String(value);
}

static String formatValue(JsonVariantConst value, const String &suffix, int decimals = 0) {
  if (value.isNull()) {
    return "--";
  }
  const String unit = displaySuffix(suffix);
  if (decimals > 0) {
    return String(value.as<float>(), decimals) + unit;
  }
  return String(static_cast<int>(round(value.as<float>()))) + unit;
}

static String formatIsoTime(const String &value) {
  const int t = value.indexOf('T');
  if (t >= 0 && value.length() >= t + 6) {
    int hours = value.substring(t + 1, t + 3).toInt();
    const int minutes = value.substring(t + 4, t + 6).toInt();
    hours = (hours + 9) % 24;
    String output = twoDigit(hours);
    output += ":";
    output += twoDigit(minutes);
    return output;
  }
  return "";
}

static String dateKeyFromIso(const String &value) {
  if (value.length() < 16) {
    return value.length() >= 10 ? value.substring(0, 10) : value;
  }

  CivilDate date = {
      value.substring(0, 4).toInt(),
      value.substring(5, 7).toInt(),
      value.substring(8, 10).toInt(),
  };
  const int hour = value.substring(11, 13).toInt();
  if (hour + 9 >= 24) {
    date = civilFromDays(daysFromCivil(date.year, date.month, date.day) + 1);
  }
  return String(date.year) + "-" + twoDigit(date.month) + "-" + twoDigit(date.day);
}

static CivilDate parseDateKey(const String &value) {
  if (value.length() < 10) {
    return {1970, 1, 1};
  }
  return {
      value.substring(0, 4).toInt(),
      value.substring(5, 7).toInt(),
      value.substring(8, 10).toInt(),
  };
}

static int daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

static CivilDate civilFromDays(int days) {
  days += 719468;
  const int era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int year = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
  return {year, static_cast<int>(month), static_cast<int>(day)};
}

static String dateKeyFromCivil(const CivilDate &date) {
  char buffer[11];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month, date.day);
  return String(buffer);
}

static int weekdayFromDays(int days) {
  int weekday = (days + 4) % 7;
  if (weekday < 0) {
    weekday += 7;
  }
  return weekday;
}

static bool sameEventDay(JsonObjectConst event, const String &dateKey) {
  return dateKeyFromIso(jsonString(event["startsAt"])) == dateKey;
}

static void drawText(int16_t x,
                     int16_t y,
                     const String &text,
                     int maxChars = 0,
                     TextSize size = TextSize::Small) {
  drawKorean(x, y, maxChars > 0 ? utf8Prefix(text, maxChars) : text, size);
}

static void drawInvertedText(int16_t x,
                             int16_t y,
                             int16_t width,
                             int16_t height,
                             const String &text,
                             int maxChars = 0,
                             TextSize size = TextSize::Bold) {
  display.fillRect(x, y - height + 5, width, height, GxEPD_BLACK);
  setKoreanTextColors(GxEPD_WHITE, GxEPD_BLACK);
  drawKorean(x + 5, y, maxChars > 0 ? utf8Prefix(text, maxChars) : text, size);
  setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
}

static void drawGridCellFrame(int16_t x,
                              int16_t y,
                              int16_t w,
                              int16_t h,
                              int row,
                              int col) {
  if (row == 0) {
    display.drawFastHLine(x, y, w, GxEPD_BLACK);
  }
  if (col == 0) {
    display.drawFastVLine(x, y, h, GxEPD_BLACK);
  }
  display.drawFastHLine(x, y + h, w, GxEPD_BLACK);
  display.drawFastVLine(x + w, y, h, GxEPD_BLACK);
}

static void drawBatteryIcon(int16_t x, int16_t y, int percent, bool charging) {
  display.drawRect(x, y, 34, 16, GxEPD_BLACK);
  display.fillRect(x + 35, y + 5, 3, 6, GxEPD_BLACK);
  const int fillWidth = constrain(percent, 0, 100) * 30 / 100;
  if (fillWidth > 0) {
    display.fillRect(x + 2, y + 2, fillWidth, 12, GxEPD_BLACK);
  }
  if (charging) {
    display.drawLine(x + 15, y + 2, x + 10, y + 9, GxEPD_BLACK);
    display.drawLine(x + 10, y + 9, x + 18, y + 9, GxEPD_BLACK);
    display.drawLine(x + 18, y + 9, x + 13, y + 15, GxEPD_BLACK);
  }
}

static void drawNativeHeader(JsonObjectConst root, const String &title, const DeviceTelemetry &telemetry) {
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GxEPD_BLACK);
  display.drawLine(0, 35, SCREEN_WIDTH, 35, GxEPD_BLACK);
  drawInvertedText(10, 25, 116, 25, title, 5);
  drawText(140, 24, String(screenPage + 1) + "/" + String(SCREEN_PAGE_COUNT));
  drawText(192, 24, formatIsoTime(jsonString(root["generatedAt"])));

  drawWifiSignalIcon(610, 27, telemetry.rssi);
  drawText(646, 24, telemetry.ssid.length() > 0 ? telemetry.ssid.substring(0, 7) : "Wi-Fi", 8);
  if (telemetry.batteryPercent >= 0) {
    drawBatteryIcon(742, 10, telemetry.batteryPercent, telemetry.batteryChargeState == "charging");
  }
}

static void drawSparkline(JsonArrayConst history, int16_t x, int16_t y, int16_t w, int16_t h) {
  if (history.size() < 2) {
    display.drawLine(x, y + h / 2, x + w, y + h / 2, GxEPD_BLACK);
    return;
  }

  float minValue = history[0].as<float>();
  float maxValue = minValue;
  for (JsonVariantConst value : history) {
    const float number = value.as<float>();
    if (number < minValue) minValue = number;
    if (number > maxValue) maxValue = number;
  }
  if (fabs(maxValue - minValue) < 0.0001f) {
    maxValue = minValue + 1.0f;
  }

  int previousX = x;
  int previousY = y + h - static_cast<int>((history[0].as<float>() - minValue) / (maxValue - minValue) * h);
  for (size_t i = 1; i < history.size(); i++) {
    const int currentX = x + static_cast<int>(i * w / (history.size() - 1));
    const int currentY = y + h - static_cast<int>((history[i].as<float>() - minValue) / (maxValue - minValue) * h);
    display.drawLine(previousX, previousY, currentX, currentY, GxEPD_BLACK);
    previousX = currentX;
    previousY = currentY;
  }
}

static float parseMarketNumber(const String &value, bool *ok = nullptr) {
  String normalized;
  normalized.reserve(value.length());
  for (int i = 0; i < value.length(); i++) {
    const char c = value[i];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
      normalized += c;
    }
  }

  if (normalized.length() == 0 || normalized == "-" || normalized == "+") {
    if (ok) *ok = false;
    return 0.0f;
  }

  if (ok) *ok = true;
  return normalized.toFloat();
}

static float stockPreviousClose(JsonObjectConst stock, bool *ok = nullptr) {
  bool priceOk = false;
  bool rateOk = false;
  const float latestPrice = parseMarketNumber(jsonString(stock["price"]), &priceOk);
  const float latestRate = parseMarketNumber(jsonString(stock["changePercent"]), &rateOk);
  if (priceOk && rateOk && latestRate > -99.0f) {
    if (ok) *ok = true;
    return latestPrice / (1.0f + latestRate / 100.0f);
  }

  JsonArrayConst history = stock["history"].as<JsonArrayConst>();
  if (history.size() > 0) {
    if (ok) *ok = true;
    return history[0].as<float>();
  }

  if (ok) *ok = false;
  return 0.0f;
}

static float stockPercentRange(JsonObjectConst stock) {
  bool ok = false;
  const float latestRate = fabs(parseMarketNumber(jsonString(stock["changePercent"]), &ok));
  const float padded = ok ? ceilf(max(1.0f, latestRate) * 1.35f) : 3.0f;
  return constrain(padded, 1.0f, 15.0f);
}

static int percentY(float percent, float range, int16_t y, int16_t h) {
  const float clamped = constrain(percent, -range, range);
  return y + h / 2 - static_cast<int>((clamped / range) * (h / 2 - 5));
}

static void drawChartGrid(int16_t x, int16_t y, int16_t w, int16_t h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  const int midY = y + h / 2;
  for (int px = x + 5; px < x + w - 5; px += 8) {
    display.drawFastHLine(px, midY, 4, GxEPD_BLACK);
  }
  for (int i = 1; i < 4; i++) {
    const int gx = x + (w * i) / 4;
    for (int py = y + 5; py < y + h - 5; py += 8) {
      display.drawFastVLine(gx, py, 3, GxEPD_BLACK);
    }
  }
}

static void drawPercentLineChart(JsonObjectConst stock, int16_t x, int16_t y, int16_t w, int16_t h) {
  JsonArrayConst history = stock["history"].as<JsonArrayConst>();
  drawChartGrid(x, y, w, h);
  if (history.size() < 2) {
    return;
  }

  bool baselineOk = false;
  const float baseline = stockPreviousClose(stock, &baselineOk);
  if (!baselineOk || fabs(baseline) < 0.0001f) {
    return;
  }

  const float range = stockPercentRange(stock);
  int previousX = x + 5;
  float firstPercent = ((history[0].as<float>() - baseline) / baseline) * 100.0f;
  int previousY = percentY(firstPercent, range, y, h);
  for (size_t i = 1; i < history.size(); i++) {
    const int currentX = x + 5 + static_cast<int>(i * (w - 10) / (history.size() - 1));
    const float percent = ((history[i].as<float>() - baseline) / baseline) * 100.0f;
    const int currentY = percentY(percent, range, y, h);
    display.drawLine(previousX, previousY, currentX, currentY, GxEPD_BLACK);
    previousX = currentX;
    previousY = currentY;
  }
}

static bool drawCandleChart(JsonObjectConst stock, int16_t x, int16_t y, int16_t w, int16_t h) {
  JsonArrayConst candles = stock["candles"].as<JsonArrayConst>();
  if (candles.size() < 2) {
    return false;
  }

  bool baselineOk = false;
  const float baseline = stockPreviousClose(stock, &baselineOk);
  if (!baselineOk || fabs(baseline) < 0.0001f) {
    return false;
  }

  drawChartGrid(x, y, w, h);
  const float range = stockPercentRange(stock);
  const int count = min(static_cast<int>(candles.size()), 36);
  const int start = static_cast<int>(candles.size()) - count;
  const int slot = max(5, (w - 12) / max(1, count));
  const int bodyW = max(2, min(6, slot - 2));

  for (int i = 0; i < count; i++) {
    JsonObjectConst candle = candles[start + i];
    const float open = candle["o"].as<float>();
    const float high = candle["h"].as<float>();
    const float low = candle["l"].as<float>();
    const float close = candle["c"].as<float>();
    if (open <= 0 || high <= 0 || low <= 0 || close <= 0) {
      continue;
    }

    const int cx = x + 6 + i * (w - 12) / max(1, count - 1);
    const int highY = percentY(((high - baseline) / baseline) * 100.0f, range, y, h);
    const int lowY = percentY(((low - baseline) / baseline) * 100.0f, range, y, h);
    const int openY = percentY(((open - baseline) / baseline) * 100.0f, range, y, h);
    const int closeY = percentY(((close - baseline) / baseline) * 100.0f, range, y, h);
    const int top = min(openY, closeY);
    const int bottom = max(openY, closeY);
    display.drawLine(cx, highY, cx, lowY, GxEPD_BLACK);
    if (close >= open) {
      display.drawRect(cx - bodyW / 2, top, bodyW, max(2, bottom - top), GxEPD_BLACK);
    } else {
      display.fillRect(cx - bodyW / 2, top, bodyW, max(2, bottom - top), GxEPD_BLACK);
    }
  }

  return true;
}

static String compactChartPrice(float value) {
  if (!isfinite(value) || value <= 0) {
    return "--";
  }
  if (value >= 1000.0f) {
    return String(static_cast<long>(round(value)));
  }
  if (value >= 100.0f) {
    return String(value, 1);
  }
  return String(value, 2);
}

static String removeNumberSeparators(String value) {
  value.replace(",", "");
  value.replace("，", "");
  return value;
}

static String stockDirectionWord(JsonObjectConst stock) {
  const String direction = jsonString(stock["direction"]);
  if (direction == "up") return "상승";
  if (direction == "down") return "하락";
  if (direction == "flat") return "보합";
  return "변동";
}

static String signedStockValue(JsonObjectConst stock, const String &value, const String &suffix = "") {
  String cleaned = removeNumberSeparators(value);
  while (cleaned.startsWith("+") || cleaned.startsWith("-")) {
    cleaned.remove(0, 1);
  }
  const String direction = jsonString(stock["direction"]);
  if (direction == "up") return "+" + cleaned + suffix;
  if (direction == "down") return "-" + cleaned + suffix;
  return cleaned + suffix;
}

// Price scale on the right of the chart: +range / previous close / -range.
static void drawChartPriceAxis(JsonObjectConst stock, int16_t x, int16_t y, int16_t w, int16_t h) {
  bool baselineOk = false;
  const float baseline = stockPreviousClose(stock, &baselineOk);
  if (!baselineOk || fabs(baseline) < 0.0001f) {
    return;
  }

  const float range = stockPercentRange(stock);
  const float prices[] = {
      baseline * (1.0f + range / 100.0f),
      baseline,
      baseline * (1.0f - range / 100.0f),
  };
  const float percents[] = {range, 0.0f, -range};

  for (int i = 0; i < 3; i++) {
    const int tickY = percentY(percents[i], range, y, h);
    display.drawFastHLine(x, tickY, 3, GxEPD_BLACK);
    drawText(x + 5, tickY + 3, removeNumberSeparators(compactChartPrice(prices[i])), 0, TextSize::Micro);
  }
}

// Time labels only; prices live on the y-axis.
static void drawChartPointLabels(JsonObjectConst stock,
                                 int16_t x,
                                 int16_t y,
                                 int16_t w,
                                 int16_t h) {
  (void)h;
  JsonArrayConst candles = stock["candles"].as<JsonArrayConst>();
  if (candles.size() >= 2) {
    const int count = min(static_cast<int>(candles.size()), 36);
    const int start = static_cast<int>(candles.size()) - count;
    const int indexes[] = {start, start + (count - 1) / 2, start + count - 1};

    for (int i = 0; i < 3; i++) {
      JsonObjectConst candle = candles[indexes[i]];
      const String time = jsonString(candle["t"], "--:--");
      const int16_t textW = measureKorean(time, TextSize::Micro);
      const int labelX[] = {x, x + w / 2 - textW / 2, x + w - textW};
      drawText(labelX[i], y, time, 8, TextSize::Micro);
    }
    return;
  }

  JsonArrayConst history = stock["history"].as<JsonArrayConst>();
  if (history.size() < 2) {
    return;
  }

  const char *labels[] = {"시작", "중간", "현재"};
  for (int i = 0; i < 3; i++) {
    const int16_t textW = measureKorean(labels[i], TextSize::Micro);
    const int labelX[] = {x, x + w / 2 - textW / 2, x + w - textW};
    drawText(labelX[i], y, labels[i], 8, TextSize::Micro);
  }
}

static void setupSdAssets() {
  if (!ENABLE_SD_ASSETS || sdAssetsInitialized) {
    return;
  }
  sdAssetsInitialized = true;

  pinMode(SD_EN, OUTPUT);
  digitalWrite(SD_EN, HIGH);
  delay(20);
  pinMode(SD_DET, INPUT_PULLUP);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  if (!SD.begin(SD_CS, SPI, 20000000)) {
    Serial.println("SD mount failed; retrying with SD_EN low");
    digitalWrite(SD_EN, LOW);
    delay(20);
    if (!SD.begin(SD_CS, SPI, 20000000)) {
      Serial.println("SD assets unavailable");
      sdAssetsAvailable = false;
      return;
    }
  }

  sdAssetsAvailable = true;
  Serial.printf("SD assets mounted: type=%u, size=%lluMB\n",
                static_cast<unsigned>(SD.cardType()),
                static_cast<unsigned long long>(SD.cardSize() / (1024ULL * 1024ULL)));
  SD.mkdir("/eink");
  SD.mkdir("/eink/icons");
  SD.mkdir("/eink/fonts");
}

enum class WeatherGlyph {
  Clear,
  PartlyCloudy,
  Cloudy,
  Fog,
  Rain,
  Snow,
  Thunder,
};

static WeatherGlyph weatherGlyphForCode(int code) {
  if (code == 0) return WeatherGlyph::Clear;
  if (code == 1 || code == 2) return WeatherGlyph::PartlyCloudy;
  if (code == 45 || code == 48) return WeatherGlyph::Fog;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WeatherGlyph::Snow;
  if (code >= 95 && code <= 99) return WeatherGlyph::Thunder;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WeatherGlyph::Rain;
  return WeatherGlyph::Cloudy;
}

// s(x): scale a coordinate designed on a 32px grid to the requested size.
static void drawSunShape(int16_t x, int16_t y, int size, int16_t cx, int16_t cy, int16_t r) {
  const auto s = [size](int v) { return static_cast<int16_t>(v * size / 32); };
  const int16_t px = x + s(cx);
  const int16_t py = y + s(cy);
  const int16_t pr = max<int16_t>(3, s(r));
  display.fillCircle(px, py, pr, GxEPD_BLACK);
  display.fillCircle(px, py, pr - max<int16_t>(2, pr / 3), GxEPD_WHITE);
  const int16_t inner = pr + max<int16_t>(2, s(3));
  const int16_t outer = inner + max<int16_t>(2, s(4));
  for (int i = 0; i < 8; i++) {
    const float angle = i * PI / 4.0f;
    const float dx = cosf(angle);
    const float dy = sinf(angle);
    for (int t = -1; t <= 0; t++) {
      display.drawLine(px + static_cast<int16_t>(dx * inner) + t,
                       py + static_cast<int16_t>(dy * inner),
                       px + static_cast<int16_t>(dx * outer) + t,
                       py + static_cast<int16_t>(dy * outer),
                       GxEPD_BLACK);
    }
  }
}

// Solid cloud with a white inset so the outline reads as a bold 2px stroke.
static void drawCloudShape(int16_t x, int16_t y, int size, int16_t offsetY) {
  const auto s = [size](int v) { return static_cast<int16_t>(v * size / 32); };
  const int16_t oy = s(offsetY);
  display.fillCircle(x + s(10), y + oy + s(15), s(6), GxEPD_BLACK);
  display.fillCircle(x + s(18), y + oy + s(11), s(8), GxEPD_BLACK);
  display.fillCircle(x + s(25), y + oy + s(15), s(5), GxEPD_BLACK);
  display.fillRect(x + s(8), y + oy + s(15), s(19), s(6), GxEPD_BLACK);
  display.fillCircle(x + s(10), y + oy + s(15), s(4), GxEPD_WHITE);
  display.fillCircle(x + s(18), y + oy + s(11), s(6), GxEPD_WHITE);
  display.fillCircle(x + s(25), y + oy + s(15), s(3), GxEPD_WHITE);
  display.fillRect(x + s(10), y + oy + s(14), s(15), s(5), GxEPD_WHITE);
}

static void drawWeatherIcon(int16_t x, int16_t y, int code, int size = 32) {
  const auto s = [size](int v) { return static_cast<int16_t>(v * size / 32); };
  const WeatherGlyph glyph = weatherGlyphForCode(code);

  switch (glyph) {
    case WeatherGlyph::Clear:
      drawSunShape(x, y, size, 16, 16, 7);
      break;
    case WeatherGlyph::PartlyCloudy:
      drawSunShape(x, y, size, 21, 10, 5);
      drawCloudShape(x, y, size, 8);
      break;
    case WeatherGlyph::Cloudy:
      drawCloudShape(x, y, size, 2);
      break;
    case WeatherGlyph::Fog:
      drawCloudShape(x, y, size, -4);
      for (int i = 0; i < 3; i++) {
        const int16_t lineY = y + s(21 + i * 4);
        display.fillRect(x + s(5 + (i % 2) * 3), lineY, s(22), max<int16_t>(1, s(2)), GxEPD_BLACK);
      }
      break;
    case WeatherGlyph::Rain:
      drawCloudShape(x, y, size, -2);
      for (int i = 0; i < 3; i++) {
        const int16_t dropX = x + s(9 + i * 8);
        const int16_t dropY = y + s(23);
        for (int t = 0; t <= 1; t++) {
          display.drawLine(dropX + t, dropY, dropX - s(3) + t, dropY + s(7), GxEPD_BLACK);
        }
      }
      break;
    case WeatherGlyph::Snow:
      drawCloudShape(x, y, size, -2);
      for (int i = 0; i < 3; i++) {
        const int16_t cx = x + s(9 + i * 8);
        const int16_t cy = y + s(26);
        display.drawLine(cx - s(2), cy, cx + s(2), cy, GxEPD_BLACK);
        display.drawLine(cx, cy - s(2), cx, cy + s(2), GxEPD_BLACK);
        display.drawLine(cx - s(2), cy - s(2), cx + s(2), cy + s(2), GxEPD_BLACK);
        display.drawLine(cx - s(2), cy + s(2), cx + s(2), cy - s(2), GxEPD_BLACK);
      }
      break;
    case WeatherGlyph::Thunder: {
      drawCloudShape(x, y, size, -2);
      const int16_t bx = x + s(15);
      const int16_t by = y + s(20);
      display.fillTriangle(bx, by, bx + s(5), by, bx - s(2), by + s(7), GxEPD_BLACK);
      display.fillTriangle(bx + s(4), by + s(4), bx - s(1), by + s(5), bx + s(1), by + s(12), GxEPD_BLACK);
      break;
    }
  }
}

static bool overviewMarketMatch(JsonObjectConst stock, int slot) {
  const String name = jsonString(stock["name"]);
  const String code = jsonString(stock["code"]);
  if (slot == 0) {
    return name == "KOSPI" || code == "^KS11";
  }
  if (slot == 1) {
    return name == "KOSDAQ" || code == "^KQ11";
  }
  return name.indexOf("WTI") >= 0 || code == "CL=F";
}

static bool drawOverviewMarketItem(JsonArrayConst stocks, int slot, int16_t y) {
  for (JsonObjectConst stock : stocks) {
    if (!overviewMarketMatch(stock, slot)) {
      continue;
    }

    drawText(458, y, jsonString(stock["name"]), 10);
    drawText(458, y + 18, jsonString(stock["price"], "--"), 10);
    drawText(548, y + 18, jsonString(stock["changePercent"], "--") + "%", 8);
    drawSparkline(stock["history"].as<JsonArrayConst>(), 638, y - 11, 116, 34);
    return true;
  }
  return false;
}

static void drawOverviewPage(JsonObjectConst root) {
  JsonObjectConst weather = root["weather"];
  drawText(24, 72, "오늘 요약", 0, TextSize::Large);
  drawWeatherIcon(28, 92, weather["weatherCode"].isNull() ? 3 : weather["weatherCode"].as<int>(), 48);
  drawText(92, 118, jsonString(weather["label"]) + " " + formatValue(weather["temperatureC"], "C"), 0,
           TextSize::Large);
  drawText(92, 148, jsonString(weather["condition"], "날씨 정보 없음"), 18);
  drawText(28, 196, "체감 " + formatValue(weather["apparentTemperatureC"], "C"));
  drawText(190, 196, "습도 " + formatValue(weather["humidityPercent"], "%"));
  drawText(350, 196, "바람 " + formatValue(weather["windKph"], "km/h"));

  display.drawLine(20, 226, SCREEN_WIDTH - 20, 226, GxEPD_BLACK);
  drawText(28, 258, "다가오는 일정", 0, TextSize::Large);
  JsonArrayConst events = root["events"];
  int y = 292;
  for (size_t i = 0; i < events.size() && i < 3; i++) {
    JsonObjectConst event = events[i];
    drawText(36, y, formatIsoTime(jsonString(event["startsAt"])) + " " + jsonString(event["title"]), 28);
    drawText(56, y + 24, jsonString(event["calendarName"], "캘린더"), 22);
    y += 54;
  }

  drawText(450, 258, "시장", 0, TextSize::Large);
  JsonArrayConst stocks = root["stocks"];
  for (int i = 0; i < 3; i++) {
    const int rowY = 292 + i * 48;
    if (!drawOverviewMarketItem(stocks, i, rowY)) {
      const char *fallbackLabel = i == 0 ? "KOSPI" : (i == 1 ? "KOSDAQ" : "WTI");
      drawText(458, rowY, fallbackLabel);
      drawText(458, rowY + 18, "--");
      display.drawLine(638, rowY + 6, 754, rowY + 6, GxEPD_BLACK);
    }
  }
}

static void drawWeatherPage(JsonObjectConst root) {
  JsonObjectConst weather = root["weather"];
  JsonArrayConst days = weather["daily"];
  const int top = 38;
  const int cellW = SCREEN_WIDTH / 2;
  const int cellH = (SCREEN_HEIGHT - top - 2) / 4;
  for (size_t i = 0; i < days.size() && i < 8; i++) {
    JsonObjectConst day = days[i];
    const int col = i % 2;
    const int row = i / 2;
    const int x = col * cellW;
    const int y = top + row * cellH;
    drawGridCellFrame(x, y, cellW, cellH, row, col);

    drawText(x + 10, y + 24, jsonString(day["date"]).substring(5), 5, TextSize::Bold);
    drawWeatherIcon(x + 10, y + 38, day["weatherCode"].isNull() ? 3 : day["weatherCode"].as<int>());
    drawText(x + 50, y + 54, jsonString(day["condition"]), 12, TextSize::Bold);
    drawText(x + 50, y + 77,
             formatValue(day["minTemperatureC"], "C") + "-" +
                 formatValue(day["maxTemperatureC"], "C"),
             10);
    drawText(x + 154, y + 77,
             "강수 " + formatValue(day["precipitationProbabilityPercent"], "%"),
             8);

    JsonArrayConst hourly = day["hourly"];
    int hx = x + 244;
    for (size_t h = 0; h < hourly.size() && h < 3; h++) {
      JsonObjectConst hour = hourly[h];
      drawText(hx, y + 30, formatIsoTime(jsonString(hour["time"])), 5, TextSize::Tiny);
      drawWeatherIcon(hx, y + 38, hour["weatherCode"].isNull() ? 3 : hour["weatherCode"].as<int>());
      drawText(hx, y + 86, formatValue(hour["temperatureC"], "C"), 5, TextSize::Tiny);
      hx += 48;
    }
  }
}

static void drawMonthCalendarPage(JsonObjectConst root) {
  const CivilDate base = parseDateKey(dateKeyFromIso(jsonString(root["generatedAt"])));
  const int firstDay = daysFromCivil(base.year, base.month, 1);
  const int startDay = firstDay - weekdayFromDays(firstDay);
  const String todayKey = dateKeyFromCivil(base);
  JsonArrayConst events = root["events"];

  for (int i = 0; i < 7; i++) {
    drawText(52 + i * 112, 53, WEEKDAY_LABELS[i], 0, TextSize::Tiny);
  }

  const int top = 56;
  const int cellW = SCREEN_WIDTH / 7;
  const int cellH = (SCREEN_HEIGHT - top - 1) / 6;
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 7; col++) {
      const int x = col * cellW;
      const int y = top + row * cellH;
      const CivilDate date = civilFromDays(startDay + row * 7 + col);
      const String key = dateKeyFromCivil(date);
      display.drawRect(x, y, cellW + 1, cellH + 1, GxEPD_BLACK);
      if (key == todayKey) {
        display.fillRect(x + 1, y + 1, cellW - 2, 3, GxEPD_BLACK);
        display.fillRect(x + 1, y + 1, 3, cellH - 2, GxEPD_BLACK);
      }
      drawText(x + 3, y + 13, date.day == 1 ? String(date.month) + "월1일" : String(date.day), 5, TextSize::Tiny);

      int eventY = y + 26;
      int shown = 0;
      for (JsonObjectConst event : events) {
        if (!sameEventDay(event, key)) continue;
        const String time = event["allDay"].as<bool>() ? "" : formatIsoTime(jsonString(event["startsAt"])) + " ";
        drawText(x + 4, eventY, time + jsonString(event["title"]), 10, TextSize::Tiny);
        eventY += 13;
        shown++;
        if (shown >= 4) break;
      }
      if (events.size() > 0 && shown >= 4) {
        int remaining = 0;
        for (JsonObjectConst event : events) {
          if (sameEventDay(event, key)) remaining++;
        }
        if (remaining > shown) {
          drawText(x + 4, eventY, "+" + String(remaining - shown), 4, TextSize::Tiny);
        }
      }
    }
  }
}

static void drawWeekCalendarPage(JsonObjectConst root) {
  const CivilDate base = parseDateKey(dateKeyFromIso(jsonString(root["generatedAt"])));
  const int baseDay = daysFromCivil(base.year, base.month, base.day);
  const int startDay = baseDay - weekdayFromDays(baseDay);
  const String todayKey = dateKeyFromCivil(base);
  JsonArrayConst events = root["events"];
  const int top = 40;
  const int colW = SCREEN_WIDTH / 7;

  for (int col = 0; col < 7; col++) {
    const int x = col * colW;
    const CivilDate date = civilFromDays(startDay + col);
    const String key = dateKeyFromCivil(date);
    display.drawRect(x, top, colW + 1, SCREEN_HEIGHT - top - 2, GxEPD_BLACK);
    if (key == todayKey) {
      display.fillRect(x + 1, top + 1, colW - 2, 3, GxEPD_BLACK);
      display.fillRect(x + 1, top + 1, 3, SCREEN_HEIGHT - top - 4, GxEPD_BLACK);
    }
    drawText(x + 4, 58, String(WEEKDAY_LABELS[col]), 0, TextSize::Tiny);
    drawText(x + 4, 76, String(date.month) + "/" + String(date.day), 5, TextSize::Tiny);
    display.drawLine(x, 84, x + colW, 84, GxEPD_BLACK);

    int y = 102;
    int shown = 0;
    for (JsonObjectConst event : events) {
      if (!sameEventDay(event, key)) continue;
      const String time = event["allDay"].as<bool>() ? "" : formatIsoTime(jsonString(event["startsAt"])) + " ";
      drawText(x + 5, y, time + jsonString(event["title"]), 10, TextSize::Tiny);
      y += 14;
      shown++;
      if (shown >= 25) break;
    }
    if (shown == 0) {
      drawText(x + 14, 274, "일정 없음", 5, TextSize::Tiny);
    }
  }
}

static String flowValue(JsonVariantConst value) {
  if (value.isNull()) return "--";
  const long number = value.as<long>();
  const String sign = number > 0 ? "+" : "";
  if (abs(number) >= 10000) {
    return sign + String(number / 10000) + "만";
  }
  return sign + String(number);
}

static void drawStocksPage(JsonObjectConst root) {
  JsonArrayConst stocks = root["stocks"];
  const int tileW = SCREEN_WIDTH / 3;
  const int tileH = (SCREEN_HEIGHT - 38) / 4;
  for (int i = 0; i < 12; i++) {
    const int col = i % 3;
    const int row = i / 3;
    const int x = col * tileW;
    const int y = 38 + row * tileH;
    drawGridCellFrame(x, y, tileW, tileH, row, col);
    if (i >= static_cast<int>(stocks.size())) continue;
    JsonObjectConst stock = stocks[i];
    drawInvertedText(x + 1, y + 24, tileW - 1, 24, jsonString(stock["name"]), 10);
    drawText(x + 8, y + 54, removeNumberSeparators(jsonString(stock["price"], "--")), 12, TextSize::Bold);
    drawText(x + 138,
             y + 54,
             stockDirectionWord(stock) + " " + signedStockValue(stock, jsonString(stock["changePercent"], "--"), "%"),
             11,
             TextSize::Bold);
    drawText(x + 8,
             y + 76,
             "변동 " + signedStockValue(stock, jsonString(stock["change"], "--")),
             14,
             TextSize::Tiny);
    JsonObjectConst flow = stock["investorFlow"];
    if (!flow.isNull()) {
      drawText(x + 8, y + 98, "개인 " + flowValue(flow["retail"]), 10, TextSize::Tiny);
      drawText(x + 92, y + 98, "기관 " + flowValue(flow["institutional"]), 10, TextSize::Tiny);
      drawText(x + 176, y + 98, "외인 " + flowValue(flow["foreign"]), 10, TextSize::Tiny);
    }
  }
}

static void drawStockChartTile(JsonObjectConst stock,
                               int indexInPage,
                               int globalIndex,
                               int16_t x,
                               int16_t y,
                               int16_t w,
                               int16_t h) {
  const int col = indexInPage % 2;
  const int row = indexInPage / 2;
  drawGridCellFrame(x, y, w, h, row, col);
  drawText(x + 10, y + 24, String(globalIndex + 1) + ". " + jsonString(stock["name"]), 14, TextSize::Bold);
  drawText(x + 10, y + 46,
           removeNumberSeparators(jsonString(stock["price"], "--")) + "  " +
               signedStockValue(stock, jsonString(stock["changePercent"], "--"), "%"),
           18);

  const int axisW = 42;
  const int chartX = x + 10;
  const int chartY = y + 58;
  const int chartW = w - 20 - axisW;
  const int chartH = h - 122;
  if (!drawCandleChart(stock, chartX, chartY, chartW, chartH)) {
    drawPercentLineChart(stock, chartX, chartY, chartW, chartH);
  }
  drawChartPriceAxis(stock, chartX + chartW, chartY, axisW, chartH);
  drawChartPointLabels(stock, chartX, chartY + chartH + 14, chartW, 16);

  JsonObjectConst flow = stock["investorFlow"];
  if (!flow.isNull()) {
    const int flowY = y + h - 16;
    drawText(x + 10, flowY, "개인 " + flowValue(flow["retail"]), 10, TextSize::Tiny);
    drawText(x + 130, flowY, "기관 " + flowValue(flow["institutional"]), 10, TextSize::Tiny);
    drawText(x + 250, flowY, "외인 " + flowValue(flow["foreign"]), 10, TextSize::Tiny);
  } else {
    drawText(x + 10, y + h - 16, "점선: 전일 종가 0% 기준", 16, TextSize::Tiny);
  }
}

static void drawStockChartsPage(JsonObjectConst root, int pageGroup) {
  JsonArrayConst stocks = root["stocks"];
  const int top = 38;
  const int cellW = SCREEN_WIDTH / 2;
  const int cellH = (SCREEN_HEIGHT - top - 2) / 2;
  const int start = pageGroup * 4;
  for (int i = 0; i < 4; i++) {
    const int stockIndex = start + i;
    const int col = i % 2;
    const int row = i / 2;
    const int x = col * cellW;
    const int y = top + row * cellH;
    if (stockIndex >= static_cast<int>(stocks.size())) {
      drawGridCellFrame(x, y, cellW, cellH, row, col);
      drawText(x + 18, y + 104, "표시할 항목 없음", 0, TextSize::Bold);
      continue;
    }
    drawStockChartTile(stocks[stockIndex], i, stockIndex, x, y, cellW, cellH);
  }
}

static bool renderDashboard(JsonObjectConst root, const DeviceTelemetry &telemetry, bool partialRefresh) {
  display.setRotation(0);
  if (partialRefresh) {
    display.setPartialWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  } else {
    display.setFullWindow();
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    const int page = ((screenPage % SCREEN_PAGE_COUNT) + SCREEN_PAGE_COUNT) % SCREEN_PAGE_COUNT;
    if (page == 0) {
      drawNativeHeader(root, "요약", telemetry);
      drawOverviewPage(root);
    } else if (page == 1) {
      drawNativeHeader(root, "주간날씨", telemetry);
      drawWeatherPage(root);
    } else if (page == 2) {
      drawNativeHeader(root, "캘린더", telemetry);
      drawMonthCalendarPage(root);
    } else if (page == 3) {
      drawNativeHeader(root, "주간일정", telemetry);
      drawWeekCalendarPage(root);
    } else if (page == 4) {
      drawNativeHeader(root, "시장지표", telemetry);
      drawStocksPage(root);
    } else if (page == 5) {
      drawNativeHeader(root, "차트1", telemetry);
      drawStockChartsPage(root, 0);
    } else if (page == 6) {
      drawNativeHeader(root, "차트2", telemetry);
      drawStockChartsPage(root, 1);
    } else {
      drawNativeHeader(root, "차트3", telemetry);
      drawStockChartsPage(root, 2);
    }
  } while (display.nextPage());

  return true;
}

static void setupDisplay() {
  if (!ENABLE_DISPLAY || displayInitialized) {
    return;
  }

    SPI.begin(EPD_SCK, SD_MISO, EPD_MOSI, EPD_CS);
    Serial.println("Initializing e-paper display");
    display.init(115200, true, 2, false);
    koreanFonts.begin(display);
    koreanFonts.setFontMode(1);
    koreanFonts.setFontDirection(0);
    setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
    if (BOOT_TEST_SECONDS > 0) {
      Serial.println("Drawing boot test screen");
      drawBootTest();
      delay(BOOT_TEST_SECONDS * 1000UL);
    } else {
      Serial.println("Skipping boot test screen");
    }
    setupSdAssets();
    displayInitialized = true;
}

// Last successfully fetched dashboard JSON. Page transitions re-render from
// this cache instead of re-downloading, which is the main speedup for button
// navigation. Only used when deep sleep is disabled (RAM survives).
static std::unique_ptr<uint8_t[]> cachedDashboardJson;
static int cachedDashboardJsonSize = 0;
static uint32_t cachedDashboardJsonAt = 0;
constexpr uint32_t DASHBOARD_CACHE_TTL_MS = 30UL * 60UL * 1000UL;

static bool renderDashboardFromCache(bool partialDisplayRefresh) {
  if (!cachedDashboardJson || cachedDashboardJsonSize <= 0) {
    return false;
  }
  if (millis() - cachedDashboardJsonAt > DASHBOARD_CACHE_TTL_MS) {
    Serial.println("Dashboard cache expired");
    return false;
  }

  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(
      document,
      reinterpret_cast<const char *>(cachedDashboardJson.get()),
      static_cast<size_t>(cachedDashboardJsonSize));
  if (jsonError) {
    return false;
  }

  Serial.println("Rendering page from cached dashboard JSON");
  const DeviceTelemetry telemetry = readDeviceTelemetry();
  if (!renderDashboard(document.as<JsonObjectConst>(), telemetry, partialDisplayRefresh)) {
    return false;
  }
  deviceState = "screen-updated";
  return true;
}

static bool refreshScreen(bool forceServerRefresh = false, bool partialDisplayRefresh = false) {
  lastContentLength = 0;
  lastReceivedBytes = 0;
  lastFetchAttempt = 0;
  setLastError(0, "");

  if (!ENABLE_DISPLAY) {
    deviceState = "display-disabled";
    Serial.println("Display disabled for serial/debug check");
  } else {
    setupDisplay();
  }

  if (!forceServerRefresh && ENABLE_DISPLAY && renderDashboardFromCache(partialDisplayRefresh)) {
    return true;
  }

  if (!connectWifi()) {
    deviceState = "wifi-failed";
    if (!ENABLE_DISPLAY) {
      return false;
    }
    drawStatus("와이파이 연결 실패", fetchFailureDetail());
    return false;
  }

  const DeviceTelemetry telemetry = readDeviceTelemetry();
  deviceState = "wifi-connected";
  Serial.printf("Wi-Fi SSID: %s, RSSI: %ld dBm\n",
                telemetry.ssid.c_str(),
                static_cast<long>(telemetry.rssi));
  if (telemetry.batteryPercent >= 0) {
    Serial.printf("Battery: %ld%% %.2fV charge=%s\n",
                  static_cast<long>(telemetry.batteryPercent),
                  telemetry.batteryVoltage,
                  telemetry.batteryChargeState.c_str());
  } else {
    Serial.println("Battery telemetry disabled");
  }

  std::unique_ptr<uint8_t[]> jsonBuffer;
  int jsonSize = 0;
  if (!fetchJson(endpointWithTelemetry(telemetry, forceServerRefresh), jsonBuffer, jsonSize)) {
    deviceState = "fetch-failed";
    if (!ENABLE_DISPLAY) {
      return false;
    }
    drawStatus("화면 가져오기 실패", fetchFailureDetail());
    return false;
  }
  deviceState = "json-received";

  if (!ENABLE_DISPLAY) {
    Serial.println("Display disabled; skipping dashboard render");
    return true;
  }

  // Cache before parsing: passing a const pointer below keeps the buffer
  // intact (ArduinoJson mutates mutable buffers in zero-copy mode).
  cachedDashboardJson = std::move(jsonBuffer);
  cachedDashboardJsonSize = jsonSize;
  cachedDashboardJsonAt = millis();

  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(
      document,
      reinterpret_cast<const char *>(cachedDashboardJson.get()),
      static_cast<size_t>(jsonSize));
  if (jsonError) {
    cachedDashboardJson.reset();
    cachedDashboardJsonSize = 0;
    deviceState = "json-failed";
    setLastError(-1201, String("JSON 파싱 실패\n") + jsonError.c_str());
    drawStatus("데이터 해석 실패", fetchFailureDetail());
    return false;
  }

  Serial.printf("Display refresh mode: %s\n", partialDisplayRefresh ? "partial" : "full");
  if (!renderDashboard(document.as<JsonObjectConst>(), telemetry, partialDisplayRefresh)) {
    deviceState = "render-failed";
    drawStatus("화면 표시 실패", "JSON 렌더링을 확인하세요.");
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
  screenPage = loadSavedScreenPage();
  Serial.printf("Screen page: %d\n", screenPage);
  setupButtons();
  refreshScreen();
}

void loop() {
  const DeviceSettings settings = loadDeviceSettings();
  const WaitAction action = sleepOrWait(settings.refreshSeconds);
  if (action == WaitAction::PageRefresh) {
    const bool partialDisplayRefresh = shouldUsePartialRefreshForScreenTransition();
    refreshScreen(false, partialDisplayRefresh);
  } else if (action == WaitAction::ForceRefresh) {
    pageTransitionRefreshCount = 0;
    refreshScreen(true);
  } else if (action == WaitAction::WifiSetup) {
    startSettingsPortal();
    const bool partialDisplayRefresh = shouldUsePartialRefreshForScreenTransition();
    refreshScreen(true, partialDisplayRefresh);
  }
}
