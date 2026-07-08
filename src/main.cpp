// bledash-esp32 — M2: BLE scan / recon.
// Active NimBLE scan that dumps every advertiser (address, RSSI, name, service
// UUIDs, manufacturer data as hex + ASCII) to serial, so we can identify:
//   - the Alpicool K25 and its advertised service UUIDs,
//   - the EcoFlow River 2 Max (serial in manufacturer data) vs the techroom
//     River 2, and the battery-% byte offset.
// Disable the HA alpicool_ble integration first (scripts/ha-alpicool.py disable)
// or the fridge won't be advertising.

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "config.h"

static String toHex(const uint8_t* data, size_t len) {
  String s;
  s.reserve(len * 3);
  char buf[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02X ", data[i]);
    s += buf;
  }
  return s;
}

static String toAscii(const uint8_t* data, size_t len) {
  String s;
  s.reserve(len);
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    s += (c >= 0x20 && c < 0x7f) ? c : '.';
  }
  return s;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    const std::string addr = dev->getAddress().toString();
    const std::string name = dev->haveName() ? dev->getName() : "";

    Serial.printf("\n[%s] RSSI=%d", addr.c_str(), dev->getRSSI());
    if (!name.empty()) Serial.printf("  name=\"%s\"", name.c_str());
    Serial.println();

    const uint8_t svcCount = dev->getServiceUUIDCount();
    for (uint8_t i = 0; i < svcCount; i++) {
      Serial.printf("    service: %s\n", dev->getServiceUUID(i).toString().c_str());
    }

    if (dev->haveManufacturerData()) {
      const std::string md = dev->getManufacturerData();
      const uint8_t* p = (const uint8_t*)md.data();
      const size_t n = md.size();
      // First two bytes are the company ID (little-endian).
      uint16_t company = n >= 2 ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
      Serial.printf("    mfr (companyID=0x%04X, %u bytes):\n", company, (unsigned)n);
      Serial.printf("      hex: %s\n", toHex(p, n).c_str());
      Serial.printf("      asc: %s\n", toAscii(p, n).c_str());
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    Serial.printf("\n--- scan window ended (reason=%d, %d devices) ---\n",
                  reason, results.getCount());
    NimBLEDevice::getScan()->start(0, false, true);  // restart continuous
  }
};

static ScanCallbacks scanCallbacks;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bledash M2: BLE scan / recon ===");
  Serial.printf("Looking for Alpicool MAC %s and EcoFlow serial %s\n",
                ALPICOOL_MAC, ECOFLOW_SERIAL);

  NimBLEDevice::init("bledash-scan");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, /*wantDuplicates=*/false);
  scan->setActiveScan(true);   // recon: also pull scan-response payloads
  scan->setInterval(100);
  scan->setWindow(90);
  scan->start(0, false, true);  // duration=0 -> scan forever
  Serial.println("Scanning...");
}

void loop() {
  delay(1000);
}
