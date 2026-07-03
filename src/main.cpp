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

#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "config.h"

#ifndef BUTTON_DEBOUNCE_MS
#define BUTTON_DEBOUNCE_MS 30
#endif

#ifndef BUTTON_CHORD_GRACE_MS
#define BUTTON_CHORD_GRACE_MS 150
#endif

#ifndef WIFI_SETUP_HOLD_MS
#define WIFI_SETUP_HOLD_MS 1800
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

static const char WIFI_PASSWORD_CHARSET[] =
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!@#$%^&*()-_=+[]{}:;,.?/\\|~`'\" ";

GxEPD2_BW<EPD_MODEL, EPD_MODEL::HEIGHT> display(
    EPD_MODEL(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX koreanFonts;

constexpr int SCREEN_WIDTH = 800;
constexpr int SCREEN_HEIGHT = 480;
constexpr int SCREEN_BYTES_PER_ROW = SCREEN_WIDTH / 8;
constexpr int SCREEN_BITMAP_BYTES = SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT;
static const char *deviceState = "booting";
static int lastErrorCode = 0;
static bool displayInitialized = false;
RTC_DATA_ATTR static int screenPage = 0;

static void setupDisplay();
static void drawKorean(int16_t x, int16_t y, const String &text);

struct DeviceTelemetry {
  String ssid;
  int32_t rssi;
  int32_t batteryPercent;
  float batteryVoltage;
  String batteryChargeState;
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

  delay(BUTTON_DEBOUNCE_MS);
  return digitalRead(pin) == LOW;
}

enum class ButtonIntent {
  None,
  PageLeft,
  PageRight,
  PageChord,
};

static bool buttonHeld(int pin) {
  return digitalRead(pin) == LOW;
}

static void waitForButtonRelease(int pin) {
  while (buttonHeld(pin)) {
    delay(10);
  }
}

static void waitForPageButtonsRelease() {
  while (buttonHeld(BUTTON_LEFT_PIN) || buttonHeld(BUTTON_RIGHT_PIN)) {
    delay(10);
  }
}

static uint8_t heldTopButtonCount() {
  uint8_t count = 0;
  if (buttonHeld(BUTTON_LEFT_PIN)) {
    count++;
  }
  if (buttonHeld(BUTTON_RIGHT_PIN)) {
    count++;
  }
  if (buttonHeld(BUTTON_REFRESH_PIN)) {
    count++;
  }
  return count;
}

static void waitForTopButtonsRelease() {
  while (heldTopButtonCount() > 0) {
    delay(10);
  }
}

static bool setupButtonComboStarted() {
  if (!ENABLE_BUTTONS || heldTopButtonCount() == 0) {
    return false;
  }

  const uint32_t startedAt = millis();
  while (millis() - startedAt < BUTTON_CHORD_GRACE_MS) {
    if (heldTopButtonCount() >= 2) {
      delay(BUTTON_DEBOUNCE_MS);
      return heldTopButtonCount() >= 2;
    }

    if (heldTopButtonCount() == 0) {
      break;
    }

    delay(20);
  }

  return false;
}

static bool setupButtonComboHeldFor(uint32_t holdMs) {
  const uint32_t startedAt = millis();
  while (heldTopButtonCount() >= 2) {
    if (millis() - startedAt >= holdMs) {
      return true;
    }
    delay(50);
  }
  return false;
}

static bool setupButtonComboCurrentlyPressed() {
  if (!ENABLE_BUTTONS || heldTopButtonCount() < 2) {
    return false;
  }

  delay(BUTTON_DEBOUNCE_MS);
  return heldTopButtonCount() >= 2;
}

static ButtonIntent readPageButtonIntent() {
  if (!ENABLE_BUTTONS) {
    return ButtonIntent::None;
  }

  if (!buttonHeld(BUTTON_LEFT_PIN) && !buttonHeld(BUTTON_RIGHT_PIN)) {
    return ButtonIntent::None;
  }

  bool sawLeft = buttonHeld(BUTTON_LEFT_PIN);
  bool sawRight = buttonHeld(BUTTON_RIGHT_PIN);
  const uint32_t startedAt = millis();

  while (millis() - startedAt < BUTTON_CHORD_GRACE_MS) {
    sawLeft = sawLeft || buttonHeld(BUTTON_LEFT_PIN);
    sawRight = sawRight || buttonHeld(BUTTON_RIGHT_PIN);

    if (sawLeft && sawRight) {
      delay(BUTTON_DEBOUNCE_MS);
      return ButtonIntent::PageChord;
    }

    if (!buttonHeld(BUTTON_LEFT_PIN) && !buttonHeld(BUTTON_RIGHT_PIN)) {
      break;
    }

    delay(20);
  }

  if (sawLeft) {
    return ButtonIntent::PageLeft;
  }
  if (sawRight) {
    return ButtonIntent::PageRight;
  }
  return ButtonIntent::None;
}

static bool pageChordHeldFor(uint32_t holdMs) {
  const uint32_t startedAt = millis();
  while (buttonHeld(BUTTON_LEFT_PIN) && buttonHeld(BUTTON_RIGHT_PIN)) {
    if (millis() - startedAt >= holdMs) {
      return true;
    }
    delay(50);
  }
  return false;
}

static void drawStatus(const String &title, const String &detail) {
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.drawRect(10, 10, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, GxEPD_BLACK);
    drawKorean(32, 72, title);
    drawKorean(32, 116, detail);
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

enum class WifiButtonStage {
  NetworkSelect,
  PasswordInput,
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

static void setKoreanFont() {
  koreanFonts.setFont(u8g2_font_unifont_t_korean2);
  koreanFonts.setFontMode(1);
  koreanFonts.setFontDirection(0);
  koreanFonts.setForegroundColor(GxEPD_BLACK);
  koreanFonts.setBackgroundColor(GxEPD_WHITE);
}

static void drawKorean(int16_t x, int16_t y, const String &text) {
  setKoreanFont();
  koreanFonts.drawUTF8(x, y, text.c_str());
}

static void drawWifiButtonFrame(const String &apName) {
  display.fillScreen(GxEPD_WHITE);
  display.drawRect(10, 10, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, GxEPD_BLACK);
  display.drawLine(28, 104, SCREEN_WIDTH - 28, 104, GxEPD_BLACK);
  display.drawLine(28, SCREEN_HEIGHT - 104, SCREEN_WIDTH - 28, SCREEN_HEIGHT - 104, GxEPD_BLACK);

  drawKorean(28, 48, "와이파이 설정");
  drawKorean(28, 82, "좌/우 버튼으로 이동하고 확인 버튼으로 선택합니다.");
  drawKorean(28, SCREEN_HEIGHT - 86, "취소: 상단 버튼 2개를 길게 누르세요.");
  drawKorean(28, SCREEN_HEIGHT - 58, "휴대폰 설정도 가능: 와이파이에서 " + apName + " 연결");
  drawKorean(28, SCREEN_HEIGHT - 30, "브라우저에서 http://192.168.4.1 을 여세요.");
}

static void drawWifiButtonSetup(WifiButtonStage stage,
                                const String &apName,
                                const WifiNetworkEntry *networks,
                                int networkCount,
                                int selectedNetwork,
                                const String &password,
                                int passwordChoiceIndex,
                                bool partialRefresh) {
  if (!ENABLE_DISPLAY) {
    return;
  }

  display.setRotation(0);
  if (partialRefresh) {
    display.setPartialWindow(12, 108, SCREEN_WIDTH - 24, SCREEN_HEIGHT - 222);
  } else {
    display.setFullWindow();
  }
  display.firstPage();
  do {
    if (!partialRefresh) {
      drawWifiButtonFrame(apName);
    } else {
      display.fillRect(12, 108, SCREEN_WIDTH - 24, SCREEN_HEIGHT - 222, GxEPD_WHITE);
    }

    display.setTextColor(GxEPD_BLACK);

    if (stage == WifiButtonStage::NetworkSelect) {
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
            koreanFonts.setForegroundColor(GxEPD_WHITE);
            koreanFonts.setBackgroundColor(GxEPD_BLACK);
          } else {
            koreanFonts.setForegroundColor(GxEPD_BLACK);
            koreanFonts.setBackgroundColor(GxEPD_WHITE);
          }

          setKoreanFont();
          if (i == selectedNetwork) {
            koreanFonts.setForegroundColor(GxEPD_WHITE);
            koreanFonts.setBackgroundColor(GxEPD_BLACK);
          }
          koreanFonts.drawUTF8(32, y, networks[i].ssid.substring(0, 22).c_str());
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
        }
      }
    } else if (stage == WifiButtonStage::PasswordInput) {
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
    } else {
      drawKorean(28, 176, "저장했습니다. 기기를 다시 시작합니다.");
    }
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
      if (setupButtonComboStarted()) {
        Serial.println("Button: Wi-Fi setup combo detected");
        if (setupButtonComboHeldFor(WIFI_SETUP_HOLD_MS)) {
          Serial.println("Button: Wi-Fi setup");
          waitForTopButtonsRelease();
          return WaitAction::WifiSetup;
        }
        waitForTopButtonsRelease();
      }

      if (buttonPressed(BUTTON_REFRESH_PIN)) {
        Serial.println("Button: refresh");
        waitForButtonRelease(BUTTON_REFRESH_PIN);
        return WaitAction::ForceRefresh;
      }

      const ButtonIntent pageIntent = readPageButtonIntent();
      if (pageIntent == ButtonIntent::PageChord) {
        Serial.println("Button: page chord detected");
        if (pageChordHeldFor(WIFI_SETUP_HOLD_MS)) {
          Serial.println("Button: Wi-Fi setup");
          waitForPageButtonsRelease();
          return WaitAction::WifiSetup;
        }
        waitForPageButtonsRelease();
      } else if (pageIntent == ButtonIntent::PageLeft) {
        screenPage = (screenPage + SCREEN_PAGE_COUNT - 1) % SCREEN_PAGE_COUNT;
        Serial.printf("Button: page left -> %d\n", screenPage);
        waitForButtonRelease(BUTTON_LEFT_PIN);
        return WaitAction::Refresh;
      } else if (pageIntent == ButtonIntent::PageRight) {
        screenPage = (screenPage + 1) % SCREEN_PAGE_COUNT;
        Serial.printf("Button: page right -> %d\n", screenPage);
        waitForButtonRelease(BUTTON_RIGHT_PIN);
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

  const int scanCount = WiFi.scanNetworks();
  WifiNetworkEntry wifiNetworks[WIFI_SETUP_MAX_NETWORKS];
  const int networkCount = buildWifiNetworkList(scanCount, wifiNetworks, WIFI_SETUP_MAX_NETWORKS);
  Serial.printf("Wi-Fi setup AP: %s, open http://192.168.4.1\n", apName.c_str());
  Serial.printf("Wi-Fi setup networks: scanned=%d, shown=%d\n", scanCount, networkCount);
  setupDisplay();

  DNSServer dnsServer;
  WebServer server(80);
  bool saved = false;
  bool exitRequested = false;
  WifiButtonStage buttonStage = WifiButtonStage::NetworkSelect;
  String buttonPassword;
  int selectedNetwork = 0;
  int passwordChoiceIndex = 0;
  bool uiDirty = true;
  bool partialUiRefresh = false;
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
    server.send(200, "text/html", wifiSetupPage(wifiNetworks, networkCount, saved));
  });
  server.on("/save", HTTP_POST, [&]() {
    const String ssid = server.arg("ssid");
    const String password = server.arg("password");
    if (ssid.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "와이파이를 선택해야 합니다.");
      return;
    }

    saveWifiCredentials(ssid, password);
    saved = true;
    server.send(200, "text/html", wifiSetupPage(wifiNetworks, networkCount, true));
    Serial.printf("Wi-Fi credentials saved: %s\n", ssid.c_str());
  });
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  const uint32_t startedAt = millis();
  while (!saved && !exitRequested && millis() - startedAt < WIFI_SETUP_TIMEOUT_SECONDS * 1000UL) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (setupButtonComboCurrentlyPressed()) {
      Serial.println("Wi-Fi setup cancel combo detected");
      if (setupButtonComboHeldFor(WIFI_SETUP_HOLD_MS)) {
        exitRequested = true;
        Serial.println("Wi-Fi setup canceled by button combo");
      }
      waitForTopButtonsRelease();
      continue;
    }

    if (uiDirty) {
      Serial.printf("Wi-Fi setup UI redraw: stage=%d, partial=%s\n",
                    static_cast<int>(buttonStage),
                    partialUiRefresh ? "yes" : "no");
      drawWifiButtonSetup(buttonStage,
                          apName,
                          wifiNetworks,
                          networkCount,
                          selectedNetwork,
                          buttonPassword,
                          passwordChoiceIndex,
                          partialUiRefresh);
      uiDirty = false;
      partialUiRefresh = true;
    }

    if (buttonPressed(BUTTON_LEFT_PIN)) {
      if (buttonStage == WifiButtonStage::NetworkSelect) {
        moveNetworkSelection(-1);
        Serial.printf("Wi-Fi setup button network: %s (%ld%%)\n",
                      wifiNetworks[selectedNetwork].ssid.c_str(),
                      static_cast<long>(wifiSignalPercentFromRssi(wifiNetworks[selectedNetwork].rssi)));
      } else if (buttonStage == WifiButtonStage::PasswordInput) {
        const int choiceCount = strlen(WIFI_PASSWORD_CHARSET) + 2;
        passwordChoiceIndex = (passwordChoiceIndex + choiceCount - 1) % choiceCount;
        Serial.printf("Wi-Fi setup password choice: %s\n",
                      wifiPasswordChoiceLabel(passwordChoiceIndex).c_str());
        lastPasswordConfirmIndex = -1;
      }
      waitForButtonRelease(BUTTON_LEFT_PIN);
      uiDirty = true;
    } else if (buttonPressed(BUTTON_RIGHT_PIN)) {
      if (buttonStage == WifiButtonStage::NetworkSelect) {
        moveNetworkSelection(1);
        Serial.printf("Wi-Fi setup button network: %s (%ld%%)\n",
                      wifiNetworks[selectedNetwork].ssid.c_str(),
                      static_cast<long>(wifiSignalPercentFromRssi(wifiNetworks[selectedNetwork].rssi)));
      } else if (buttonStage == WifiButtonStage::PasswordInput) {
        const int choiceCount = strlen(WIFI_PASSWORD_CHARSET) + 2;
        passwordChoiceIndex = (passwordChoiceIndex + 1) % choiceCount;
        Serial.printf("Wi-Fi setup password choice: %s\n",
                      wifiPasswordChoiceLabel(passwordChoiceIndex).c_str());
        lastPasswordConfirmIndex = -1;
      }
      waitForButtonRelease(BUTTON_RIGHT_PIN);
      uiDirty = true;
    } else if (buttonPressed(BUTTON_REFRESH_PIN)) {
      if (buttonStage == WifiButtonStage::NetworkSelect) {
        if (hasVisibleNetwork() && selectedNetwork < networkCount &&
            wifiNetworks[selectedNetwork].ssid.length() > 0) {
          buttonStage = WifiButtonStage::PasswordInput;
          buttonPassword = "";
          passwordChoiceIndex = 0;
          lastPasswordConfirmIndex = -1;
          lastPasswordConfirmAt = 0;
          Serial.printf("Wi-Fi setup button selected SSID: %s\n",
                        wifiNetworks[selectedNetwork].ssid.c_str());
        }
      } else if (buttonStage == WifiButtonStage::PasswordInput) {
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
              saved = true;
              buttonStage = WifiButtonStage::Saved;
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
      }
      waitForButtonRelease(BUTTON_REFRESH_PIN);
      uiDirty = true;
    }

    delay(20);
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.scanDelete();

  if (saved) {
    drawWifiButtonSetup(WifiButtonStage::Saved,
                        apName,
                        wifiNetworks,
                        networkCount,
                        selectedNetwork,
                        buttonPassword,
                        passwordChoiceIndex,
                        false);
    delay(800);
    ESP.restart();
  }

  if (exitRequested) {
    Serial.println("Wi-Fi setup exited by button");
    if (ENABLE_DISPLAY) {
      drawStatus("와이파이 설정", "취소했습니다. 대시보드로 돌아갑니다.");
    }
    return;
  }

  Serial.println("Wi-Fi setup timed out");
  if (ENABLE_DISPLAY) {
    drawStatus("와이파이 설정", "시간이 초과되었습니다. 대시보드로 돌아갑니다.");
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
    koreanFonts.begin(display);
    koreanFonts.setFontMode(1);
    koreanFonts.setFontDirection(0);
    koreanFonts.setForegroundColor(GxEPD_BLACK);
    koreanFonts.setBackgroundColor(GxEPD_WHITE);
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
    drawStatus("와이파이 연결 실패", "와이파이 이름과 비밀번호를 확인하세요.");
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

  std::unique_ptr<uint8_t[]> bitmapBuffer;
  int bitmapSize = 0;
  if (!fetchBitmap(endpointWithTelemetry(telemetry, forceServerRefresh), bitmapBuffer, bitmapSize)) {
    deviceState = "fetch-failed";
    if (!ENABLE_DISPLAY) {
      return false;
    }
    drawStatus("화면 가져오기 실패", "서버 주소, 토큰, Vercel 로그를 확인하세요.");
    return false;
  }
  deviceState = "bitmap-received";

  if (!ENABLE_DISPLAY) {
    Serial.println("Display disabled; skipping bitmap render");
    return true;
  }

  if (!renderBitmap(bitmapBuffer.get(), bitmapSize)) {
    deviceState = "render-failed";
    drawStatus("화면 표시 실패", "비트맵 주소와 크기를 확인하세요.");
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
