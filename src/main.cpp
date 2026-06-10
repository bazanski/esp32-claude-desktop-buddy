#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_mac.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "layout.h"
#ifdef ESP32_S3_LCD_316
#include "panel_rgb.h"
#endif

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#include "buddy_common.h"
#ifdef ESP32_S3_LCD_316
int W = 820, H = 320;
#elif defined(ESP32_C6_LCD)
int W = 172, H = 320;
#else
int W = 240, H = 240;
#endif
int CX = W / 2;
int CY_BASE = H / 2;

static inline int getSafeX(int y) {
#if defined(ESP32_S3_LCD_316) || defined(ESP32_C6_LCD)
  (void)y; return 4;
#else
  int dy = abs(y - 120);
  if (dy >= 120) return 12;
  float half_w = sqrtf(14400.0f - (float)(dy * dy));
  return 120 - (int)half_w + 12;
#endif
}
#ifdef ESP32_C6_LCD
#define LED_PIN 10          // LED on C6-LCD, active-low
#elif !defined(ESP32_S3_LCD_316)
#define LED_PIN 21          // yellow/orange LED on seeed_xiao_esp32s3, active-low
#endif

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

#ifdef ESP32_S3_LCD_316
// fillRect/setTextColor store raw values; esp_lcd draw_bitmap byte-swaps each pixel.
// drawFastHLine/drawPixel pre-swap internally so they display the raw color correctly.
// For fillRect/setTextColor paths, pre-swap with sw() so double-swap → correct display.
static inline uint16_t sw(uint16_t c) { return (c >> 8) | (c << 8); }
static inline uint16_t dcol(uint16_t c) { return (c >> 8) | (c << 8); }
#else
static inline uint16_t dcol(uint16_t c) { return c; }
#endif

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
#ifndef ESP32_S3_LCD_316
bool     swallowBtnC = false;
#endif
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
#ifndef ESP32_S3_LCD_316
  // Landscape layout has a dedicated buddy zone — no peek compression needed.
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
#endif
  spr.fillSprite(0x0000);
  characterInvalidate();
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(dcol(p.textDim), dcol(PANEL));
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, dcol(p.textDim));
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, dcol(p.textDim));
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, dcol(PANEL));
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(dcol(sel ? p.text : p.textDim), dcol(PANEL));
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(dcol(p.textDim), dcol(PANEL));
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(dcol(vals[i-1] ? GREEN : p.textDim), dcol(PANEL));
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, dcol(PANEL));
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(dcol(sel ? p.text : p.textDim), dcol(PANEL));
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(dcol(HOT), dcol(PANEL));
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Axp.PowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, dcol(PANEL));
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(dcol(sel ? p.text : p.textDim), dcol(PANEL));
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    int clockY = H * 90 / 240;
    spr.fillRect(0, clockY, W, H - clockY, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, H * 7 / 12);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, H * 35 / 48);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, H * 5 / 6);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
#ifdef ESP32_S3_LCD_316
  const int X0 = BUDDY_ZONE_W;
  const int INC = 22;
#else
  const int X0 = 0;
  const int INC = 12;
#endif
  spr.setTextColor(dcol(p.text), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y), y); spr.print("Info");
  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
#ifdef ESP32_S3_LCD_316
  spr.setCursor(W - 48, y);
#else
  spr.setCursor(W - getSafeX(y) - 28, y);
#endif
  spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += INC;
  spr.setTextColor(dcol(p.body), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y), y); spr.print(section);
  y += INC;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(dcol(p.bg));
  spr.setTextSize(1);
  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
  spr.setCursor((W - 17 * 6) / 2, H * 13 / 48);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor((W - 17 * 6) / 2, H * 31 / 48);   spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(dcol(p.text), dcol(p.bg));
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, H * 5 / 12);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
#ifdef ESP32_S3_LCD_316
  const int X0 = BUDDY_ZONE_W;
  const int S = 2, LH = 18;
  spr.fillRect(X0, 0, W - X0, H, dcol(p.bg));
  int y = 4;
#else
  const int X0 = 0;
  const int S = 1, LH = 8;
  int top = H * 7 / 24;
  spr.fillRect(0, top, W, H - top, p.bg);
  int y = top + 2;
#endif
  spr.setTextSize(S);
  auto ln = [&](const char* fmt, ...) {
    char b[48]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(X0 + getSafeX(y), y); spr.print(b); y += LH;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += LH / 2;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += LH / 2;
    spr.setTextColor(dcol(p.text), dcol(p.bg));
    ln("Press BOOT on a prompt");
    ln("to approve from here.");
    y += LH / 2;
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(dcol(p.text), dcol(p.bg));    ln("BOOT");
    spr.setTextColor(dcol(p.textDim), dcol(p.bg)); ln("    next screen");
    ln("    approve prompt"); y += LH / 2;
    spr.setTextColor(dcol(p.text), dcol(p.bg));    ln("B   bottom edge");
    spr.setTextColor(dcol(p.textDim), dcol(p.bg)); ln("    next page");
    ln("    deny prompt");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += LH / 2;
    spr.setTextColor(dcol(p.text), dcol(p.bg));
    ln("LINK");
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat_mA = (int)M5.Axp.GetBatCurrent();
    int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(dcol(p.text), dcol(p.bg));
    spr.setTextSize(2);
    spr.setCursor(X0 + getSafeX(y), y);
    spr.printf("%d%%", pct);
    spr.setTextSize(S);
    spr.setTextColor(dcol(full ? GREEN : (charging ? HOT : p.textDim)), dcol(p.bg));
    spr.setCursor(X0 + getSafeX(y) + 56, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20 * S;

    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += LH / 2;

    spr.setTextColor(dcol(p.text), dcol(p.bg));
    ln("SYSTEM");
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(dcol(linked ? GREEN : (settings().bt ? HOT : p.textDim)), dcol(p.bg));
    spr.setTextSize(2);
    spr.setCursor(X0 + getSafeX(y), y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(S);
    y += 20 * S;

    spr.setTextColor(dcol(p.text), dcol(p.bg));
    ln("  %s", btName);
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += LH / 2;

    if (linked) {
      uint32_t age2 = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age2);
    } else if (settings().bt) {
      spr.setTextColor(dcol(p.text), dcol(p.bg));
      ln("TO PAIR");
      spr.setTextColor(dcol(p.textDim), dcol(p.bg));
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("made by");
    y += 4;
    spr.setTextColor(dcol(p.text), dcol(p.bg));
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("source");
    y += 4;
    spr.setTextColor(dcol(p.text), dcol(p.bg));
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(dcol(p.textDim), dcol(p.bg));
    ln("hardware");
    y += 4;
    ln("M5StickC Plus / S3");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
template<uint8_t COLS>
static uint8_t wrapInto(const char* in, char out[][COLS], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  int area = H * 13 / 40;
  spr.fillRect(0, H - area, W, area, p.bg);
  spr.drawFastHLine(0, H - area, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  int yVal = H - area + 4;
  spr.setCursor(getSafeX(yVal), yVal);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  int yTool = H - area + (toolLen <= 10 ? 14 : 18);
  spr.setCursor(getSafeX(yTool), yTool);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  int yHint1 = H - area + 34;
  spr.setCursor(getSafeX(yHint1), yHint1);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    int yHint2 = H - area + 42;
    spr.setCursor(getSafeX(yHint2), yHint2);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    int ySent = H - 18;
    spr.setCursor(getSafeX(ySent), ySent);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    int yAct = H - 18;
    spr.setCursor(getSafeX(yAct), yAct);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - getSafeX(yAct) - 48, yAct);
    spr.print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
#ifdef ESP32_S3_LCD_316
  const int X0 = BUDDY_ZONE_W;
  const int S = 2;
  spr.fillRect(X0, 0, W - X0, H, dcol(p.bg));
  int y = 30;
#else
  const int X0 = 0;
  const int S = 1;
  int top = H * 7 / 24;
  spr.fillRect(0, top, W, H - top, p.bg);
  int y = top + 16;
#endif
  spr.setTextSize(S);

  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y), y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  int heartStartX = X0 + getSafeX(y) + 48 * S;
  for (int i = 0; i < 4; i++) tinyHeart(heartStartX + i * 16 * S, y + 2, i < mood, moodCol);

  y += 20 * S;
  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y), y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  int circleStartX = X0 + getSafeX(y) + 32 * S;
  for (int i = 0; i < 10; i++) {
    int px = circleStartX + i * 9 * S;
    if (i < fed) spr.fillCircle(px, y + S, 2 * S, dcol(p.body));
    else         spr.drawCircle(px, y + S, 2 * S, p.textDim);
  }

  y += 20 * S;
  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y), y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  int rectStartX = X0 + getSafeX(y) + 48 * S;
  for (int i = 0; i < 5; i++) {
    int px = rectStartX + i * 13 * S;
    if (i < en) spr.fillRect(px, y - 2, 9 * S, 6 * S, dcol(enCol));
    else        spr.drawRect(px, y - 2, 9 * S, 6 * S, p.textDim);
  }

  y += 24 * S;
  spr.fillRoundRect(X0 + getSafeX(y), y - 2, 42 * S, 14 * S, 3 * S, dcol(p.body));
  spr.setTextColor(dcol(p.bg), dcol(p.body));
  spr.setCursor(X0 + getSafeX(y) + 5 * S, y + S); spr.printf("Lv %u", stats().level);

  y += 20 * S;
  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y), y);
  spr.printf("approved %u", stats().approvals);

  y += 10 * S;
  spr.setCursor(X0 + getSafeX(y), y);
  spr.printf("denied   %u", stats().denials);

  y += 10 * S;
  uint32_t nap = stats().napSeconds;
  spr.setCursor(X0 + getSafeX(y), y);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);

  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(X0 + getSafeX(yPx), yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 10 * S);
  tokFmt("today    ", tama.tokensToday, y + 20 * S);
}

static void drawPetHowTo(const Palette& p) {
#ifdef ESP32_S3_LCD_316
  const int X0 = BUDDY_ZONE_W;
  const int LH = 16, GAP = 6;
  spr.fillRect(X0, 0, W - X0, H, dcol(p.bg));
  spr.setTextSize(2);
  int y = 2;
#else
  const int LH = 9, GAP = 4;
  const int X0 = 0;
  int top = H * 7 / 24;
  spr.fillRect(0, top, W, H - top, p.bg);
  spr.setTextSize(1);
  int y = top + 2;
#endif
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(dcol(c), dcol(p.bg));
    spr.setCursor(X0 + getSafeX(y), y); spr.print(s); y += LH;
  };
  auto gap = [&]() { y += GAP; };

  y += 22;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "BOOT: screens / approve");
  ln(p.textDim, "B: page / deny");
}

void drawPet() {
  const Palette& p = characterPalette();

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
#ifdef ESP32_S3_LCD_316
  const int X0 = BUDDY_ZONE_W;
  int y = 2;
  spr.setTextSize(2);
#else
  const int X0 = 0;
  int y = H * 7 / 24;
  spr.setTextSize(1);
#endif
  spr.setTextColor(dcol(p.text), dcol(p.bg));
  spr.setCursor(X0 + getSafeX(y + 2), y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(dcol(p.textDim), dcol(p.bg));
#ifdef ESP32_S3_LCD_316
  spr.setCursor(W - 48, y + 2);
#else
  spr.setCursor(W - getSafeX(y + 2) - 28, y + 2);
#endif
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 3, LH = 8, WIDTH = 21;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    int yHUD = H - LH - 8;
    spr.setCursor(getSafeX(yHUD), yHUD);
    spr.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][52];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    int yHUD = H - AREA + 2 + i * LH;
    spr.setCursor(getSafeX(yHUD), yHUD);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    int yScroll = H - LH - 8;
    spr.setCursor(W - getSafeX(yScroll) - 18, yScroll);
    spr.printf("-%u", msgScroll);
  }
}

#ifdef ESP32_S3_LCD_316
// ── S3 approval card — right zone (x=BUDDY_ZONE_W..W, y=0..H) ───────────────
static void drawApprovalS3() {
  const Palette& p = characterPalette();
  const int XO = BUDDY_ZONE_W;   // 320
  const int ZW = W - XO;         // 500

  spr.fillRect(XO, 0, ZW, H, p.bg);
  spr.drawFastVLine(XO, 0, H, p.textDim);   // separator (pre-swaps internally)

  spr.setTextSize(2);
  int y = 6;
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextColor(sw(waited >= 10 ? HOT : p.textDim), p.bg);
  spr.setCursor(XO + 8, y);
  spr.printf("approve? %lus", (unsigned long)waited);
  y += 22;

  spr.setTextColor(sw(p.text), p.bg);
  spr.setCursor(XO + 8, y);
  spr.print(tama.promptTool);
  y += 22;

  // Hint — full right-zone width at textSize 2
  spr.setTextColor(sw(p.textDim), p.bg);
  static char hrows[4][42];
  uint8_t hn = wrapInto(tama.promptHint, hrows, 4, (uint8_t)((ZW - 16) / 12));
  for (uint8_t i = 0; i < hn; i++) {
    spr.setCursor(XO + 8, y); spr.print(hrows[i]); y += 22;
  }

  if (responseSent) {
    spr.setTextColor(sw(p.textDim), p.bg);
    spr.setCursor(XO + 8, y + 4); spr.print("sent...");
  }

  spr.setTextSize(2);
  spr.setTextColor(sw(GREEN), p.bg);
  spr.setCursor(XO + 8, H - 26);
  spr.print("BOOT:approve");
  spr.setTextColor(sw(HOT), p.bg);
  spr.setCursor(W - 6 * 12 - 8, H - 26);
  spr.print("B:deny");
  spr.setTextSize(1);
}

// ── S3 HUD — right zone (x=BUDDY_ZONE_W..W) ─────────────────────────────────
static void drawHudS3() {
  if (tama.promptId[0]) { drawApprovalS3(); return; }

  const int XO  = BUDDY_ZONE_W;  // 320
  const int ZW  = W - XO;        // 500

  spr.fillRect(XO, 0, ZW, H, 0x0000);
  spr.drawFastVLine(XO, 0, H, WHITE);   // vertical separator (pre-swaps)

  spr.setTextSize(2);
  int y = 6;
  bool linked = bleConnected();
  spr.setTextColor(sw(linked ? GREEN : ORANGE), 0x0000);
  spr.setCursor(XO + 8, y);
  spr.print(linked ? "connected" : "no claude");
  y += 22;

  if (!linked) return;

  spr.setTextSize(2);
  spr.setTextColor(sw(WHITE), 0x0000);
  spr.setCursor(XO + 8, y);
  spr.printf("%u run", tama.sessionsRunning);
  if (tama.sessionsWaiting > 0) {
    spr.setTextColor(sw(HOT), 0x0000);
    spr.printf("  %u wait", tama.sessionsWaiting);
  }
  y += 22;

  uint32_t tok = tama.tokensToday;
  spr.setTextColor(sw(LIGHTGREY), 0x0000);
  spr.setCursor(XO + 8, y);
  if      (tok >= 1000000) spr.printf("%.1fM tok", tok / 1000000.0f);
  else if (tok >= 1000)    spr.printf("%.1fK tok", tok / 1000.0f);
  else                     spr.printf("%lu tok",   (unsigned long)tok);
  y += 22;

  spr.drawFastHLine(XO + 8, y, ZW - 16, LIGHTGREY);  // pre-swaps
  y += 10;

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  const Palette& p = characterPalette();

  if (tama.nLines == 0) {
    spr.setTextColor(sw(LIGHTGREY), 0x0000);
    spr.setCursor(XO + 8, y); spr.print(tama.msg);
    return;
  }

  const int LH    = 20;
  const int WIDTH = (ZW - 16) / 12;  // chars per line at textSize 2 (~40)
  const int MAXR  = 64;
  int SHOW = (H - y - 4) / LH;
  if (SHOW > MAXR) SHOW = MAXR;

  spr.setTextSize(2);
  static char disp[64][42];
  static uint8_t srcOf[64];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < MAXR; i++) {
    uint8_t got = wrapInto(tama.lines[i], disp + nDisp,
                           (uint8_t)(MAXR - nDisp), (uint8_t)WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }
  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;
  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? sw(WHITE) : sw(p.textDim), 0x0000);
    spr.setCursor(XO + 8, y + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(sw(LIGHTGREY), 0x0000);
    spr.setCursor(W - 18, y + (SHOW - 1) * LH);
    spr.printf("-%u", msgScroll);
  }
}
#endif

void setup() {
  Serial.begin(115200);
  M5.begin();
  Serial.println("setup: M5.begin done");
#ifndef ESP32_S3_LCD_316
  M5.Lcd.setRotation(0);
#endif
  M5.Imu.Init();
  M5.Beep.begin();
  Serial.println("setup: starting BT");
  startBt();
  Serial.println("setup: BT done");
#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
#endif
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  Serial.printf("setup: createSprite(%d,%d) PSRAM=%s free=%u\n", W, H, psramFound()?"yes":"no", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  bool sprOk = spr.createSprite(W, H);
  Serial.printf("setup: sprite %s ptr=%p\n", sprOk?"ok":"FAILED", spr.getPointer());
#ifdef ESP32_S3_LCD_316
  spr.setSwapBytes(false);  // RGB panel blit is raw — no byte-swap needed
#endif
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // Always prefer the installed GIF character; user can cycle to ASCII via
  // nextPet(). NVS is used only for which ASCII species was last chosen.
  buddyMode = !gifAvailable;
  applyDisplayMode();

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  M5.Beep.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

#ifdef LED_PIN
  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }
#endif

#ifndef ESP32_S3_LCD_316
  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }
#endif

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()
#ifdef ESP32_S3_LCD_316
      || M5.BtnBoot.isPressed()
#else
      || M5.BtnC.isPressed()
#endif
     ) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
#ifndef ESP32_S3_LCD_316
      if (M5.BtnC.isPressed()) swallowBtnC = true;
#endif
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

#ifdef ESP32_S3_LCD_316
  // BOOT button (GPIO0): approve when prompt active, otherwise cycle display mode
  if (M5.BtnBoot.wasPressed()) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (!menuOpen && !settingsOpen && !resetOpen) {
      beep(1800, 30);
      displayMode = (displayMode + 1) % DISP_COUNT;
      applyDisplayMode();
    }
  }
#endif

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

#ifndef ESP32_S3_LCD_316
  // BtnC: reserved for future use (D7 / GPIO44)
  if (M5.BtnC.wasPressed()) {
    if (swallowBtnC) { swallowBtnC = false; }
    else {
      beep(1600, 40);
    }
  }
#endif

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff
#ifndef ESP32_S3_LCD_316
      || landscapeClock
#endif
     ) {
    // skip sprite render — face-down, powered off, or (non-S3) landscape clock
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
#ifndef ESP32_S3_LCD_316
  if (landscapeClock) {
    drawClock();
  } else
#endif
  if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
#ifdef ESP32_S3_LCD_316
    else drawHudS3();
#else
    else if (settings().hud) drawHUD();
#endif
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
#ifdef ESP32_S3_LCD_316
    // Separator drawn last every frame so no screen's fillRect can erase it.
    // Two pixels wide to reduce tearing visibility from software rotation.
    spr.drawFastVLine(BUDDY_ZONE_W,     0, H, WHITE);
    spr.drawFastVLine(BUDDY_ZONE_W + 1, 0, H, WHITE);
    panelBlit(spr.getPointer(), W, H);
#else
    spr.pushSprite(0, 0);
#endif
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Axp.ScreenBreath(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
