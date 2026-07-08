// bledash-esp32 — M1: hello display.
// Initialize the onboard 0.42" 72x40 SSD1306 OLED and show "bledash" with a
// pixel-animated dot. Also probes the two candidate I2C pin pairs so the serial
// log states definitively which GPIOs drive the onboard panel (open question #1).

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"

static const uint8_t SSD1306_ADDR = 0x3C;

// Candidate I2C pin pairs for this board family: {SDA, SCL}.
// config.h's OLED_SDA/OLED_SCL is tried first, then the alternate vendor labels.
struct PinPair {
  uint8_t sda;
  uint8_t scl;
};
static const PinPair kCandidates[] = {{OLED_SDA, OLED_SCL}, {8, 9}, {5, 6}};

// Full framebuffer (_F_), hardware I2C. Pins are set via Wire.begin() before
// display.begin(), so the constructor pins are placeholders.
U8G2_SSD1306_72X40_ER_F_HW_I2C display(U8G2_R0, /*reset=*/U8X8_PIN_NONE);

static bool i2cDevicePresent(uint8_t sda, uint8_t scl, uint8_t addr) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(400000);
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;  // 0 == device ACKed
}

static PinPair gActive = {OLED_SDA, OLED_SCL};

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bledash M1: hello display ===");

  bool found = false;
  for (const PinPair& p : kCandidates) {
    bool ack = i2cDevicePresent(p.sda, p.scl, SSD1306_ADDR);
    Serial.printf("I2C probe SDA=%2d SCL=%2d @0x%02X -> %s\n", p.sda, p.scl,
                  SSD1306_ADDR, ack ? "ACK (display here)" : "no response");
    if (ack && !found) {
      gActive = p;
      found = true;
    }
  }

  if (!found) {
    Serial.println("WARNING: no SSD1306 ACK on any candidate pins; "
                   "defaulting to config.h pins. Check wiring.");
  }
  Serial.printf("Using SDA=%d SCL=%d\n", gActive.sda, gActive.scl);

  Wire.end();
  Wire.begin(gActive.sda, gActive.scl);
  display.begin();
  display.setContrast(180);
}

void loop() {
  static uint8_t frame = 0;

  display.clearBuffer();

  display.setFont(u8g2_font_7x14B_tr);
  display.drawStr(6, 16, "bledash");

  // A dot that walks across the bottom to prove the loop is live.
  const int w = display.getDisplayWidth();
  int x = frame % w;
  display.drawDisc(x, 32, 2);

  display.sendBuffer();

  frame = (frame + 3) % w;
  delay(60);
}
