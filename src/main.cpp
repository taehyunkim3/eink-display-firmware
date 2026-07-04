#include <Arduino.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
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
#include "generated/nanum_gothic_coding_bitmap.h"

#ifndef BUTTON_DEBOUNCE_MS
#define BUTTON_DEBOUNCE_MS 30
#endif

#ifndef BUTTON_SCAN_INTERVAL_MS
#define BUTTON_SCAN_INTERVAL_MS 10
#endif

#ifndef BUTTON_CLICK_MIN_MS
#define BUTTON_CLICK_MIN_MS 60
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
RTC_DATA_ATTR static int screenPage = 0;
RTC_DATA_ATTR static uint32_t pageTransitionRefreshCount = 0;

static void setupDisplay();
static void drawKorean(int16_t x, int16_t y, const String &text);

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
      clampSetting(PAGE_FULL_REFRESH_INTERVAL, 1, 20),
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

  return {
      clampSetting(fullRefreshInterval, 1, 20),
      clampSetting(refreshSeconds, SETTINGS_REFRESH_SECONDS_MIN, SETTINGS_REFRESH_SECONDS_MAX),
  };
}

static void saveDeviceSettings(const DeviceSettings &settings) {
  Preferences preferences;
  preferences.begin("settings", false);
  preferences.putUInt("fullEvery", clampSetting(settings.fullRefreshInterval, 1, 20));
  preferences.putUInt("refreshSec",
                      clampSetting(settings.refreshSeconds,
                                   SETTINGS_REFRESH_SECONDS_MIN,
                                   SETTINGS_REFRESH_SECONDS_MAX));
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
    leftRightSeen_ = false;
    leftRightHoldEmitted_ = false;
    suppressLeftRightClicks_ = false;
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
      if (!leftRightSeen_) {
        leftRightSeen_ = true;
        bothDownStartedAt_ = now;
        suppressLeftRightClicks_ = true;
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

    const bool suppressLeftRightClicks = suppressLeftRightClicks_;
    if (event == ButtonEvent::None && left_.released && left_.releasedAfter >= BUTTON_CLICK_MIN_MS &&
        !suppressLeftRightClicks) {
      event = ButtonEvent::LeftClick;
    }
    if (event == ButtonEvent::None && right_.released &&
        right_.releasedAfter >= BUTTON_CLICK_MIN_MS && !suppressLeftRightClicks) {
      event = ButtonEvent::RightClick;
    }
    if (event == ButtonEvent::None && refresh_.released &&
        refresh_.releasedAfter >= BUTTON_CLICK_MIN_MS) {
      event = ButtonEvent::RefreshClick;
    }

    if (resetLeftRightAfterEvent) {
      leftRightSeen_ = false;
      leftRightHoldEmitted_ = false;
      suppressLeftRightClicks_ = false;
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
  bool leftRightSeen_ = false;
  bool leftRightHoldEmitted_ = false;
  bool suppressLeftRightClicks_ = false;
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

static const uint32_t FULL_REFRESH_OPTIONS[] = {1, 3, 5, 10, 20};
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
  if (interval <= 1) {
    return "항상 전체";
  }
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

static bool isNanumHangul(uint32_t codepoint) {
  return codepoint >= NANUM_HANGUL_START &&
         codepoint < NANUM_HANGUL_START + NANUM_HANGUL_COUNT;
}

static void drawNanumHangul(int16_t x, int16_t baseline, uint32_t codepoint) {
  const uint32_t glyphIndex = codepoint - NANUM_HANGUL_START;
  const uint32_t offset = glyphIndex * NANUM_GLYPH_BYTES;
  const int16_t top = baseline - NANUM_GLYPH_SIZE + 2;

  for (uint8_t row = 0; row < NANUM_GLYPH_SIZE; row++) {
    for (uint8_t byteIndex = 0; byteIndex < NANUM_GLYPH_BYTES_PER_ROW; byteIndex++) {
      const uint32_t byteOffset = offset + row * NANUM_GLYPH_BYTES_PER_ROW + byteIndex;
      const uint8_t bits = pgm_read_byte(&NANUM_HANGUL_BITMAPS[byteOffset]);
      for (uint8_t bit = 0; bit < 8; bit++) {
        const uint8_t col = byteIndex * 8 + bit;
        if (col >= NANUM_GLYPH_SIZE) {
          break;
        }
        if ((bits & (0x80 >> bit)) != 0) {
          display.drawPixel(x + col, top + row, koreanTextForeground);
        }
      }
    }
  }
}

static void drawKorean(int16_t x, int16_t y, const String &text) {
  setKoreanFont();
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
    if (codepoint == ' ') {
      cursorX += 8;
      continue;
    }
    if (isNanumHangul(codepoint)) {
      drawNanumHangul(cursorX, y, codepoint);
      cursorX += NANUM_GLYPH_SIZE;
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
        Serial.println("Button: refresh");
        return WaitAction::ForceRefresh;
      } else if (buttonEvent == ButtonEvent::LeftRightHold) {
        Serial.println("Button: settings chord hold");
        buttonManager.waitForAllReleased();
        return WaitAction::WifiSetup;
      } else if (buttonEvent == ButtonEvent::LeftClick) {
        screenPage = (screenPage + SCREEN_PAGE_COUNT - 1) % SCREEN_PAGE_COUNT;
        Serial.printf("Button: page left -> %d\n", screenPage);
        return WaitAction::PageRefresh;
      } else if (buttonEvent == ButtonEvent::RightClick) {
        screenPage = (screenPage + 1) % SCREEN_PAGE_COUNT;
        Serial.printf("Button: page right -> %d\n", screenPage);
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

static String formatValue(JsonVariantConst value, const String &suffix, int decimals = 0) {
  if (value.isNull()) {
    return "--";
  }
  if (decimals > 0) {
    return String(value.as<float>(), decimals) + suffix;
  }
  return String(static_cast<int>(round(value.as<float>()))) + suffix;
}

static String formatIsoTime(const String &value) {
  const int t = value.indexOf('T');
  if (t >= 0 && value.length() >= t + 6) {
    return value.substring(t + 1, t + 6);
  }
  return "";
}

static String dateKeyFromIso(const String &value) {
  return value.length() >= 10 ? value.substring(0, 10) : value;
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

static void drawText(int16_t x, int16_t y, const String &text, int maxChars = 0) {
  drawKorean(x, y, maxChars > 0 ? utf8Prefix(text, maxChars) : text);
}

static void drawInvertedText(int16_t x, int16_t y, int16_t width, int16_t height, const String &text, int maxChars = 0) {
  display.fillRect(x, y - height + 5, width, height, GxEPD_BLACK);
  setKoreanTextColors(GxEPD_WHITE, GxEPD_BLACK);
  drawKorean(x + 5, y, maxChars > 0 ? utf8Prefix(text, maxChars) : text);
  setKoreanTextColors(GxEPD_BLACK, GxEPD_WHITE);
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

static void drawWeatherIcon(int16_t x, int16_t y, int code) {
  if (code == 0 || code == 1) {
    display.fillCircle(x + 16, y + 16, 8, GxEPD_BLACK);
    display.drawLine(x + 16, y + 1, x + 16, y + 7, GxEPD_BLACK);
    display.drawLine(x + 16, y + 25, x + 16, y + 31, GxEPD_BLACK);
    display.drawLine(x + 1, y + 16, x + 7, y + 16, GxEPD_BLACK);
    display.drawLine(x + 25, y + 16, x + 31, y + 16, GxEPD_BLACK);
    display.drawLine(x + 5, y + 5, x + 9, y + 9, GxEPD_BLACK);
    display.drawLine(x + 23, y + 23, x + 27, y + 27, GxEPD_BLACK);
    display.drawLine(x + 27, y + 5, x + 23, y + 9, GxEPD_BLACK);
    display.drawLine(x + 9, y + 23, x + 5, y + 27, GxEPD_BLACK);
  } else if (code >= 51 && code <= 82) {
    display.fillCircle(x + 11, y + 13, 7, GxEPD_BLACK);
    display.fillCircle(x + 20, y + 11, 8, GxEPD_BLACK);
    display.fillRect(x + 6, y + 14, 23, 8, GxEPD_BLACK);
    display.drawLine(x + 8, y + 25, x + 5, y + 31, GxEPD_BLACK);
    display.drawLine(x + 17, y + 25, x + 14, y + 31, GxEPD_BLACK);
    display.drawLine(x + 26, y + 25, x + 23, y + 31, GxEPD_BLACK);
  } else {
    display.fillCircle(x + 11, y + 14, 7, GxEPD_BLACK);
    display.fillCircle(x + 20, y + 12, 9, GxEPD_BLACK);
    display.fillCircle(x + 26, y + 16, 6, GxEPD_BLACK);
    display.fillRect(x + 6, y + 16, 25, 8, GxEPD_BLACK);
  }
}

static void drawOverviewPage(JsonObjectConst root) {
  JsonObjectConst weather = root["weather"];
  drawText(24, 72, "오늘 요약");
  drawWeatherIcon(28, 98, weather["weatherCode"].isNull() ? 3 : weather["weatherCode"].as<int>());
  drawText(72, 120, jsonString(weather["label"]) + " " + formatValue(weather["temperatureC"], "C"));
  drawText(72, 148, jsonString(weather["condition"], "날씨 정보 없음"), 18);
  drawText(28, 196, "체감 " + formatValue(weather["apparentTemperatureC"], "C"));
  drawText(190, 196, "습도 " + formatValue(weather["humidityPercent"], "%"));
  drawText(350, 196, "바람 " + formatValue(weather["windKph"], "km/h"));

  display.drawLine(20, 226, SCREEN_WIDTH - 20, 226, GxEPD_BLACK);
  drawText(28, 258, "다가오는 일정");
  JsonArrayConst events = root["events"];
  int y = 292;
  for (size_t i = 0; i < events.size() && i < 3; i++) {
    JsonObjectConst event = events[i];
    drawText(36, y, formatIsoTime(jsonString(event["startsAt"])) + " " + jsonString(event["title"]), 28);
    drawText(56, y + 24, jsonString(event["calendarName"], "캘린더"), 22);
    y += 54;
  }

  drawText(450, 258, "시장");
  JsonArrayConst stocks = root["stocks"];
  y = 292;
  for (size_t i = 0; i < stocks.size() && i < 3; i++) {
    JsonObjectConst stock = stocks[i];
    drawText(458, y, jsonString(stock["name"]), 12);
    drawText(610, y, jsonString(stock["price"]), 12);
    drawText(610, y + 24, jsonString(stock["changePercent"]) + "%", 10);
    y += 54;
  }
}

static void drawWeatherPage(JsonObjectConst root) {
  JsonObjectConst weather = root["weather"];
  drawText(24, 68, jsonString(weather["label"]) + " 7일 예보");
  JsonArrayConst days = weather["daily"];
  int y = 92;
  for (size_t i = 0; i < days.size() && i < 7; i++) {
    JsonObjectConst day = days[i];
    display.drawRect(18, y - 18, SCREEN_WIDTH - 36, 50, GxEPD_BLACK);
    drawText(28, y + 2, jsonString(day["date"]).substring(5), 6);
    drawWeatherIcon(104, y - 14, day["weatherCode"].isNull() ? 3 : day["weatherCode"].as<int>());
    drawText(146, y + 2, jsonString(day["condition"]), 12);
    drawText(304, y + 2, formatValue(day["minTemperatureC"], "C") + "-" + formatValue(day["maxTemperatureC"], "C"));
    drawText(446, y + 2, "강수 " + formatValue(day["precipitationProbabilityPercent"], "%"));

    JsonArrayConst hourly = day["hourly"];
    int hx = 566;
    for (size_t h = 0; h < hourly.size() && h < 3; h++) {
      JsonObjectConst hour = hourly[h];
      drawText(hx, y - 8, formatIsoTime(jsonString(hour["time"])), 5);
      drawText(hx, y + 14, formatValue(hour["temperatureC"], "C"), 5);
      hx += 72;
    }
    y += 53;
  }
}

static void drawMonthCalendarPage(JsonObjectConst root) {
  const CivilDate base = parseDateKey(dateKeyFromIso(jsonString(root["generatedAt"])));
  const int firstDay = daysFromCivil(base.year, base.month, 1);
  const int startDay = firstDay - weekdayFromDays(firstDay);
  const String todayKey = dateKeyFromCivil(base);
  JsonArrayConst events = root["events"];

  for (int i = 0; i < 7; i++) {
    drawText(52 + i * 112, 55, WEEKDAY_LABELS[i]);
  }

  const int top = 62;
  const int cellW = SCREEN_WIDTH / 7;
  const int cellH = (SCREEN_HEIGHT - top - 4) / 6;
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 7; col++) {
      const int x = col * cellW;
      const int y = top + row * cellH;
      const CivilDate date = civilFromDays(startDay + row * 7 + col);
      const String key = dateKeyFromCivil(date);
      display.drawRect(x, y, cellW + 1, cellH + 1, GxEPD_BLACK);
      if (key == todayKey) {
        display.drawRect(x + 4, y + 4, cellW - 8, cellH - 8, GxEPD_BLACK);
        display.drawRect(x + 5, y + 5, cellW - 10, cellH - 10, GxEPD_BLACK);
      }
      drawText(x + 8, y + 22, date.day == 1 ? String(date.month) + "월 1일" : String(date.day), 6);

      int eventY = y + 44;
      int shown = 0;
      for (JsonObjectConst event : events) {
        if (!sameEventDay(event, key)) continue;
        display.fillRect(x + 8, eventY - 14, 3, 18, GxEPD_BLACK);
        drawText(x + 16, eventY, jsonString(event["title"]), 5);
        drawText(x + 16, eventY + 20, formatIsoTime(jsonString(event["startsAt"])), 5);
        eventY += 38;
        shown++;
        if (shown >= 2) break;
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
      display.drawRect(x + 4, top + 4, colW - 8, SCREEN_HEIGHT - top - 10, GxEPD_BLACK);
      display.drawRect(x + 5, top + 5, colW - 10, SCREEN_HEIGHT - top - 12, GxEPD_BLACK);
    }
    drawText(x + 8, 60, String(WEEKDAY_LABELS[col]));
    drawText(x + 8, 82, String(date.month) + "/" + String(date.day));
    display.drawLine(x, 92, x + colW, 92, GxEPD_BLACK);

    int y = 126;
    int shown = 0;
    for (JsonObjectConst event : events) {
      if (!sameEventDay(event, key)) continue;
      display.fillRect(x + 8, y - 15, 3, 20, GxEPD_BLACK);
      drawText(x + 16, y, jsonString(event["title"]), 5);
      drawText(x + 16, y + 22, formatIsoTime(jsonString(event["startsAt"])), 5);
      y += 56;
      shown++;
      if (shown >= 6) break;
    }
    if (shown == 0) {
      drawText(x + 22, 274, "일정 없음", 5);
    }
  }
}

static String flowValue(JsonVariantConst value) {
  if (value.isNull()) return "--";
  const long number = value.as<long>();
  if (abs(number) >= 10000) {
    return String(number / 10000) + "만";
  }
  return String(number);
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
    display.drawRect(x, y, tileW + 1, tileH + 1, GxEPD_BLACK);
    if (i >= static_cast<int>(stocks.size())) continue;
    JsonObjectConst stock = stocks[i];
    drawInvertedText(x + 1, y + 24, tileW - 1, 24, jsonString(stock["name"]), 10);
    drawText(x + 8, y + 52, jsonString(stock["price"]), 12);
    drawText(x + 150, y + 52, jsonString(stock["changePercent"]) + "%", 8);
    drawSparkline(stock["history"].as<JsonArrayConst>(), x + 10, y + 62, tileW - 20, 26);
    JsonObjectConst flow = stock["investorFlow"];
    if (!flow.isNull()) {
      drawText(x + 8, y + 104, "개인 " + flowValue(flow["retail"]), 9);
      drawText(x + 88, y + 104, "기관 " + flowValue(flow["institutional"]), 9);
      drawText(x + 168, y + 104, "외인 " + flowValue(flow["foreign"]), 9);
    }
  }
}

static void drawDevicePage(const DeviceTelemetry &telemetry) {
  drawText(28, 76, "기기상태");
  drawText(28, 118, String("Wi-Fi: ") + telemetry.ssid);
  drawText(28, 154, String("RSSI: ") + String(telemetry.rssi) + " dBm");
  if (telemetry.batteryPercent >= 0) {
    drawText(28, 190, String("Battery: ") + String(telemetry.batteryPercent) + "% " +
                         String(telemetry.batteryVoltage, 2) + "V");
  } else {
    drawText(28, 190, "Battery: --");
  }
  drawText(28, 226, String("Charge: ") + telemetry.batteryChargeState);
  drawText(28, 262, String("Page: ") + String(screenPage + 1) + "/" + String(SCREEN_PAGE_COUNT));
  drawText(28, 316, String("상태: ") + deviceState);
  drawText(28, 352, String("최근 오류: ") + String(lastErrorCode));
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
    } else {
      drawNativeHeader(root, "기기상태", telemetry);
      drawDevicePage(telemetry);
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
    displayInitialized = true;
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

  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(document, jsonBuffer.get(), jsonSize);
  if (jsonError) {
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
