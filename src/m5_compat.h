#ifndef M5_COMPAT_H
#define M5_COMPAT_H

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <LittleFS.h>
using fs::File;
using fs::FS;
#include <time.h>
#include <sys/time.h>
#include <Bounce2.h>

#ifdef ESP32_S3_LCD_316
  #include "panel_rgb.h"
  #define PIN_BTN_A 43
  #define PIN_BTN_B 44
  #define PIN_BL    6
#elif defined(ESP32_C6_LCD)
  #define PIN_BTN_A 4
  #define PIN_BTN_B 5
  #define PIN_BL    22
#else
  #define PIN_BTN_A 5
  #define PIN_BTN_B 6
  #define PIN_BTN_C 44  // D7 = GPIO44
  #define PIN_BL    43
#endif

// Color definitions
#ifndef BLACK
#define BLACK       0x0000
#define NAVY        0x000F
#define DARKGREEN   0x03E0
#define DARKCYAN    0x03EF
#define MAROON      0x7800
#define PURPLE      0x780F
#define OLIVE       0x7BE0
#define LIGHTGREY   0xC618
#define DARKGREY    0x7BEF
#define BLUE        0x001F
#define GREEN       0x07E0
#define CYAN        0x07FF
#define RED         0xF800
#define MAGENTA     0xF81F
#define YELLOW      0xFFE0
#define WHITE       0xFFFF
#define ORANGE      0xFD20
#define GREENYELLOW 0xAFE5
#define PINK        0xF81F
#endif

// Time structures used by BM8563 RTC in M5
struct RTC_TimeTypeDef {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
};
struct RTC_DateTypeDef {
  uint8_t WeekDay;
  uint8_t Month;
  uint8_t Date;
  uint16_t Year;
};

// Button compatibility using Bounce2
class CompatibilityButton {
private:
  Bounce debouncer;
  uint8_t pin;
  bool lastState;
  bool pressed;
  bool longPressed;
  uint32_t pressStartMs;
  bool isPressedFlag;

public:
  CompatibilityButton(uint8_t p) : pin(p), lastState(false), pressed(false), longPressed(false), pressStartMs(0), isPressedFlag(false) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    debouncer.attach(pin);
    debouncer.interval(25);
  }

  void update() {
    debouncer.update();
    bool currentVal = (debouncer.read() == LOW); // LOW = pressed for INPUT_PULLUP
    isPressedFlag = currentVal;
    
    // wasPressed / pressed check
    pressed = false;
    if (currentVal && !lastState) {
      pressed = true;
      pressStartMs = millis();
    }
    
    if (!currentVal) {
      longPressed = false;
    }
    
    lastState = currentVal;
  }

  bool isPressed() {
    return isPressedFlag;
  }

  bool wasPressed() {
    return pressed;
  }

  bool wasReleased() {
    return debouncer.rose(); // rose = LOW (pressed) to HIGH (released)
  }

  bool pressedFor(uint32_t ms) {
    if (isPressed() && (millis() - pressStartMs >= ms)) {
      return true;
    }
    return false;
  }
};

// RTC compatibility using standard ESP32 internal time functions
class CompatibilityRtc {
public:
  void GetTime(RTC_TimeTypeDef* tm) {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    tm->Hours = timeinfo ? timeinfo->tm_hour : 0;
    tm->Minutes = timeinfo ? timeinfo->tm_min : 0;
    tm->Seconds = timeinfo ? timeinfo->tm_sec : 0;
  }
  
  void GetDate(RTC_DateTypeDef* dt) {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    dt->WeekDay = timeinfo ? timeinfo->tm_wday : 0;
    dt->Month = timeinfo ? timeinfo->tm_mon + 1 : 1;
    dt->Date = timeinfo ? timeinfo->tm_mday : 1;
    dt->Year = timeinfo ? timeinfo->tm_year + 1900 : 2026;
  }
  
  void SetTime(RTC_TimeTypeDef* tm) {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    if (!timeinfo) return;
    timeinfo->tm_hour = tm->Hours;
    timeinfo->tm_min = tm->Minutes;
    timeinfo->tm_sec = tm->Seconds;
    time_t t_new = mktime(timeinfo);
    struct timeval tv = { .tv_sec = t_new, .tv_usec = 0 };
    settimeofday(&tv, NULL);
  }
  
  void SetDate(RTC_DateTypeDef* dt) {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    if (!timeinfo) return;
    timeinfo->tm_mon = dt->Month - 1;
    timeinfo->tm_mday = dt->Date;
    timeinfo->tm_wday = dt->WeekDay;
    timeinfo->tm_year = dt->Year - 1900;
    time_t t_new = mktime(timeinfo);
    struct timeval tv = { .tv_sec = t_new, .tv_usec = 0 };
    settimeofday(&tv, NULL);
  }
};

// Power management compatibility
class CompatibilityAxp {
public:
  void ScreenBreath(uint8_t level) {
#ifdef ESP32_S3_LCD_316
    panelBacklight((uint8_t)(level * 255 / 100));
#else
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, level > 0 ? HIGH : LOW);
#endif
  }
  void SetLDO2(bool state) {
#ifdef ESP32_S3_LCD_316
    panelBacklight(state ? 200 : 0);
#else
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, state ? HIGH : LOW);
#endif
  }
  float GetBatVoltage() {
    return 4.1f;
  }
  float GetBatCurrent() {
    return GetVBusVoltage() > 4.0f ? 100.0f : -50.0f;
  }
  float GetVBusVoltage() {
    return 5.0f;
  }
  int GetTempInAXP192() {
    return 28;
  }
  void PowerOff() {
    ESP.deepSleep(0);
  }
  uint8_t GetBtnPress() {
    return 0;
  }
};

// Buzzer compatibility
class CompatibilityBeep {
public:
  void begin() {}
  void update() {}
  void tone(uint16_t freq, uint16_t dur) {}
};

// IMU compatibility
class CompatibilityImu {
public:
  void Init() {}
  void getAccelData(float* ax, float* ay, float* az) {
    *ax = 0.0f;
    *ay = 0.0f;
    *az = 1.0f;
  }
};

// M5Compat manager class
class M5Compat {
public:
  TFT_eSPI Lcd;
  CompatibilityAxp Axp;
  CompatibilityBeep Beep;
  CompatibilityImu Imu;
  CompatibilityRtc Rtc;
  CompatibilityButton BtnA;
  CompatibilityButton BtnB;
#ifdef ESP32_S3_LCD_316
  CompatibilityButton BtnBoot;  // GPIO0 BOOT acts as secondary approve
#else
  CompatibilityButton BtnC;
#endif

#ifdef ESP32_S3_LCD_316
  M5Compat() : BtnA(PIN_BTN_A), BtnB(PIN_BTN_B), BtnBoot(0) {}
#else
  M5Compat() : BtnA(PIN_BTN_A), BtnB(PIN_BTN_B), BtnC(PIN_BTN_C) {}
#endif

  void begin() {
    Serial.begin(115200);
    delay(200);  // let USB-CDC settle

#ifdef ESP32_S3_LCD_316
    // RGB-parallel panel: initialise via esp_lcd driver, not TFT_eSPI.
    // Lcd.init() is intentionally skipped — Lcd is only used as a canvas
    // (TFT_eSprite base), never as a hardware SPI device.
    panelInit();   // sets up RGB panel + turns backlight on
    BtnA.begin();
    BtnB.begin();
    BtnBoot.begin();
#else
    // Backlight on FIRST so we see life even before SPI is ready
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);

#ifdef ESP32_C6_LCD
    // ESP32-C6: explicitly init FSPI before TFT_eSPI touches it.
    // TFT_eSPI falls back to the C3 driver which doesn't auto-init on C6.
    SPI.begin(7 /*SCLK*/, 2 /*MISO*/, 6 /*MOSI*/, 14 /*CS*/);

    // Manual RST pulse — guarantees ST7789 comes out of reset cleanly.
    pinMode(21, OUTPUT);   // RST
    digitalWrite(21, HIGH); delay(10);
    digitalWrite(21, LOW);  delay(20);
    digitalWrite(21, HIGH); delay(150);
#endif

    Lcd.init();
    Lcd.setRotation(0);
    // RED fill: if screen shows red, SPI is working.
    // If screen stays white/blank, SPI still not reaching ST7789.
    Lcd.fillScreen(TFT_RED);

    BtnA.begin();
    BtnB.begin();
    BtnC.begin();
#endif // ESP32_S3_LCD_316
  }

  void update() {
    BtnA.update();
    BtnB.update();
#ifdef ESP32_S3_LCD_316
    BtnBoot.update();
#else
    BtnC.update();
#endif
  }
};

extern M5Compat M5;

#endif
