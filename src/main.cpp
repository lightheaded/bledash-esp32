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
// Onboard BOOT button (GPIO9): a press toggles the fridge compressor on/off.
// It's the only runtime-usable button on this board — RST is the chip reset
// line, not a readable GPIO. GPIO9 is active-low with an external pull-up and is
// a boot strap (must read HIGH at reset), which a runtime press doesn't affect.
static const uint8_t kBootButtonPin = 9;
// Toggling fridge power requires a deliberate press-and-HOLD, not a tap: turning
// the fridge off (or on) by accident is the failure we're guarding against. The
// hold shows a full-screen countdown; releasing early cancels.
static const uint32_t kHoldToToggleMs = 1000;
// The main loop blocks inside the BLE scan, so we scan in short slices and bail
// the instant BOOT is pressed — otherwise the hold animation would start up to a
// full scan window late and the countdown wouldn't track the real hold.
static const uint32_t kScanSliceMs = 200;
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

// Set by the BOOT-button ISR on each press (falling edge); consumed in loop().
// The ISR does the minimum — latch a flag — so a press is never lost while the
// loop is blocked in the scan. The hold gesture itself is measured by polling
// the pin directly in confirmPowerToggle(), so contact bounce is harmless: a
// bounce can't sustain a 1 s hold.
static volatile bool buttonPressed = false;

static void IRAM_ATTR onBootButton() { buttonPressed = true; }

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

// Loading spinner: a ring of 8 dots with one highlighted, advancing by `frame`.
// Shown in place of the fridge reading while a power change awaits its confirming
// QUERY, so the display never shows a stale on/off state as if it were current.
static void drawSpinner(int cx, int cy, uint8_t frame) {
  const float r = 9.0f;
  for (int i = 0; i < 8; i++) {
    float a = i * (2.0f * PI / 8.0f) - PI / 2.0f;  // 0 at top, clockwise
    int x = cx + (int)lroundf(r * cosf(a));
    int y = cy + (int)lroundf(r * sinf(a));
    if (i == frame % 8) display.drawDisc(x, y, 2);
    else display.drawPixel(x, y);
  }
}

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
  if (fridge.powerChangePending()) {
    // A power toggle was just sent; show a spinner until the confirming QUERY
    // lands, rather than the (still stale) on/off reading.
    static uint8_t spin = 0;
    drawSpinner(sx + leftW / 2, sy + 13, spin++);
  } else if (!fStale && !f.poweredOn) {
    // Compressor off: big "OFF" plus a compact hint that the BOOT button turns
    // it back on ("R btn" in the board's USB-down orientation). Without the hint
    // an off fridge looks like a dead readout.
    drawBigCentered(sx, leftW, bigY, "OFF");
    display.setFont(u8g2_font_4x6_tf);
    const char* hint = "hold=ON";
    display.drawStr(sx + (leftW - display.getStrWidth(hint)) / 2, spY, hint);
  } else {
    // Lower-left: setpoint normally; ".." while (re)connecting. No "ON" — a shown
    // temperature already implies it's running.
    display.setFont(u8g2_font_5x7_tf);
    char sp[8];
    if (fStale)
      snprintf(sp, sizeof(sp), "%s", fridge.connected() ? ".." : "");
    else
      snprintf(sp, sizeof(sp), "%d\xb0", f.targetTemp);
    display.drawStr(sx, spY, sp);
    char temp[8];
    snprintf(temp, sizeof(temp), fStale ? "--" : "%d", f.actualTemp);
    if (fStale) drawBigCentered(sx, leftW, bigY, temp);
    else drawBigTemp(sx, leftW, bigY, temp);
  }

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

// Scan for adverts in short slices instead of one long blocking call, returning
// early the moment BOOT is pressed so the hold animation starts promptly. Slices
// after the first continue the same scan (no clear) so results accumulate.
static NimBLEScanResults scanWindow() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  NimBLEScanResults res;
  for (uint32_t elapsed = 0; elapsed < kScanWindowMs; elapsed += kScanSliceMs) {
    res = scan->getResults(kScanSliceMs, elapsed != 0);
    if (buttonPressed) break;
  }
  return res;
}

// Full-screen hold-to-confirm progress: title naming the pending action, a bar
// filling left→right, and a 1-decimal countdown. Transient, so it skips the
// burn-in shift.
static void renderHoldScreen(const char* action, uint32_t held, uint32_t holdMs) {
  if (held > holdMs) held = holdMs;
  const int W = display.getDisplayWidth();  // 72
  display.clearBuffer();

  display.setFont(u8g2_font_5x7_tf);
  const char* l1 = "HOLD TO TURN";
  display.drawStr((W - display.getStrWidth(l1)) / 2, 6, l1);
  char l2[16];
  snprintf(l2, sizeof(l2), "FRIDGE %s", action);
  display.drawStr((W - display.getStrWidth(l2)) / 2, 14, l2);

  // Countdown, big and centered (e.g. "0.7").
  char cd[8];
  snprintf(cd, sizeof(cd), "%.1f", (holdMs - held) / 1000.0f);
  display.setFont(u8g2_font_logisoso16_tr);
  display.drawStr((W - display.getStrWidth(cd)) / 2, 31, cd);

  // Progress bar along the bottom.
  const int bx = 1, by = 34, bw = W - 2, bh = 5;
  display.drawFrame(bx, by, bw, bh);
  int fill = (bw - 2) * (int)held / (int)holdMs;
  if (fill > 0) display.drawBox(bx + 1, by + 1, fill, bh - 2);

  display.sendBuffer();
}

// Brief post-toggle acknowledgement so the press feels registered before the
// re-QUERY confirms the real state.
static void renderToggleFlash(const char* action) {
  const int W = display.getDisplayWidth();
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  const char* l1 = "FRIDGE";
  display.drawStr((W - display.getStrWidth(l1)) / 2, 12, l1);
  display.setFont(u8g2_font_logisoso16_tr);
  display.drawStr((W - display.getStrWidth(action)) / 2, 34, action);
  display.sendBuffer();
  delay(800);
}

// After a toggle, animate the spinner smoothly while polling for the confirming
// QUERY — the main loop repaints only once per scan (~2.5 s), far too slow for a
// spinner, so we drive it here at ~25 fps and re-QUERY periodically until the
// status frame resolves (clears powerChangePending) or we give up. renderPage()
// already draws the spinner in the pending branch; calling it rapidly animates
// it while keeping the battery column live.
static void waitForPowerConfirm() {
  const uint32_t kTimeoutMs = 4000;
  const uint32_t kRequeryMs = 800;
  uint32_t start = millis();
  uint32_t lastQuery = 0;
  while (fridge.powerChangePending() && millis() - start < kTimeoutMs) {
    uint32_t nowMs = millis();
    if (nowMs - lastQuery >= kRequeryMs) {
      fridge.requestStatusNow();  // safe: the 800 ms flash already gave the device
      lastQuery = nowMs;          // time to apply the SET (vendor re-queries at 500 ms)
    }
    renderPage();
    delay(40);
  }
}

// Handle a latched BOOT press: require a full hold before toggling fridge power,
// rendering the countdown throughout. A release (or the button never actually
// being held, e.g. a bounce) cancels with no change. Polls the pin directly so
// the gesture is immune to the loop's scan blocking.
static void confirmPowerToggle() {
  buttonPressed = false;
  if (!fridge.canControl()) {
    Serial.println("[button] fridge not connected/ready; ignoring hold");
    return;
  }
  bool target = !fridge.reading().poweredOn;  // OFF->ON or ON->OFF
  const char* action = target ? "ON" : "OFF";
  uint32_t start = millis();

  while (digitalRead(kBootButtonPin) == LOW) {  // LOW = held (active-low)
    uint32_t held = millis() - start;
    renderHoldScreen(action, held, kHoldToToggleMs);
    if (held >= kHoldToToggleMs) {
      Serial.printf("[button] hold confirmed -> turning fridge %s\n", action);
      fridge.setPower(target);
      renderToggleFlash(action);
      waitForPowerConfirm();  // smooth spinner until the change is confirmed
      buttonPressed = false;  // discard any bounce latched during the hold
      return;
    }
    delay(25);
  }
  Serial.println("[button] hold released early -> cancelled");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bledash: fridge + battery ===");

  // BOOT button toggles fridge power. Active-low; FALLING = press. The press is
  // latched here and confirmed by a press-and-hold in confirmPowerToggle().
  pinMode(kBootButtonPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(kBootButtonPin), onBootButton, FALLING);

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
  // One scan window feeds both devices (sliced so a BOOT press is seen quickly).
  NimBLEScan* scan = NimBLEDevice::getScan();
  NimBLEScanResults res = scanWindow();

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

  // BOOT-button press: hold-to-confirm, then toggle fridge power. Handled after
  // poll() so setPower()'s forced re-QUERY lands on the NEXT poll() (giving the
  // device time to apply the SET) rather than immediately, which would read the
  // stale pre-toggle state and then not re-query for a full interval.
  if (buttonPressed) confirmPowerToggle();

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
