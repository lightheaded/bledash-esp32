// bledash-esp32 — M4: combined single page (fridge + EcoFlow battery).
// One BLE scan loop feeds both devices: it parses EcoFlow battery from passive
// advertisements and spots the Alpicool advertising so the driver can connect.
// The fridge connection is held; EcoFlow is never connected to.

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"
#include "devices/alpicool.h"
#include "devices/ecoflow.h"

#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
#include "devices/ecoflow_session.h"
// A non-empty string literal is >1 byte (the NUL); "" is exactly 1.
static_assert(sizeof(ECOFLOW_USER_ID) > 1,
              "ECOFLOW_GATT=1 requires a non-empty ECOFLOW_USER_ID in config.h "
              "(fetch it with scripts/ecoflow_userid.py)");
#endif

static const uint32_t kPollIntervalMs = 60000;
static const uint32_t kScanWindowMs = 2500;
static const uint32_t kReconnectMs = 5000;
// The authenticated GATT handshake needs a usable link (bidirectional writes +
// notifications). Below this advertised RSSI we don't grab the connection —
// which would only stall and also stop the unit advertising — and instead keep
// reading battery % passively from advertisements, which works at longer range.
static const int kEcoMinRssiDbm = -88;
// Stale thresholds: fridge polls every 60 s; EcoFlow advertises continuously.
static const uint32_t kFridgeStaleMs = 2 * kPollIntervalMs + 15000;
static const uint32_t kBatteryStaleMs = 30000;

U8G2_SSD1306_72X40_ER_F_HW_I2C display(U8G2_R0, /*reset=*/U8X8_PIN_NONE);
AlpicoolDriver fridge;
EcoflowMonitor battery;
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
EcoflowSession ecoflowSession;
static uint32_t lastEcoConnAttemptMs = 0;
#endif

static uint32_t lastConnAttemptMs = 0;

static const uint8_t* bigFont(const char* s) {
  return strlen(s) >= 3 ? u8g2_font_logisoso16_tr : u8g2_font_logisoso24_tr;
}

// Draw a number as large as fits: full-size for <=2 chars, smaller for 3
// (e.g. "-18", "100"), horizontally centered in [x0, x0+w).
static void drawBigCentered(int x0, int w, int baseline, const char* s) {
  display.setFont(bigFont(s));
  int sw = display.getStrWidth(s);
  display.drawStr(x0 + (w - sw) / 2, baseline, s);
}

// Like drawBigCentered but with a small superscript degree symbol after the
// number (the large fonts are ASCII-only and have no °). Number+° are centered
// as a unit and the ° is top-aligned to the big glyph.
static void drawBigTemp(int x0, int w, int baseline, const char* s) {
  const uint8_t* font = bigFont(s);
  display.setFont(font);
  int bw = display.getStrWidth(s);
  int bigAsc = display.getAscent();
  display.setFont(u8g2_font_6x10_tf);
  int dw = display.getStrWidth("\xb0");
  int smallAsc = display.getAscent();

  int startX = x0 + (w - (bw + dw)) / 2;
  display.setFont(font);
  display.drawStr(startX, baseline, s);
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(startX + bw, baseline - bigAsc + smallAsc, "\xb0");
}

// Big battery %: number in logisoso16 with a small "%" in 6x10, centered on the
// given baseline. Shared by the passive-% and rich EcoFlow views.
static void drawBattPct(int x0, int w, int baseline, int pct, bool stale) {
  if (stale) {
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(x0 + (w - display.getStrWidth("--")) / 2, baseline, "--");
    return;
  }
  char num[6];
  snprintf(num, sizeof(num), "%d", pct);
  display.setFont(u8g2_font_logisoso16_tr);
  int nw = display.getStrWidth(num);
  display.setFont(u8g2_font_6x10_tf);
  int pw = display.getStrWidth("%");
  int startX = x0 + (w - (nw + pw)) / 2;
  if (startX < x0) startX = x0;
  display.setFont(u8g2_font_logisoso16_tr);
  display.drawStr(startX, baseline, num);
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(startX + nw, baseline, "%");
}

#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
// Rich EcoFlow column for the authenticated GATT session: big SoC % on top, then
// a direction triangle + watts, then time remaining. x0 already includes the
// burn-in x-shift; sy is the y-shift. The vertical bar (drawn by renderPage)
// carries the at-a-glance level and separates the two columns.
static void drawEcoflowRich(int x0, int w, int sy, int socPct,
                            const EcoflowRichReading& er) {
  bool charging = er.state == EcoflowRichReading::Charge::kCharging;
  bool discharging = er.state == EcoflowRichReading::Charge::kDischarging;

  // Top: SoC %, big.
  drawBattPct(x0, w, sy + 16, socPct, false);

  // Middle: watts, with a direction triangle when charging/discharging; idle is
  // plain "0W" (no glyph — a dash reads like a minus).
  int watts = charging      ? (er.haveInputWatts ? (int)er.inputWatts : 0)
              : discharging ? (er.haveOutputWatts ? (int)er.outputWatts : 0)
                            : 0;
  display.setFont(u8g2_font_6x10_tf);
  char l2[8];
  snprintf(l2, sizeof(l2), "%dW", watts);
  int wWidth = display.getStrWidth(l2);
  int base = sy + 28;
  if (charging || discharging) {
    const int triW = 5, triH = 7;
    int lx = x0 + (w - (triW + 2 + wWidth)) / 2;
    int triTop = base - 8;
    if (charging) {  // ▲ into the battery
      display.drawTriangle(lx + triW / 2, triTop, lx, triTop + triH, lx + triW, triTop + triH);
    } else {  // ▼ out of the battery
      display.drawTriangle(lx, triTop, lx + triW, triTop, lx + triW / 2, triTop + triH);
    }
    display.drawStr(lx + triW + 2, base, l2);
  } else {
    display.drawStr(x0 + (w - wWidth) / 2, base, l2);
  }

  // Bottom: time remaining, H:MM, always shown (to-full charging, else to-empty;
  // the device caps the idle estimate at 5999 -> 99:59).
  bool haveTime = false;
  unsigned rem = 0;
  if (charging) {
    if (er.haveChargeRemainMin) { rem = er.chargeRemainMin; haveTime = true; }
  } else if (er.haveDischargeRemainMin) {
    rem = er.dischargeRemainMin;
    haveTime = true;
  }
  display.setFont(u8g2_font_5x7_tf);
  char l3[10];
  if (haveTime) {
    if (rem > 5999) rem = 5999;
    snprintf(l3, sizeof(l3), "%u:%02u", rem / 60, rem % 60);
  } else {
    snprintf(l3, sizeof(l3), "--:--");
  }
  display.drawStr(x0 + (w - display.getStrWidth(l3)) / 2, sy + 37, l3);
}
#endif

static void renderPage() {
  const FridgeReading& f = fridge.reading();
  const BatteryReading& b = battery.reading();
  uint32_t now = millis();
  bool fStale = !f.valid || (now - f.lastUpdateMs > kFridgeStaleMs);
  bool bStale = !b.valid || (now - b.lastUpdateMs > kBatteryStaleMs);
  int socPct = b.percent;
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  // While we hold the GATT session the unit stops advertising, so prefer the
  // authenticated SoC; the rich column below adds watts + time remaining.
  const EcoflowRichReading& er = ecoflowSession.reading();
  if (ecoflowSession.authenticated() && er.haveSoc) {
    socPct = (int)(er.soc + 0.5f);
    bStale = false;
  }
#endif
  const int W = display.getDisplayWidth();  // 72
  // Burn-in mitigation: shift the whole frame by a small offset that advances
  // every few minutes, so no pixel is lit in the same spot indefinitely. The
  // layout is sized to a (W-1-MAXSHIFT)-wide safe area so shifting never clips.
  const int MAXSHIFT = 2;
  int step = now / 240000;  // advance every 4 minutes
  const int sx = step % (MAXSHIFT + 1);
  const int sy = (step / (MAXSHIFT + 1)) % (MAXSHIFT + 1);

  const int Weff = W - 1 - MAXSHIFT;  // safe drawable width
  const int leftW = 32;               // fridge column (x 0..31)
  const int vbarX = 33;               // vertical SoC bar; doubles as separator
  const int vbarW = 4;
  const int rightX = 39;              // EcoFlow / battery column
  const int rightW = Weff - rightX;
  const int bigY = 24 + sy;           // fridge temp, raised toward the top
  const int spY = 37 + sy;            // setpoint, lower-left

  display.clearBuffer();

  // --- Left column: fridge ---
  // Lower-left: setpoint normally; "OFF" when the compressor is off; ".." while
  // (re)connecting. No "ON" — a shown temperature already implies it's running.
  display.setFont(u8g2_font_5x7_tf);
  char sp[8];
  if (fStale)
    snprintf(sp, sizeof(sp), "%s", fridge.connected() ? ".." : "");
  else if (!f.poweredOn)
    snprintf(sp, sizeof(sp), "OFF");
  else
    snprintf(sp, sizeof(sp), "%d\xb0", f.targetTemp);
  display.drawStr(sx, spY, sp);
  char temp[8];
  snprintf(temp, sizeof(temp), fStale ? "--" : "%d", f.actualTemp);
  if (fStale) drawBigCentered(sx, leftW, bigY, temp);
  else drawBigTemp(sx, leftW, bigY, temp);

  // --- Right column: battery (passive %) or rich EcoFlow (authenticated GATT) ---
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  if (ecoflowSession.authenticated() &&
      (er.haveSoc || er.haveInputWatts || er.haveOutputWatts)) {
    drawEcoflowRich(sx + rightX, rightW, sy, socPct, er);
  } else
#endif
  {
    // Passive battery-% only (GATT off or not yet authenticated): big % centered.
    drawBattPct(sx + rightX, rightW, sy + 24, socPct, bStale);
  }

  // --- Vertical SoC bar down the middle: at-a-glance level + column separator,
  // draining from the top as the battery drains ---
  const int barTop = sy;
  const int barH = 38;  // full height minus the burn-in shift range
  display.drawFrame(sx + vbarX, barTop, vbarW, barH);
  if (!bStale) {
    int pct = socPct < 0 ? 0 : (socPct > 100 ? 100 : socPct);
    int fillH = (barH - 2) * pct / 100;
    display.drawBox(sx + vbarX + 1, barTop + 1 + (barH - 2 - fillH), vbarW - 2, fillH);
  }

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bledash: fridge + battery ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin();
  display.setContrast(180);
  display.setFontMode(1);

  NimBLEDevice::init("bledash");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max TX power for marginal range
  NimBLEDevice::getScan()->setActiveScan(true);

  fridge.begin(ALPICOOL_MAC, kPollIntervalMs);
  battery.begin(ECOFLOW_SERIAL);
  Serial.printf("Fridge %s, EcoFlow serial %s\n", ALPICOOL_MAC, ECOFLOW_SERIAL);
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  ecoflowSession.begin(ECOFLOW_SERIAL, ECOFLOW_USER_ID);
  Serial.println("EcoFlow GATT telemetry: ENABLED (advanced)");
#endif
}

void loop() {
  // One scan window feeds both devices.
  NimBLEScan* scan = NimBLEDevice::getScan();
  NimBLEScanResults res = scan->getResults(kScanWindowMs, false);

  const NimBLEAdvertisedDevice* fridgeDev = nullptr;
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  const NimBLEAdvertisedDevice* ecoDev = nullptr;
#endif
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    battery.onAdvertisement(d);
    if (fridge.matches(d)) fridgeDev = d;
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
    if (ecoflowSession.matches(d)) ecoDev = d;
#endif
  }

  uint32_t now = millis();
  if (!fridge.connected() && fridgeDev &&
      (lastConnAttemptMs == 0 || now - lastConnAttemptMs >= kReconnectMs)) {
    lastConnAttemptMs = now;
    fridge.connectTo(fridgeDev);
  }
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  // The EcoFlow stops advertising while we hold the connection, so it only
  // reappears in the scan after a drop — then we reconnect. Only attempt when
  // the link is strong enough to complete the handshake.
  if (!ecoflowSession.connected() && ecoDev &&
      (lastEcoConnAttemptMs == 0 || now - lastEcoConnAttemptMs >= kReconnectMs)) {
    int rssi = ecoDev->getRSSI();
    if (rssi >= kEcoMinRssiDbm) {
      lastEcoConnAttemptMs = now;
      ecoflowSession.connectTo(ecoDev);
    } else {
      static uint32_t lastWeakLogMs = 0;
      if (now - lastWeakLogMs > 10000) {
        lastWeakLogMs = now;
        Serial.printf("[ecoflow] seen at RSSI=%d dBm (< %d) — too weak for GATT, "
                      "staying on passive battery %%\n", rssi, kEcoMinRssiDbm);
      }
    }
  }
#endif
  scan->clearResults();

  fridge.poll();
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  ecoflowSession.poll();
#endif
  renderPage();

  // Log a status line only when something changes, so the serial console stays
  // useful on this otherwise headless device without spamming every loop.
  const FridgeReading& f = fridge.reading();
  const BatteryReading& b = battery.reading();
  static char last[192] = "";
  char line[192];
  snprintf(line, sizeof(line),
           "fridge: conn=%d valid=%d %d/%d on=%d | batt: valid=%d %d%%",
           fridge.connected(), f.valid, f.actualTemp, f.targetTemp, f.poweredOn,
           b.valid, b.percent);
#if defined(ECOFLOW_GATT) && ECOFLOW_GATT
  const EcoflowRichReading& er = ecoflowSession.reading();
  const char* stStr = er.state == EcoflowRichReading::Charge::kCharging      ? "chg"
                      : er.state == EcoflowRichReading::Charge::kDischarging ? "dsg"
                      : er.state == EcoflowRichReading::Charge::kIdle        ? "idle"
                                                                            : "?";
  // Show the remaining-time that matches the current direction (the other field
  // reads a large sentinel value).
  unsigned remainMin =
      er.state == EcoflowRichReading::Charge::kCharging
          ? (er.haveChargeRemainMin ? er.chargeRemainMin : 0u)
          : (er.haveDischargeRemainMin ? er.dischargeRemainMin : 0u);
  const char* remainLabel =
      er.state == EcoflowRichReading::Charge::kCharging ? "toFull" : "toEmpty";
  size_t used = strlen(line);
  snprintf(line + used, sizeof(line) - used,
           " | eco: conn=%d auth=%d %s in=%dW out=%dW soc=%.1f %s=%umin",
           ecoflowSession.connected(), ecoflowSession.authenticated(), stStr,
           er.haveInputWatts ? (int)er.inputWatts : -1,
           er.haveOutputWatts ? (int)er.outputWatts : -1,
           er.haveSoc ? er.soc : -1.0f, remainLabel, remainMin);
#endif
  if (strcmp(line, last) != 0) {
    strcpy(last, line);
    Serial.printf("[page] %s\n", line);
  }
}
