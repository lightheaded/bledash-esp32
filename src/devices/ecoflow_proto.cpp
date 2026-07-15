#include "devices/ecoflow_proto.h"

#include <string.h>

namespace ecoflow {
namespace {

// Inner Packet V2 header is fixed-size; payload begins right after it.
constexpr uint8_t kEncPrefix0 = 0x5A;
constexpr uint8_t kEncPrefix1 = 0x5A;
constexpr uint8_t kEncUnknown = 0x01;  // constant byte at outer offset 3
constexpr size_t kEncHeader = 6;       // prefix(2)+type(1)+0x01(1)+len(2)

constexpr uint8_t kPktPrefix = 0xAA;
constexpr uint8_t kPktVersion2 = 0x02;
constexpr uint8_t kPktProduct = 0x0D;  // River 2 product byte
constexpr size_t kPktHeader = 16;      // through cmd_id; payload starts here

}  // namespace

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

uint16_t crc16(const uint8_t* data, size_t len) {
  // CRC-16/ARC: reflected, poly 0xA001, init 0x0000.
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x0001) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

// --- Outer EncPacket ---

std::vector<uint8_t> encodeEncPacket(FrameType type, const uint8_t* payload,
                                     size_t len) {
  std::vector<uint8_t> f;
  f.reserve(kEncHeader + len + 2);
  f.push_back(kEncPrefix0);
  f.push_back(kEncPrefix1);
  f.push_back((uint8_t)((uint8_t)type << 4));
  f.push_back(kEncUnknown);
  uint16_t lenField = (uint16_t)(len + 2);  // +2 counts the trailing CRC16
  f.push_back((uint8_t)(lenField & 0xFF));
  f.push_back((uint8_t)(lenField >> 8));
  for (size_t i = 0; i < len; i++) f.push_back(payload[i]);
  uint16_t c = crc16(f.data(), f.size());
  f.push_back((uint8_t)(c & 0xFF));
  f.push_back((uint8_t)(c >> 8));
  return f;
}

bool decodeEncPacket(const uint8_t* data, size_t len, EncFrame& out) {
  if (len < kEncHeader + 2) return false;
  if (data[0] != kEncPrefix0 || data[1] != kEncPrefix1) return false;
  uint16_t lenField = (uint16_t)(data[4] | (data[5] << 8));
  if (lenField < 2) return false;
  size_t payloadLen = (size_t)(lenField - 2);
  if (len < kEncHeader + payloadLen + 2) return false;
  uint16_t got = (uint16_t)(data[kEncHeader + payloadLen] |
                            (data[kEncHeader + payloadLen + 1] << 8));
  if (crc16(data, kEncHeader + payloadLen) != got) return false;
  out.type = (FrameType)(data[2] >> 4);
  out.payload.assign(data + kEncHeader, data + kEncHeader + payloadLen);
  return true;
}

// --- Inner Packet V2 ---

std::vector<uint8_t> encodePacketV2(const Packet& p) {
  const size_t n = p.payload.size();
  std::vector<uint8_t> f;
  f.reserve(kPktHeader + n + 2);
  f.push_back(kPktPrefix);
  f.push_back(kPktVersion2);
  f.push_back((uint8_t)(n & 0xFF));
  f.push_back((uint8_t)(n >> 8));
  f.push_back(crc8(f.data(), 4));  // header CRC over prefix+version+len
  f.push_back(kPktProduct);
  f.push_back((uint8_t)(p.seq & 0xFF));
  f.push_back((uint8_t)((p.seq >> 8) & 0xFF));
  f.push_back((uint8_t)((p.seq >> 16) & 0xFF));
  f.push_back((uint8_t)((p.seq >> 24) & 0xFF));
  f.push_back(0x00);
  f.push_back(0x00);
  f.push_back(p.src);
  f.push_back(p.dst);
  f.push_back(p.cmdSet);
  f.push_back(p.cmdId);
  for (size_t i = 0; i < n; i++) f.push_back(p.payload[i]);
  uint16_t c = crc16(f.data(), f.size());
  f.push_back((uint8_t)(c & 0xFF));
  f.push_back((uint8_t)(c >> 8));
  return f;
}

bool decodePacketV2(const uint8_t* data, size_t len, Packet& out) {
  if (len < kPktHeader + 2) return false;
  if (data[0] != kPktPrefix || data[1] != kPktVersion2) return false;
  if (crc8(data, 4) != data[4]) return false;
  size_t payloadLen = (size_t)(data[2] | (data[3] << 8));
  if (len < kPktHeader + payloadLen + 2) return false;
  uint16_t got = (uint16_t)(data[kPktHeader + payloadLen] |
                            (data[kPktHeader + payloadLen + 1] << 8));
  if (crc16(data, kPktHeader + payloadLen) != got) return false;
  out.seq = (uint32_t)data[6] | ((uint32_t)data[7] << 8) |
            ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);
  out.src = data[12];
  out.dst = data[13];
  out.cmdSet = data[14];
  out.cmdId = data[15];
  out.payload.assign(data + kPktHeader, data + kPktHeader + payloadLen);
  return true;
}

// --- Little-endian payload readers ---

bool readU16LE(const std::vector<uint8_t>& b, size_t off, uint16_t& out) {
  if (off + 2 > b.size()) return false;
  out = (uint16_t)(b[off] | (b[off + 1] << 8));
  return true;
}

bool readU32LE(const std::vector<uint8_t>& b, size_t off, uint32_t& out) {
  if (off + 4 > b.size()) return false;
  out = (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
        ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
  return true;
}

bool readF32LE(const std::vector<uint8_t>& b, size_t off, float& out) {
  uint32_t u;
  if (!readU32LE(b, off, u)) return false;
  memcpy(&out, &u, sizeof(out));  // IEEE-754 little-endian on our targets
  return true;
}

// --- Telemetry ---

bool applyTelemetry(const Packet& p, Telemetry& t) {
  // PD heartbeat: total in/out watts.
  if (p.src == 0x02 && p.cmdSet == 0x20 && p.cmdId == 0x02) {
    uint16_t w;
    if (readU16LE(p.payload, 15, w)) { t.outputWatts = w; t.haveOutputWatts = true; }
    if (readU16LE(p.payload, 17, w)) { t.inputWatts = w; t.haveInputWatts = true; }
    return true;
  }
  // BMS heartbeat: high-resolution SoC.
  if (p.src == 0x03 && p.cmdSet == 0x20 && p.cmdId == 0x32) {
    float s;
    if (readF32LE(p.payload, 53, s)) { t.soc = s; t.haveSoc = true; }
    return true;
  }
  // EMS heartbeat: remaining time to full / empty (minutes).
  if (p.src == 0x03 && p.cmdSet == 0x20 && p.cmdId == 0x02) {
    uint32_t m;
    if (readU32LE(p.payload, 17, m)) { t.chargeRemainMin = m; t.haveChargeRemainMin = true; }
    if (readU32LE(p.payload, 21, m)) { t.dischargeRemainMin = m; t.haveDischargeRemainMin = true; }
    return true;
  }
  return false;
}

}  // namespace ecoflow
