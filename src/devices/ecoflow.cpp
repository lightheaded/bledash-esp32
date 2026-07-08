#include "devices/ecoflow.h"

namespace {
constexpr uint16_t kCompanyId = 0xB5B5;  // EcoFlow
constexpr size_t kSerialOffset = 3;      // ASCII serial starts here
constexpr size_t kSerialLen = 16;
constexpr size_t kBatteryOffset = 19;    // battery % byte (verified at M2)
}  // namespace

void EcoflowMonitor::begin(const char* serial) { serial_ = serial; }

void EcoflowMonitor::onAdvertisement(const NimBLEAdvertisedDevice* dev) {
  if (!dev->haveManufacturerData()) return;
  const std::string md = dev->getManufacturerData();
  const uint8_t* p = (const uint8_t*)md.data();
  const size_t n = md.size();

  // Need company ID + serial + battery byte present.
  if (n <= kBatteryOffset) return;
  uint16_t company = (uint16_t)(p[0] | (p[1] << 8));
  if (company != kCompanyId) return;

  // Match the full serial, not just the company ID — there can be more than one
  // EcoFlow in range.
  String serial;
  serial.reserve(kSerialLen);
  for (size_t i = 0; i < kSerialLen; i++) serial += (char)p[kSerialOffset + i];
  if (serial != serial_) return;

  reading_.percent = p[kBatteryOffset];
  reading_.valid = true;
  reading_.lastUpdateMs = millis();
}
