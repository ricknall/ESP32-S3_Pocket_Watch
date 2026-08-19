#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include "BoardPins.h"

#if __has_include("WatchConfig.h")
#include "WatchConfig.h"
#else
#include "WatchConfig.example.h"
#endif

namespace {
constexpr char FIRMWARE_VERSION[] = "Pocket Watch Clock 0.4.0";
constexpr char CENTRAL_TIME_RULE[] = "CST6CDT,M3.2.0/2,M11.1.0/2";
constexpr uint8_t CST9217_ADDRESS = 0x5A;
constexpr uint8_t CST_ACK = 0xAB;
constexpr uint8_t AXP2101_ADDRESS = 0x34;
constexpr uint8_t AXP2101_EXPECTED_CHIP_ID = 0x4A;
constexpr uint8_t AXP2101_STATUS1 = 0x00;
constexpr uint8_t AXP2101_STATUS2 = 0x01;
constexpr uint8_t AXP2101_CHIP_ID = 0x03;
constexpr uint8_t AXP2101_ADC_CONTROL = 0x30;
constexpr uint8_t AXP2101_BATTERY_VOLTAGE_HIGH = 0x34;
constexpr uint8_t AXP2101_USB_VOLTAGE_HIGH = 0x38;
constexpr uint8_t AXP2101_SYSTEM_VOLTAGE_HIGH = 0x3A;
constexpr uint8_t AXP2101_BATTERY_DETECTION = 0x68;
constexpr uint8_t AXP2101_BATTERY_PERCENT = 0xA4;
constexpr uint16_t SCREEN_MAX = BoardPins::LCD_WIDTH - 1;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t NTP_TIMEOUT_MS = 15000;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 120;
constexpr uint32_t TOUCH_REPEAT_MS = 600;
constexpr uint32_t POWER_SAMPLE_INTERVAL_MS = 5000;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    BoardPins::LCD_CS, BoardPins::LCD_SCLK,
    BoardPins::LCD_SDIO0, BoardPins::LCD_SDIO1,
    BoardPins::LCD_SDIO2, BoardPins::LCD_SDIO3);

Arduino_CO5300 *display = new Arduino_CO5300(
    bus, BoardPins::LCD_RESET, 0,
    BoardPins::LCD_WIDTH, BoardPins::LCD_HEIGHT,
    6, 0, 0, 0);

struct TouchSample {
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  uint16_t screenX = 0;
  uint16_t screenY = 0;
};

struct PowerSample {
  bool valid = false;
  bool batteryConnected = false;
  bool usbPresent = false;
  bool charging = false;
  bool discharging = false;
  uint8_t rawStatus1 = 0;
  uint8_t rawStatus2 = 0;
  uint8_t chargeStage = 0;
  uint16_t batteryMillivolts = 0;
  uint16_t usbMillivolts = 0;
  uint16_t systemMillivolts = 0;
  int batteryPercent = -1;
};

bool displayReady = false;
bool touchReady = false;
bool powerReady = false;
bool clockSynced = false;
bool use24Hour = false;
bool powerDisplayDirty = false;
volatile bool touchInterruptPending = false;
uint32_t lastClockSecond = UINT32_MAX;
int lastRenderedHour = -1;
int lastRenderedMinute = -1;
int lastRenderedYearDay = -1;
bool lastRenderedUse24Hour = false;
char lastRenderedZone[8]{};
uint32_t lastAcceptedTouchMs = 0;
uint32_t lastPowerSampleMs = 0;
TouchSample lastAcceptedTouch{};
PowerSample powerSample{};

void IRAM_ATTR onTouchInterrupt() {
  touchInterruptPending = true;
}

bool credentialsConfigured() {
  return strcmp(WATCH_WIFI_SSID, "YOUR_WIFI_NAME") != 0 &&
         strlen(WATCH_WIFI_SSID) > 0 &&
         strcmp(WATCH_WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") != 0;
}

void drawCentered(const char *text, int16_t y, uint8_t size, uint16_t color,
                  bool opaque = false) {
  if (!displayReady) return;
  const int16_t width = static_cast<int16_t>(strlen(text) * 6 * size);
  const int16_t centeredX = (BoardPins::LCD_WIDTH - width) / 2;
  display->setTextSize(size);
  if (opaque) {
    display->setTextColor(color, RGB565_BLACK);
  } else {
    display->setTextColor(color);
  }
  display->setCursor(centeredX > 0 ? centeredX : 0, y);
  display->print(text);
}

void drawStatus(const char *line1, const char *line2 = nullptr,
                uint16_t color = RGB565_LIGHTGREY) {
  if (!displayReady) return;
  display->fillRect(55, 370, 356, 62, RGB565_BLACK);
  drawCentered(line1, line2 ? 375 : 392, 2, color);
  if (line2) drawCentered(line2, 403, 2, RGB565_LIGHTGREY);
}

void drawFaceFrame() {
  if (!displayReady) return;
  display->fillScreen(RGB565_BLACK);
  display->drawCircle(233, 233, 221, RGB565_BLUE);
  display->drawCircle(233, 233, 220, RGB565_BLUE);
  display->setTextWrap(false);
  drawCentered("GOD'S TIME", 42, 2, RGB565_CYAN);
  display->drawFastHLine(92, 76, 282, RGB565_DARKGREY);
  display->drawFastHLine(92, 357, 282, RGB565_DARKGREY);
}

bool currentLocalTime(tm &local) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &local);
  return true;
}

void drawClock(bool force = false) {
  if (!displayReady) return;

  tm local{};
  const bool valid = currentLocalTime(local);
  const uint32_t second = valid ? static_cast<uint32_t>(local.tm_sec) : 60;
  if (!force && second == lastClockSecond && !powerDisplayDirty) return;
  lastClockSecond = second;

  if (!valid) {
    display->fillRect(55, 90, 356, 257, RGB565_BLACK);
    lastRenderedHour = -1;
    lastRenderedMinute = -1;
    lastRenderedYearDay = -1;
    lastRenderedZone[0] = '\0';
    powerDisplayDirty = false;
    drawCentered("--:--", 142, 7, RGB565_WHITE);
    drawCentered("WAITING FOR NTP", 270, 2, RGB565_YELLOW);
    return;
  }

  int hour = local.tm_hour;
  char meridiem[3] = "";
  if (!use24Hour) {
    strcpy(meridiem, hour >= 12 ? "PM" : "AM");
    hour %= 12;
    if (hour == 0) hour = 12;
  }

  char timeText[8];
  snprintf(timeText, sizeof(timeText), use24Hour ? "%02d:%02d" : "%d:%02d",
           hour, local.tm_min);

  const bool modeChanged = use24Hour != lastRenderedUse24Hour;
  const bool minuteChanged = local.tm_hour != lastRenderedHour ||
                             local.tm_min != lastRenderedMinute;
  if (force || modeChanged || minuteChanged) {
    display->fillRect(55, 125, 356, 76, RGB565_BLACK);
    drawCentered(timeText, 130, 7, RGB565_WHITE);
    lastRenderedHour = local.tm_hour;
    lastRenderedMinute = local.tm_min;
  }

  char secondsText[16];
  if (use24Hour) {
    snprintf(secondsText, sizeof(secondsText), ":%02d", local.tm_sec);
  } else {
    snprintf(secondsText, sizeof(secondsText), "%s  :%02d", meridiem,
             local.tm_sec);
  }
  if (force || modeChanged) {
    display->fillRect(55, 202, 356, 34, RGB565_BLACK);
  }
  drawCentered(secondsText, 207, 3, RGB565_GREEN, true);

  if (force || local.tm_yday != lastRenderedYearDay) {
    display->fillRect(55, 263, 356, 71, RGB565_BLACK);

    char weekday[16];
    strftime(weekday, sizeof(weekday), "%A", &local);
    drawCentered(weekday, 269, 3, RGB565_CYAN);

    char dateText[24];
    strftime(dateText, sizeof(dateText), "%b %d, %Y", &local);
    drawCentered(dateText, 310, 2, RGB565_LIGHTGREY);
    lastRenderedYearDay = local.tm_yday;
  }

  char zone[8];
  strftime(zone, sizeof(zone), "%Z", &local);
  if (force || modeChanged || powerDisplayDirty ||
      strcmp(zone, lastRenderedZone) != 0) {
    if (powerReady && powerSample.valid) {
      char batteryLine[40];
      if (!powerSample.batteryConnected) {
        snprintf(batteryLine, sizeof(batteryLine),
                 "%s  NO BATTERY", powerSample.usbPresent ? "USB" : "PMU");
      } else if (powerSample.batteryPercent >= 0) {
        snprintf(batteryLine, sizeof(batteryLine), "BAT %d%%  %u.%02uV  %s",
                 powerSample.batteryPercent,
                 powerSample.batteryMillivolts / 1000,
                 (powerSample.batteryMillivolts % 1000) / 10,
                 powerSample.charging ? "CHG" :
                     (powerSample.usbPresent ? "USB" : "BAT"));
      } else {
        snprintf(batteryLine, sizeof(batteryLine), "BAT %u.%02uV  %s",
                 powerSample.batteryMillivolts / 1000,
                 (powerSample.batteryMillivolts % 1000) / 10,
                 powerSample.charging ? "CHG" :
                     (powerSample.usbPresent ? "USB" : "BAT"));
      }

      char hintLine[40];
      snprintf(hintLine, sizeof(hintLine), "%s  TAP: %s", zone,
               use24Hour ? "12-HOUR" : "24-HOUR");
      const uint16_t batteryColor =
          powerSample.batteryConnected && powerSample.batteryPercent >= 0 &&
                  powerSample.batteryPercent <= 15
              ? RGB565_RED
              : (powerSample.charging ? RGB565_CYAN : RGB565_GREEN);
      drawStatus(batteryLine, hintLine, batteryColor);
    } else {
      char status[40];
      snprintf(status, sizeof(status), "NTP SYNCED  %s", zone);
      drawStatus(status, use24Hour ? "TAP: 12-HOUR" : "TAP: 24-HOUR",
                 RGB565_GREEN);
    }
    snprintf(lastRenderedZone, sizeof(lastRenderedZone), "%s", zone);
    powerDisplayDirty = false;
  }

  lastRenderedUse24Hour = use24Hour;
}

bool pingAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool readPowerRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(AXP2101_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AXP2101_ADDRESS, static_cast<size_t>(1)) != 1) {
    while (Wire.available()) Wire.read();
    return false;
  }
  value = static_cast<uint8_t>(Wire.read());
  return true;
}

bool writePowerRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(AXP2101_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readPowerVoltage(uint8_t highRegister, uint8_t highMask,
                      uint16_t &millivolts) {
  uint8_t high = 0;
  uint8_t low = 0;
  if (!readPowerRegister(highRegister, high) ||
      !readPowerRegister(highRegister + 1, low)) {
    return false;
  }
  millivolts = static_cast<uint16_t>((high & highMask) << 8) | low;
  return true;
}

const char *chargeStageName(uint8_t stage) {
  switch (stage) {
    case 0: return "TRICKLE";
    case 1: return "PRE-CHARGE";
    case 2: return "CONSTANT CURRENT";
    case 3: return "CONSTANT VOLTAGE";
    case 4: return "CHARGE COMPLETE";
    case 5: return "NOT CHARGING";
    default: return "UNKNOWN";
  }
}

bool samplePower(PowerSample &sample) {
  uint8_t status1 = 0;
  uint8_t status2 = 0;
  if (!readPowerRegister(AXP2101_STATUS1, status1) ||
      !readPowerRegister(AXP2101_STATUS2, status2)) {
    return false;
  }

  sample.rawStatus1 = status1;
  sample.rawStatus2 = status2;
  sample.batteryConnected = (status1 & 0x08) != 0;
  sample.usbPresent = (status1 & 0x20) != 0 && (status2 & 0x08) == 0;
  sample.charging = (status2 >> 5) == 0x01;
  sample.discharging = (status2 >> 5) == 0x02;
  sample.chargeStage = status2 & 0x07;
  sample.batteryPercent = -1;

  if (sample.batteryConnected) {
    if (!readPowerVoltage(AXP2101_BATTERY_VOLTAGE_HIGH, 0x1F,
                          sample.batteryMillivolts)) {
      return false;
    }
    uint8_t percent = 0;
    if (readPowerRegister(AXP2101_BATTERY_PERCENT, percent) && percent <= 100) {
      sample.batteryPercent = percent;
    }
  }

  if (sample.usbPresent &&
      !readPowerVoltage(AXP2101_USB_VOLTAGE_HIGH, 0x3F,
                        sample.usbMillivolts)) {
    return false;
  }
  if (!readPowerVoltage(AXP2101_SYSTEM_VOLTAGE_HIGH, 0x3F,
                        sample.systemMillivolts)) {
    return false;
  }

  sample.valid = true;
  return true;
}

bool initializePowerManager() {
  uint8_t chipId = 0;
  if (!readPowerRegister(AXP2101_CHIP_ID, chipId)) {
    Serial.println("AXP2101: power manager did not respond at I2C 0x34.");
    return false;
  }
  if (chipId != AXP2101_EXPECTED_CHIP_ID) {
    Serial.printf("AXP2101: unexpected electronic ID 0x%02X.\n", chipId);
    return false;
  }

  uint8_t detection = 0;
  uint8_t adcControl = 0;
  if (!readPowerRegister(AXP2101_BATTERY_DETECTION, detection) ||
      !readPowerRegister(AXP2101_ADC_CONTROL, adcControl)) {
    Serial.println("AXP2101: could not read measurement configuration.");
    return false;
  }

  // Only enable sensing. Charging current, charging voltage, and power rails
  // remain exactly as configured by the board.
  if (((detection & 0x01) == 0 &&
       !writePowerRegister(AXP2101_BATTERY_DETECTION, detection | 0x01)) ||
      ((adcControl & 0x0D) != 0x0D &&
       !writePowerRegister(AXP2101_ADC_CONTROL, adcControl | 0x0D))) {
    Serial.println("AXP2101: could not enable battery measurements.");
    return false;
  }

  delay(50);
  Serial.printf("AXP2101 electronic ID: 0x%02X; battery sensing enabled.\n",
                chipId);
  return true;
}

void refreshPowerSample(bool force = false) {
  if (!powerReady) return;
  const uint32_t now = millis();
  if (!force && now - lastPowerSampleMs < POWER_SAMPLE_INTERVAL_MS) return;
  lastPowerSampleMs = now;

  PowerSample updated{};
  if (!samplePower(updated)) {
    Serial.println("AXP2101: battery status read failed.");
    return;
  }

  const bool visibleChange =
      !powerSample.valid ||
      updated.batteryConnected != powerSample.batteryConnected ||
      updated.usbPresent != powerSample.usbPresent ||
      updated.charging != powerSample.charging ||
      updated.batteryPercent != powerSample.batteryPercent ||
      updated.batteryMillivolts / 10 != powerSample.batteryMillivolts / 10;
  powerSample = updated;
  if (visibleChange) powerDisplayDirty = true;
}

void printBatteryStatus() {
  Serial.println();
  Serial.println("--- BATTERY / POWER ---");
  if (!powerReady) {
    Serial.println("AXP2101: NOT AVAILABLE");
    return;
  }

  refreshPowerSample(true);
  if (!powerSample.valid) {
    Serial.println("AXP2101: telemetry unavailable");
    return;
  }

  Serial.println("Power manager: AXP2101 at I2C 0x34");
  Serial.printf("Battery connected: %s\n",
                powerSample.batteryConnected ? "YES" : "NO");
  Serial.printf("USB power present: %s\n", powerSample.usbPresent ? "YES" : "NO");
  Serial.printf("Charging: %s\n", powerSample.charging ? "YES" : "NO");
  Serial.printf("Discharging: %s\n", powerSample.discharging ? "YES" : "NO");
  Serial.printf("Charge stage: %s\n", chargeStageName(powerSample.chargeStage));
  if (powerSample.batteryConnected) {
    Serial.printf("Battery voltage: %u mV\n", powerSample.batteryMillivolts);
    if (powerSample.batteryPercent >= 0) {
      Serial.printf("Battery level: %d%%\n", powerSample.batteryPercent);
    } else {
      Serial.println("Battery level: NOT YET AVAILABLE");
    }
  }
  if (powerSample.usbPresent) {
    Serial.printf("USB voltage: %u mV\n", powerSample.usbMillivolts);
  }
  Serial.printf("System voltage: %u mV\n", powerSample.systemMillivolts);
  Serial.printf("Raw PMU status: STATUS1=0x%02X STATUS2=0x%02X\n",
                powerSample.rawStatus1, powerSample.rawStatus2);
}

bool i2cWrite(const uint8_t *bytes, size_t length) {
  Wire.beginTransmission(CST9217_ADDRESS);
  Wire.write(bytes, length);
  return Wire.endTransmission() == 0;
}

bool i2cWriteRead(const uint8_t *command, size_t commandLength,
                  uint8_t *reply, size_t replyLength) {
  Wire.beginTransmission(CST9217_ADDRESS);
  Wire.write(command, commandLength);
  if (Wire.endTransmission() != 0) return false;

  const size_t received = Wire.requestFrom(CST9217_ADDRESS, replyLength);
  if (received != replyLength) {
    while (Wire.available()) Wire.read();
    return false;
  }
  return Wire.readBytes(reply, replyLength) == replyLength;
}

void resetTouchController() {
  pinMode(BoardPins::TOUCH_RESET, OUTPUT);
  digitalWrite(BoardPins::TOUCH_RESET, LOW);
  delay(30);
  digitalWrite(BoardPins::TOUCH_RESET, HIGH);
  delay(50);
  delay(1000);
  pinMode(BoardPins::TOUCH_INT, INPUT);
}

bool probeCst9217() {
  resetTouchController();
  if (!pingAddress(CST9217_ADDRESS)) return false;

  const uint8_t enterCommandMode[] = {0xD1, 0x01};
  if (!i2cWrite(enterCommandMode, sizeof(enterCommandMode))) return false;
  delay(10);

  uint8_t chipReply[4]{};
  const uint8_t chipCommand[] = {0xD2, 0x04};
  if (!i2cWriteRead(chipCommand, sizeof(chipCommand),
                    chipReply, sizeof(chipReply))) {
    return false;
  }

  const uint16_t projectId =
      static_cast<uint16_t>(chipReply[1] << 8) | chipReply[0];
  const uint16_t chipId =
      static_cast<uint16_t>(chipReply[3] << 8) | chipReply[2];
  Serial.printf("CST92xx electronic ID: chip=0x%04X project=0x%04X\n",
                chipId, projectId);
  return chipId == 0x9217 || chipId == 0x9220;
}

bool readTouch(TouchSample &sample) {
  uint8_t reply[15]{};
  const uint8_t readCommand[] = {0xD0, 0x00};
  if (!i2cWriteRead(readCommand, sizeof(readCommand), reply, sizeof(reply))) {
    return false;
  }

  const uint8_t acknowledge[] = {0xD0, 0x00, CST_ACK};
  if (!i2cWrite(acknowledge, sizeof(acknowledge))) return false;
  if (reply[6] != CST_ACK || (reply[5] & 0x7F) == 0) return false;
  if ((reply[0] & 0x0F) != 0x06) return false;

  sample.rawX = static_cast<uint16_t>((reply[1] << 4) | (reply[3] >> 4));
  sample.rawY = static_cast<uint16_t>((reply[2] << 4) | (reply[3] & 0x0F));
  const uint16_t boundedX = constrain(sample.rawX, 0, SCREEN_MAX);
  const uint16_t boundedY = constrain(sample.rawY, 0, SCREEN_MAX);
  sample.screenX = SCREEN_MAX - boundedX;
  sample.screenY = SCREEN_MAX - boundedY;
  return true;
}

bool acceptTouch(const TouchSample &sample) {
  const uint32_t now = millis();
  if (now - lastAcceptedTouchMs < TOUCH_DEBOUNCE_MS) return false;

  const bool nearPrevious =
      abs(static_cast<int>(sample.screenX) - lastAcceptedTouch.screenX) <= 8 &&
      abs(static_cast<int>(sample.screenY) - lastAcceptedTouch.screenY) <= 8;
  if (nearPrevious && now - lastAcceptedTouchMs < TOUCH_REPEAT_MS) return false;

  lastAcceptedTouch = sample;
  lastAcceptedTouchMs = now;
  return true;
}

void stopWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool syncClock() {
  if (!credentialsConfigured()) {
    Serial.println("Wi-Fi credentials are not configured.");
    drawClock(true);
    drawStatus("CONFIGURE WI-FI", "EDIT WatchConfig.h", RGB565_YELLOW);
    return false;
  }

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WATCH_WIFI_SSID);
  drawStatus("CONNECTING WI-FI", nullptr, RGB565_YELLOW);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WATCH_WIFI_SSID, WATCH_WIFI_PASSWORD);

  const uint32_t wifiStarted = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStarted < WIFI_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection timed out.");
    stopWiFi();
    drawClock(true);
    drawStatus("WI-FI FAILED", "SERIAL: sync TO RETRY", RGB565_RED);
    return false;
  }

  Serial.print("Wi-Fi connected; address ");
  Serial.println(WiFi.localIP());
  drawStatus("SETTING GOD'S TIME", nullptr, RGB565_CYAN);
  setenv("TZ", CENTRAL_TIME_RULE, 1);
  tzset();
  configTzTime(CENTRAL_TIME_RULE,
               "pool.ntp.org", "time.nist.gov", "time.google.com");

  const uint32_t ntpStarted = millis();
  while (time(nullptr) < 1700000000 && millis() - ntpStarted < NTP_TIMEOUT_MS) {
    delay(100);
  }

  clockSynced = time(nullptr) >= 1700000000;
  stopWiFi();
  if (clockSynced) {
    Serial.println("NTP synchronized; Wi-Fi switched off.");
    lastClockSecond = UINT32_MAX;
    drawClock(true);
  } else {
    Serial.println("NTP synchronization timed out.");
    drawClock(true);
    drawStatus("NTP FAILED", "SERIAL: sync TO RETRY", RGB565_RED);
  }
  return clockSynced;
}

void printStatus() {
  tm local{};
  Serial.println();
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
  Serial.printf("Display: %s\n", displayReady ? "CO5300 READY" : "FAILED");
  Serial.printf("Touch: %s\n", touchReady ? "CST9217 READY" : "FAILED");
  Serial.printf("Power manager: %s\n", powerReady ? "AXP2101 READY" : "FAILED");
  Serial.printf("Clock synchronized: %s\n", clockSynced ? "YES" : "NO");
  Serial.printf("Display mode: %s\n", use24Hour ? "24-hour" : "12-hour");
  if (currentLocalTime(local)) {
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %Z", &local);
    Serial.printf("Local time: %s\n", timestamp);
  }
  Serial.printf("Flash: %u bytes; PSRAM: %u bytes\n",
                ESP.getFlashChipSize(), ESP.getPsramSize());
  printBatteryStatus();
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  status - print clock and hardware state");
  Serial.println("  battery - print battery, USB, and charging information");
  Serial.println("  power  - same as battery");
  Serial.println("  sync   - reconnect Wi-Fi and repeat NTP synchronization");
  Serial.println("  12     - select 12-hour display");
  Serial.println("  24     - select 24-hour display");
  Serial.println("  help   - show this list");
  Serial.println("A deliberate screen tap toggles 12/24-hour display.");
}

void processCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (command.length() == 0) return;

  if (command == "status") {
    printStatus();
  } else if (command == "battery" || command == "power") {
    printBatteryStatus();
  } else if (command == "sync") {
    syncClock();
  } else if (command == "12") {
    use24Hour = false;
    lastClockSecond = UINT32_MAX;
    drawClock(true);
  } else if (command == "24") {
    use24Hour = true;
    lastClockSecond = UINT32_MAX;
    drawClock(true);
  } else if (command == "help") {
    printHelp();
  } else {
    Serial.printf("Unknown command: %s\n", command.c_str());
    printHelp();
  }
  Serial.print("> ");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("========================================");
  Serial.println(FIRMWARE_VERSION);
  Serial.println("Waveshare ESP32-S3-Touch-AMOLED-1.75C");
  Serial.println("Central Time: automatic CST/CDT");
  Serial.println("========================================");

  Wire.begin(BoardPins::I2C_SDA, BoardPins::I2C_SCL, 400000);

  // Touch is probed first because its reset is shared with the AMOLED reset.
  touchReady = probeCst9217();
  displayReady = display->begin();
  Serial.printf("CO5300 display initialization: %s\n",
                displayReady ? "SUCCESS" : "FAILED");
  if (displayReady) display->setBrightness(128);

  // Display initialization resets CST9217 again through the shared reset line.
  delay(200);
  touchInterruptPending = false;
  attachInterrupt(BoardPins::TOUCH_INT, onTouchInterrupt, FALLING);

  powerReady = initializePowerManager();
  if (powerReady) refreshPowerSample(true);

  drawFaceFrame();
  drawClock(true);
  syncClock();
  printStatus();
  printHelp();
  Serial.print("> ");
}

void loop() {
  if (Serial.available()) processCommand(Serial.readStringUntil('\n'));

  refreshPowerSample();

  if (touchReady && touchInterruptPending) {
    noInterrupts();
    touchInterruptPending = false;
    interrupts();

    TouchSample sample;
    bool pointAvailable = false;
    for (uint8_t attempt = 0; attempt < 3 && !pointAvailable; ++attempt) {
      pointAvailable = readTouch(sample);
      if (!pointAvailable) delay(2);
    }

    // Empty CST9217 IRQ packets are normal and intentionally remain silent.
    if (pointAvailable && acceptTouch(sample)) {
      use24Hour = !use24Hour;
      Serial.printf("TOUCH X=%u Y=%u; display changed to %s\n",
                    sample.screenX, sample.screenY,
                    use24Hour ? "24-hour" : "12-hour");
      lastClockSecond = UINT32_MAX;
      drawClock(true);
    }
  }

  drawClock();
  delay(2);
}
