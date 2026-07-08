// bledash-esp32 — M3: Alpicool driver + fridge page.
// Connect to the K25 over BLE, poll status once a minute, and render the fridge
// readings on the 72x40 OLED. EcoFlow is added in M4.

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"
#include "devices/alpicool.h"

static const uint32_t kPollIntervalMs = 60000;
// Consider a reading stale after two missed polls.
static const uint32_t kStaleMs = 2 * kPollIntervalMs + 15000;

U8G2_SSD1306_72X40_ER_F_HW_I2C display(U8G2_R0, /*reset=*/U8X8_PIN_NONE);
AlpicoolDriver fridge;

static void renderFridge() {
  const FridgeReading& r = fridge.reading();
  bool stale = !r.valid || (millis() - r.lastUpdateMs > kStaleMs);

  display.clearBuffer();

  // Small header row: label + connection/power state.
  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 6, "FRIDGE");
  const char* state = stale ? (fridge.connected() ? "..." : "OFFLINE")
                            : (r.poweredOn ? "ON" : "OFF");
  display.drawStr(display.getDisplayWidth() - display.getStrWidth(state), 6, state);

  // Big actual temperature.
  char big[8];
  if (stale) {
    snprintf(big, sizeof(big), "--");
  } else {
    snprintf(big, sizeof(big), "%d", r.actualTemp);
  }
  display.setFont(u8g2_font_logisoso20_tr);
  int bw = display.getStrWidth(big);
  int bx = (display.getDisplayWidth() - bw) / 2 - 5;
  display.drawStr(bx, 30, big);
  // Degree + C to the upper right of the number.
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(bx + bw + 2, 15, "\xb0" "C");

  // Setpoint on the bottom row.
  display.setFont(u8g2_font_5x7_tf);
  char sp[12];
  if (stale) {
    snprintf(sp, sizeof(sp), "set --");
  } else {
    snprintf(sp, sizeof(sp), "set %d\xb0", r.targetTemp);
  }
  display.drawStr(0, 39, sp);

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bledash M3: Alpicool driver ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin();
  display.setContrast(180);
  display.setFontMode(1);

  NimBLEDevice::init("bledash");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max TX power for marginal in-car range
  fridge.begin(ALPICOOL_MAC, kPollIntervalMs);

  Serial.printf("Target fridge: %s\n", ALPICOOL_MAC);
}

void loop() {
  fridge.loop();
  renderFridge();
  delay(200);
}
