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

static const uint32_t kPollIntervalMs = 60000;
static const uint32_t kScanWindowMs = 2500;
static const uint32_t kReconnectMs = 5000;
// Stale thresholds: fridge polls every 60 s; EcoFlow advertises continuously.
static const uint32_t kFridgeStaleMs = 2 * kPollIntervalMs + 15000;
static const uint32_t kBatteryStaleMs = 30000;

U8G2_SSD1306_72X40_ER_F_HW_I2C display(U8G2_R0, /*reset=*/U8X8_PIN_NONE);
AlpicoolDriver fridge;
EcoflowMonitor battery;

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

static void renderPage() {
  const FridgeReading& f = fridge.reading();
  const BatteryReading& b = battery.reading();
  uint32_t now = millis();
  bool fStale = !f.valid || (now - f.lastUpdateMs > kFridgeStaleMs);
  bool bStale = !b.valid || (now - b.lastUpdateMs > kBatteryStaleMs);
  const int W = display.getDisplayWidth();  // 72
  const int split = 37;
  const int leftW = split - 1;       // fridge column
  const int rightX = split + 2;      // battery column
  const int rightW = W - rightX;

  display.clearBuffer();

  // --- Left column: fridge ---
  display.setFont(u8g2_font_5x7_tf);
  const char* st = fStale ? (fridge.connected() ? ".." : "off") : (f.poweredOn ? "ON" : "OFF");
  display.drawStr(0, 7, st);                                   // top-left: on/off
  char sp[8];                                                  // top-right: setpoint + unit
  if (fStale) snprintf(sp, sizeof(sp), "\xb0" "C");
  else snprintf(sp, sizeof(sp), "%d\xb0" "C", f.targetTemp);
  display.drawStr(leftW - display.getStrWidth(sp), 7, sp);
  char temp[8];
  snprintf(temp, sizeof(temp), fStale ? "--" : "%d", f.actualTemp);
  if (fStale) drawBigCentered(0, leftW, 36, temp);
  else drawBigTemp(0, leftW, 36, temp);

  // --- Right column: battery ---
  display.setFont(u8g2_font_5x7_tf);
  const char* batTitle = "BATT %";
  display.drawStr(rightX + (rightW - display.getStrWidth(batTitle)) / 2, 7, batTitle);
  char pct[8];
  snprintf(pct, sizeof(pct), bStale ? "--" : "%d", b.percent);
  drawBigCentered(rightX, rightW, 36, pct);

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bledash M4: fridge + battery ===");

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
}

void loop() {
  // One scan window feeds both devices.
  NimBLEScan* scan = NimBLEDevice::getScan();
  NimBLEScanResults res = scan->getResults(kScanWindowMs, false);

  const NimBLEAdvertisedDevice* fridgeDev = nullptr;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    battery.onAdvertisement(d);
    if (fridge.matches(d)) fridgeDev = d;
  }

  uint32_t now = millis();
  if (!fridge.connected() && fridgeDev &&
      (lastConnAttemptMs == 0 || now - lastConnAttemptMs >= kReconnectMs)) {
    lastConnAttemptMs = now;
    fridge.connectTo(fridgeDev);
  }
  scan->clearResults();

  fridge.poll();
  renderPage();

  // Log a status line only when something changes, so the serial console stays
  // useful on this otherwise headless device without spamming every loop.
  const FridgeReading& f = fridge.reading();
  const BatteryReading& b = battery.reading();
  static char last[96] = "";
  char line[96];
  snprintf(line, sizeof(line),
           "fridge: conn=%d valid=%d %d/%d on=%d | batt: valid=%d %d%%",
           fridge.connected(), f.valid, f.actualTemp, f.targetTemp, f.poweredOn,
           b.valid, b.percent);
  if (strcmp(line, last) != 0) {
    strcpy(last, line);
    Serial.printf("[page] %s\n", line);
  }
}
