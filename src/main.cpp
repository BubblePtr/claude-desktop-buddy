#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <SensorPCF85063.hpp>
#include <LittleFS.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

// ─── pin definitions ─────────────────────────────────────────────────────────
#define PIN_LCD_CS   12
#define PIN_LCD_SCK  11
#define PIN_LCD_D0    4
#define PIN_LCD_D1    5
#define PIN_LCD_D2    6
#define PIN_LCD_D3    7
#define PIN_I2C_SDA  15
#define PIN_I2C_SCL  14
#define PIN_TP_INT   21
#define PIN_BOOT      0

#define LCD_W_PHYS  368
#define LCD_H_PHYS  448

// ─── display stack ───────────────────────────────────────────────────────────
// Logical canvas is half the physical resolution; hwDisplayPush() upscales 2×.
static Arduino_ESP32QSPI _bus(PIN_LCD_CS, PIN_LCD_SCK,
                               PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
static Arduino_SH8601    _gfx(&_bus, GFX_NOT_DEFINED, 0, LCD_W_PHYS, LCD_H_PHYS);
Arduino_Canvas*          spr = nullptr;   // 184×224; extern in buddy/character

// ─── TCA9554 GPIO expander (bare I2C — no library needed) ────────────────────
#define EXP_ADDR 0x20
static uint8_t _expOut = 0;
static void expSet(uint8_t val) {
  _expOut = val;
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(0x01);   // output register
  Wire.write(val);
  Wire.endTransmission();
}
static void expInit() {
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(0x03);   // config register
  Wire.write(0x00);   // all pins as output
  Wire.endTransmission();
  expSet(0x00);
}
static void expResetSequence() {
  expSet(0x00);   // assert reset on LCD(bit0), touch(bit1); disable DSI pwr(bit2)
  delay(20);
  expSet(0x07);   // release resets, enable DSI power
  delay(100);
}

// ─── hardware objects ────────────────────────────────────────────────────────
static XPowersAXP2101 axp;
static SensorQMI8658  qmi;
static SensorPCF85063 pcf;

// ─── hardware accessor functions (used by data.h / xfer.h) ──────────────────
void hwRtcSet(int y, int mo, int d, int h, int m, int s) {
  pcf.setDateTime(y, mo, d, h, m, s);
}
int hwBatMv()  { return (int)axp.getBattVoltage(); }
int hwBatMa()  { return 0; }   // AXP2101 does not expose instantaneous current
int hwVBusMv() { return (int)axp.getVbusVoltage(); }

// ─── push 184×224 canvas to 368×448 display at 2× nearest-neighbor ──────────
static void hwDisplayPush() {
  uint16_t* fb = spr->getFramebuffer();
  static uint16_t rows[LCD_W_PHYS * 2];
  uint16_t* r0 = rows;
  uint16_t* r1 = rows + LCD_W_PHYS;
  const int W = spr->width(), H = spr->height();
  for (int y = 0; y < H; y++) {
    uint16_t* src = fb + y * W;
    for (int x = 0; x < W; x++) {
      uint16_t c  = src[x];
      r0[x*2]     = c; r0[x*2+1] = c;
      r1[x*2]     = c; r1[x*2+1] = c;
    }
    _gfx.draw16bitRGBBitmap(0, y * 2, rows, LCD_W_PHYS, 2);
  }
}

static uint8_t rgb565ToRgb332(uint16_t c) {
  uint8_t r = (c >> 13) & 0x07;
  uint8_t g = (c >> 8)  & 0x07;
  uint8_t b = (c >> 3)  & 0x03;
  return (uint8_t)((r << 5) | (g << 2) | b);
}

bool debugScreenshotWrite(Stream& out) {
  if (!spr) return false;

  const uint16_t* fb = spr->getFramebuffer();
  if (!fb) return false;

  const uint16_t w = spr->width();
  const uint16_t h = spr->height();
  const uint32_t totalBytes = (uint32_t)w * h;
  const size_t rawChunkBytes = 240;
  const size_t b64Bytes = ((rawChunkBytes + 2) / 3) * 4 + 1;
  uint8_t raw[rawChunkBytes];
  char b64[b64Bytes];
  size_t rawUsed = 0;
  uint32_t chunkSeq = 0;
  uint32_t sentBytes = 0;

  out.printf(
    "{\"ack\":\"screenshot\",\"ok\":true,\"n\":0,"
    "\"fmt\":\"rgb332\",\"w\":%u,\"h\":%u,\"bytes\":%lu,\"chunk\":%u}\n",
    (unsigned)w, (unsigned)h, (unsigned long)totalBytes, (unsigned)rawChunkBytes
  );

  auto flushChunk = [&]() -> bool {
    if (rawUsed == 0) return true;
    size_t outLen = 0;
    int rc = mbedtls_base64_encode(
      (uint8_t*)b64, sizeof(b64), &outLen, raw, rawUsed
    );
    if (rc != 0) return false;
    b64[outLen] = 0;
    out.printf(
      "{\"ack\":\"screenshot_chunk\",\"ok\":true,\"seq\":%lu,\"d\":\"%s\"}\n",
      (unsigned long)chunkSeq++, b64
    );
    sentBytes += rawUsed;
    rawUsed = 0;
    return true;
  };

  for (uint16_t y = 0; y < h; y++) {
    const uint16_t* row = fb + (size_t)y * w;
    for (uint16_t x = 0; x < w; x++) {
      raw[rawUsed++] = rgb565ToRgb332(row[x]);
      if (rawUsed == rawChunkBytes && !flushChunk()) return false;
    }
  }

  if (!flushChunk()) return false;

  out.printf(
    "{\"ack\":\"screenshot_end\",\"ok\":true,\"chunks\":%lu,\"bytes\":%lu}\n",
    (unsigned long)chunkSeq, (unsigned long)sentBytes
  );
  return true;
}

#include "character.h"
#include "stats.h"

const int W = 184, H = 224;
const int CX = W / 2;
const int CY_BASE = H / 2;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background
const uint16_t GREEN = 0x07E0;
const uint16_t RED   = 0xF800;

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
int8_t       debugStateOverride = -1;   // -1 = auto, 0..6 = forced PersonaState
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;   // 0..4
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage    = 0;
uint8_t petPage     = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed    = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode   = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;

static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
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

// ─── BOOT button state machine ───────────────────────────────────────────────
struct Button {
  bool _cur = false, _prev = false;
  uint32_t _pressAt = 0;
  bool _longFired = false;

  void update() {
    _prev = _cur;
    _cur  = (digitalRead(PIN_BOOT) == LOW);
    if (!_prev && _cur) { _pressAt = millis(); _longFired = false; }
  }
  bool isPressed()   const { return _cur; }
  bool wasPressed()  const { return _cur && !_prev; }
  bool wasReleased() const { return !_cur && _prev; }
  bool pressedFor(uint32_t ms) {
    if (!_cur || _longFired) return false;
    if (millis() - _pressAt >= ms) { _longFired = true; return true; }
    return false;
  }
};
static Button btnA;

// AXP2101 power-key events — set each loop, consumed by button logic
static bool _axpShortPress = false;
static bool _axpLongPress  = false;

static void pollAxpIrq() {
  _axpShortPress = false;
  _axpLongPress  = false;
  if (axp.getIrqStatus() == 0) return;
  _axpShortPress = axp.isPekeyShortPressIrq();
  _axpLongPress  = axp.isPekeyLongPressIrq();
  axp.clearIrqStatus();
}

// ─── IMU ─────────────────────────────────────────────────────────────────────
static bool isFaceDown() {
  float ax, ay, az;
  qmi.getAccelerometer(ax, ay, az);
  az = -az;   // QMI8658 Z-axis is inverted vs M5StickC convention
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// ─── brightness / screen ─────────────────────────────────────────────────────
static const uint8_t BRIGHT_TABLE[] = { 50, 100, 150, 200, 255 };
static void applyBrightness() { _gfx.setBrightness(BRIGHT_TABLE[brightLevel]); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}

bool responseSent = false;

// beep is a no-op on this hardware (no buzzer); kept for call-site compatibility
static void beep(uint16_t, uint16_t) {}

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
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  characterSetPeekYOffset(0);
  buddySetPeek(peek);
  buddySetPeekYOffset(0);
  buddySetScaleOverride(0);
  spr->fillScreen(0x0000);
  characterInvalidate();
}

// ─── attention border animation (replaces GPIO LED) ──────────────────────────
static void drawAttentionBorder(bool on, const uint16_t bg) {
  uint32_t now = millis();
  bool pulse = on && ((now / 400) % 2);
  spr->fillRect(0, 0, W, 8, pulse ? HOT : bg);
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// "clock rot" removed — landscape clock is not supported on this display
const char* settingsItems[] = {
  "brightness", "sound", "bluetooth", "wifi", "led",
  "transcript", "ascii pet", "reset", "back"
};
const uint8_t SETTINGS_N = 9;

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
    case 2: s.bt   = !s.bt;    break;
    case 3: s.wifi = !s.wifi;  break;
    case 4: s.led  = !s.led;   break;
    case 5: s.hud  = !s.hud;   break;
    case 6: nextPet(); return;
    case 7: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 8: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx   = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
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
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr->drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr->setTextColor(p.textDim, PANEL);
  int x = mx + 8;
  spr->setCursor(x, hy); spr->print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr->fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr->setCursor(x, hy); spr->print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr->fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 165, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr->fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr->drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr->setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr->setTextColor(sel ? p.text : p.textDim, PANEL);
    spr->setCursor(mx + 6, my + 8 + i * 14);
    spr->print(sel ? "> " : "  ");
    spr->print(settingsItems[i]);
    spr->setCursor(mx + mw - 36, my + 8 + i * 14);
    spr->setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr->printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr->setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr->print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr->printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 165, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr->fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr->drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr->setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr->setTextColor(sel ? p.text : p.textDim, PANEL);
    spr->setCursor(mx + 6, my + 8 + i * 14);
    spr->print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr->setTextColor(HOT, PANEL);
    spr->print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: axp.shutdown(); break;
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
  int mw = 165, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr->fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr->drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr->setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr->setTextColor(sel ? p.text : p.textDim, PANEL);
    spr->setCursor(mx + 6, my + 8 + i * 14);
    spr->print(sel ? "> " : "  ");
    spr->print(menuItems[i]);
    if (i == 4) spr->print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// ─── RTC cache ───────────────────────────────────────────────────────────────
static uint8_t _clkH = 0, _clkM = 0, _clkS = 0;
static uint8_t _clkMo = 1, _clkD = 1, _clkDow = 0;
uint32_t       _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool    _onUsb = false;

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = axp.getVbusVoltage() > 4000;
  RTC_DateTime dt = pcf.getDateTime();
  _clkH   = dt.getHour();
  _clkM   = dt.getMinute();
  _clkS   = dt.getSecond();
  _clkMo  = dt.getMonth();
  _clkD   = dt.getDay();
  _clkDow = dt.getWeek();
}

static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const int CLOCK_CLEAR_Y = 126;
static const int CLOCK_PEEK_SHIFT_Y = 0;
static const int CLOCK_TIME_Y = 160;
static const int CLOCK_DATE_Y = 208;

static uint8_t clockDow() { return _clkDow % 7; }

// Portrait-only clock (landscape removed — AMOLED canvas has no HW rotation).
// On the squarer AMOLED canvas, reserve the upper region for the pet and the
// lower region for the clock so they never redraw over each other.
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkH, _clkM);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkS);
  uint8_t mi = (_clkMo >= 1 && _clkMo <= 12) ? _clkMo - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkD);

  spr->fillRect(0, CLOCK_CLEAR_Y, W, H - CLOCK_CLEAR_Y, p.bg);
  spr->setTextSize(3);
  int timeX = CX - (int)((strlen(hm) + strlen(ss)) * 18) / 2;

  spr->setTextColor(p.text, p.bg);
  spr->setCursor(timeX, CLOCK_TIME_Y);
  spr->print(hm);

  // Seconds stay on the same line, dimmed rather than pushed to a second row.
  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(timeX + (int)strlen(hm) * 18, CLOCK_TIME_Y);
  spr->print(ss);

  // Date — textSize 1, 6×8 px.
  spr->setTextSize(1);
  spr->setCursor(CX - (int)(strlen(dl) * 6) / 2, CLOCK_DATE_Y);
  spr->print(dl);
}

static void renderPetSurface() {
  if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr->fillScreen(p.bg);
    spr->setTextColor(p.textDim, p.bg);
    spr->setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr->setCursor(8, 90);
      spr->print("installing");
      spr->setCursor(8, 102);
      spr->printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr->drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr->fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr->setCursor(8, 100);
      spr->print("no character loaded");
    }
  }
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_SLEEP;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning > 0)   return P_BUSY;
  return P_IDLE;
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState  = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  qmi.getAccelerometer(ax, ay, az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr->setTextColor(p.text, p.bg);
  spr->setCursor(4, y); spr->print("Info");
  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(W - 28, y); spr->printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr->setTextColor(p.body, p.bg);
  spr->setCursor(4, y); spr->print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr->fillScreen(p.bg);
  spr->setTextSize(1);
  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(8, 52);  spr->print("BLUETOOTH PAIRING");
  spr->setCursor(8, 172); spr->print("enter on desktop:");
  spr->setTextSize(3);
  spr->setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr->setCursor((W - 18 * 6) / 2, 95);
  spr->print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr->fillRect(0, TOP, W, H - TOP, p.bg);
  spr->setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr->setCursor(4, y); spr->print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr->setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr->setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr->setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr->setTextColor(p.text, p.bg);    ln("A   BOOT button");
    spr->setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr->setTextColor(p.text, p.bg);    ln("B   PWR button");
    spr->setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr->setTextColor(p.text, p.bg);    ln("hold A");
    spr->setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr->setTextColor(p.text, p.bg);    ln("hold A + tap B");
    spr->setTextColor(p.textDim, p.bg); ln("    next ascii pet"); y += 4;
    spr->setTextColor(p.text, p.bg);    ln("hold PWR");
    spr->setTextColor(p.textDim, p.bg); ln("    power off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr->setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr->setTextColor(p.text, p.bg);
    ln("LINK");
    spr->setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = axp.getBattVoltage();
    int vBus_mV = axp.getVbusVoltage();
    int pct = (vBat_mV - 3200) / 10;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb      = vBus_mV > 4000;
    bool charging = axp.isCharging();
    bool full     = usb && vBat_mV > 4100 && !charging;

    spr->setTextColor(p.text, p.bg);
    spr->setTextSize(2);
    spr->setCursor(4, y);
    spr->printf("%d%%", pct);
    spr->setTextSize(1);
    spr->setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr->setCursor(60, y + 4);
    spr->print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr->setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr->setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr->setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)axp.getTemperature());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr->setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr->setTextSize(2);
    spr->setCursor(4, y);
    spr->print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr->setTextSize(1);
    y += 20;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    static char btName[16];  // set during startBt()
    extern char _btName[];
    spr->setTextColor(p.text, p.bg);
    ln("  %s", _btName);
    spr->setTextColor(p.textDim, p.bg);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr->setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr->setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr->setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr->setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr->setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr->setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr->setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("Waveshare AMOLED 1.8");
    ln("ESP32-S3 + AXP2101");
  }
}

static uint8_t wrapInto(const char* in, char out[][30], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}
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
  const int AREA = 80;
  spr->fillRect(0, H - AREA, W, AREA, p.bg);
  spr->drawFastHLine(0, H - AREA, W, p.textDim);

  spr->setTextSize(1);
  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr->setTextColor(HOT, p.bg);
  spr->printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr->setTextColor(p.text, p.bg);
  spr->setTextSize(toolLen <= 10 ? 2 : 1);
  spr->setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr->print(tama.promptTool);
  spr->setTextSize(1);

  spr->setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr->setCursor(4, H - AREA + 34);
  spr->printf("%.28s", tama.promptHint);
  if (hlen > 28) {
    spr->setCursor(4, H - AREA + 42);
    spr->printf("%.28s", tama.promptHint + 28);
  }

  if (responseSent) {
    spr->setTextColor(p.textDim, p.bg);
    spr->setCursor(4, H - 12);
    spr->print("sent...");
  } else {
    spr->setTextColor(GREEN, p.bg);
    spr->setCursor(4, H - 12);
    spr->print("A: approve");
    spr->setTextColor(HOT, p.bg);
    spr->setCursor(W - 48, H - 12);
    spr->print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr->fillCircle(x - 2, y, 2, col);
    spr->fillCircle(x + 2, y, 2, col);
    spr->fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr->drawCircle(x - 2, y, 2, col);
    spr->drawCircle(x + 2, y, 2, col);
    spr->drawLine(x - 4, y + 1, x, y + 5, col);
    spr->drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr->fillRect(0, TOP, W, H - TOP, p.bg);
  spr->setTextSize(1);
  int y = TOP + 16;

  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(6, y - 2); spr->print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(60 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr->setCursor(6, y - 2); spr->print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 44 + i * 9;
    if (i < fed) spr->fillCircle(px, y + 1, 2, p.body);
    else spr->drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr->setCursor(6, y - 2); spr->print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 60 + i * 13;
    if (i < en) spr->fillRect(px, y - 2, 9, 6, enCol);
    else spr->drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr->fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr->setTextColor(p.bg, p.body);
  spr->setCursor(11, y + 1); spr->printf("Lv %u", stats().level);

  y += 20;
  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(6, y);
  spr->printf("approved %u", stats().approvals);
  spr->setCursor(6, y + 10);
  spr->printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr->setCursor(6, y + 20);
  spr->printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr->setCursor(6, yPx);
    if (v >= 1000000)   spr->printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr->printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr->printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr->fillRect(0, TOP, W, H - TOP, p.bg);
  spr->setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr->setTextColor(c, p.bg); spr->setCursor(6, y); spr->print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;

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

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr->setTextSize(1);
  spr->setTextColor(p.text, p.bg);
  spr->setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr->printf("%s's %s", ownerName(), petName());
  } else {
    spr->print(petName());
  }
  spr->setTextColor(p.textDim, p.bg);
  spr->setCursor(W - 28, y + 2);
  spr->printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 2, LH = 8;
  const int PAD = 16;
  const int WIDTH = (W - PAD * 2) / 6;
  const int AREA = (SHOW + 1) * LH + 4;
  spr->fillRect(0, H - AREA, W, AREA, p.bg);
  spr->setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  char summary[30];
  if (tama.connected) {
    snprintf(summary, sizeof(summary), "run %u wait %u total %u",
             tama.sessionsRunning, tama.sessionsWaiting, tama.sessionsTotal);
  } else {
    snprintf(summary, sizeof(summary), "offline");
  }
  summary[WIDTH] = 0;
  spr->setTextColor(p.body, p.bg);
  spr->setCursor(PAD, H - AREA + 2);
  spr->print(summary);

  if (tama.nLines == 0) {
    spr->setTextColor(p.text, p.bg);
    static char line[30];
    strncpy(line, tama.msg, WIDTH);
    line[WIDTH] = 0;
    spr->setCursor(PAD, H - AREA + 2 + LH);
    spr->print(line);
    return;
  }

  static char disp[32][30];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end   = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr->setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr->setCursor(PAD, H - AREA + 2 + LH + i * LH);
    spr->print(disp[row]);
  }
  if (msgScroll > 0) {
    spr->setTextColor(p.body, p.bg);
    spr->setCursor(W - PAD - 14, H - LH - 2);
    spr->printf("-%u", msgScroll);
  }
}

// ─── BT name (exposed to drawInfo) ───────────────────────────────────────────
char _btName[16] = "Claude";

static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(_btName, sizeof(_btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(_btName);
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  // 1. TCA9554 expander: configure all outputs, run LCD/touch reset sequence
  expInit();
  expResetSequence();

  // 2. Display
  spr = new Arduino_Canvas(W, H, &_gfx);
  spr->begin();
  _gfx.setBrightness(0);

  // 3. AXP2101 power management
  axp.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL);
  axp.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  axp.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  axp.enableBattDetection();
  axp.enableVbusVoltageMeasure();
  axp.enableBattVoltageMeasure();
  axp.enableTemperatureMeasure();
  axp.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
  axp.clearIrqStatus();

  // 4. IMU
  if (qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_2G, SensorQMI8658::ACC_ODR_125Hz);
    qmi.enableAccelerometer();
  } else {
    Serial.println("[imu] QMI8658 init failed");
  }

  // 5. RTC
  if (!pcf.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("[rtc] PCF85063 init failed");
  }

  // 6. BOOT button
  pinMode(PIN_BOOT, INPUT_PULLUP);

  startBt();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  spr->fillScreen(0x0000);
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr->fillScreen(p.bg);
    spr->setTextSize(2);
    spr->setTextColor(p.text, p.bg);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr->setCursor(CX - (int)(strlen(line) * 12) / 2, CY_BASE - 20);
      spr->print(line);
      spr->setTextColor(p.body, p.bg);
      spr->setCursor(CX - (int)(strlen(petName()) * 12) / 2, CY_BASE + 4);
      spr->print(petName());
    } else {
      spr->setTextColor(p.body, p.bg);
      spr->setCursor(CX - 36, CY_BASE - 20);
      spr->print("Hello!");
      spr->setTextSize(1);
      spr->setTextColor(p.textDim, p.bg);
      const char* sub = "a buddy appears";
      spr->setCursor(CX - (int)(strlen(sub) * 6) / 2, CY_BASE + 4);
      spr->print(sub);
    }
    spr->setTextSize(1);
    hwDisplayPush();
    _gfx.setBrightness(BRIGHT_TABLE[brightLevel]);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  btnA.update();
  pollAxpIrq();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // shake → dizzy
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // BOOT button wake
  if (btnA.isPressed()) {
    if (screenOff) swallowBtnA = true;
    wake();
  }

  // AXP2101 long press → power off
  if (_axpLongPress) {
    axp.shutdown();
  }

  // AXP2101 short press = BtnB (screen toggle when off, action when on)
  if (_axpShortPress) {
    if (screenOff) {
      swallowBtnB = true;
      wake();
    }
    // BtnB action handled below
  }

  // BtnA long press → menu
  if (btnA.pressedFor(600) && !btnALong && !swallowBtnA) {
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

  // BtnA release
  if (btnA.wasReleased()) {
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
    btnALong    = false;
    swallowBtnA = false;
  }

  // BtnB (AXP short press)
  if (_axpShortPress && !screenOff) {
    if (swallowBtnB) { swallowBtnB = false; }
    else if (btnA.isPressed() && !btnALong && !inPrompt
             && !menuOpen && !settingsOpen && !resetOpen) {
      beep(2400, 40);
      nextPet();
      swallowBtnA = true;
    }
    else if (inPrompt) {
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

  // Clock: show on USB power, idle, RTC valid, no overlays
  clockRefreshRtc();
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;

  static bool wasClocking = false;
  if (clocking != wasClocking) {
    if (clocking) {
      characterSetPeek(true);
      characterSetPeekYOffset(CLOCK_PEEK_SHIFT_Y);
      buddySetPeek(true);
      buddySetPeekYOffset(CLOCK_PEEK_SHIFT_Y);
      buddySetScaleOverride(2);
    } else {
      applyDisplayMode();
    }
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  if (clocking) {
    uint8_t dow    = clockDow();
    bool    weekend = (dow == 0 || dow == 6);
    bool    friday  = (dow == 5);
    uint8_t h = _clkH;
    if (h >= 1 && h < 7)        activeState = P_SLEEP;
    else if (weekend)            activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)              activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)            activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)  activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)  activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                         activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  } else if (debugStateOverride >= 0 && debugStateOverride <= 6) {
    activeState = (PersonaState)debugStateOverride;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Render
  if (!napping && !screenOff) {
    if (!clocking) renderPetSurface();
  }

  if (!napping && !screenOff) {
    const Palette& p = characterPalette();
    // Attention border (replaces GPIO LED)
    bool showBorder = (activeState == P_ATTENTION && settings().led);
    drawAttentionBorder(showBorder, p.bg);

    if (blePasskey()) drawPasskey();
    else if (clocking) {
      renderPetSurface();
      drawClock();
    }
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET)  drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    hwDisplayPush();
  }

  // Face-down nap
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)  { if (faceDownFrames < 20) faceDownFrames++; }
    else       { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping     = true;
    napStartMs  = now;
    _gfx.setBrightness(15);  // very dim during nap
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // Auto screen-off on battery
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    _gfx.setBrightness(0);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
