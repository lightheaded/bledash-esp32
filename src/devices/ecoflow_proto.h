// EcoFlow River 2-series wire protocol — pure, Arduino-free codec layer.
// Two nested little-endian frames plus fixed-offset telemetry structs. Kept free
// of NimBLE/Arduino so it can be unit-tested on the host (see test/, `pio test
// -e native`). Protocol details and field offsets: plans/2026-07-15-01-...md and
// docs/protocols/ecoflow.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace ecoflow {

// --- CRCs (params from the reference implementations) ---
// CRC-8/CCITT: poly 0x07, init 0x00, no reflection. Guards the inner header.
uint8_t crc8(const uint8_t* data, size_t len);
// CRC-16/ARC: poly 0x8005 reflected (0xA001), init 0x0000. Guards both frame
// trailers.
uint16_t crc16(const uint8_t* data, size_t len);

// --- Outer EncPacket (prefix 0x5A5A) — what actually goes over BLE ---
enum class FrameType : uint8_t { kCommand = 0x0, kData = 0x1 };  // stored as type<<4

// Wrap payload in a 0x5A5A frame with a trailing CRC16. Command frames carry the
// plaintext handshake; data frames carry AES-CBC ciphertext (encrypt before
// calling this).
std::vector<uint8_t> encodeEncPacket(FrameType type, const uint8_t* payload,
                                     size_t len);

struct EncFrame {
  FrameType type = FrameType::kCommand;
  std::vector<uint8_t> payload;
};
// Parse one complete 0x5A5A frame. Returns false on bad prefix, short buffer,
// length mismatch, or CRC failure.
bool decodeEncPacket(const uint8_t* data, size_t len, EncFrame& out);

// --- Inner Packet (prefix 0xAA), version 2 (River 2 Max) ---
struct Packet {
  uint32_t seq = 0;
  uint8_t src = 0;
  uint8_t dst = 0;
  uint8_t cmdSet = 0;
  uint8_t cmdId = 0;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> encodePacketV2(const Packet& p);
// Parse one V2 inner packet. Returns false on bad prefix/version, short buffer,
// or CRC8/CRC16 failure.
bool decodePacketV2(const uint8_t* data, size_t len, Packet& out);

// --- Telemetry (fixed-offset little-endian structs; River 2 Max is NOT
// protobuf). Offsets are into the inner Packet payload. Frames can be short, so
// each field is only populated when the payload reaches its offset. ---
struct Telemetry {
  bool haveInputWatts = false;
  uint16_t inputWatts = 0;  // charge power in (watts_in_sum)
  bool haveOutputWatts = false;
  uint16_t outputWatts = 0;  // load power out (watts_out_sum)
  bool haveSoc = false;
  float soc = 0.0f;  // high-res state of charge % (f32_show_soc)
  bool haveChargeRemainMin = false;
  uint32_t chargeRemainMin = 0;  // minutes to full (chg_remain_time)
  bool haveDischargeRemainMin = false;
  uint32_t dischargeRemainMin = 0;  // minutes to empty (dsg_remain_time)
};

// Merge any recognized fields from one decoded packet into t. Returns true if
// the packet was a known telemetry pack (by src/cmdSet/cmdId), false otherwise.
bool applyTelemetry(const Packet& p, Telemetry& t);

// Little-endian payload readers (bounds-checked helpers used above; exposed for
// tests). Return false and leave out unchanged if the field is out of range.
bool readU16LE(const std::vector<uint8_t>& b, size_t off, uint16_t& out);
bool readU32LE(const std::vector<uint8_t>& b, size_t off, uint32_t& out);
bool readF32LE(const std::vector<uint8_t>& b, size_t off, float& out);

}  // namespace ecoflow
