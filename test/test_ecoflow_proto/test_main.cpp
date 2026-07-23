// Host unit tests for the EcoFlow codec layer (ecoflow_proto). Golden vectors
// were produced by an independent Python implementation of the same CRC params
// and frame layout (see scripts / plan). Run: `pio test -e native`.
#include <unity.h>

#include <vector>

#include "devices/ecoflow_proto.h"

using namespace ecoflow;

static std::vector<uint8_t> V(std::initializer_list<int> xs) {
  std::vector<uint8_t> v;
  for (int x : xs) v.push_back((uint8_t)x);
  return v;
}

// "123456789" is the canonical CRC check string.
static const std::vector<uint8_t> kCheck =
    V({0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39});

void test_crc8_check_value(void) {
  TEST_ASSERT_EQUAL_HEX8(0xF4, crc8(kCheck.data(), kCheck.size()));
  TEST_ASSERT_EQUAL_HEX8(0x00, crc8(nullptr, 0));
}

void test_crc16_check_value(void) {
  TEST_ASSERT_EQUAL_HEX16(0xBB3D, crc16(kCheck.data(), kCheck.size()));
  TEST_ASSERT_EQUAL_HEX16(0x0000, crc16(nullptr, 0));
}

// Golden inner V2 packet for a PD heartbeat with outputWatts=45, inputWatts=96.
static const std::vector<uint8_t> kPdInner = V({
    0xaa, 0x02, 0x18, 0x00, 0x4a, 0x0d, 0x44, 0x33, 0x22, 0x11, 0x00, 0x00,
    0x02, 0x20, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x60, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe3, 0xf9});

static const std::vector<uint8_t> kPdOuter = V({
    0x5a, 0x5a, 0x10, 0x01, 0x2c, 0x00, 0xaa, 0x02, 0x18, 0x00, 0x4a, 0x0d,
    0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x02, 0x20, 0x20, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x2d, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe3, 0xf9,
    0x9f, 0x32});

static Packet makePdPacket(void) {
  Packet p;
  p.seq = 0x11223344;
  p.src = 0x02;
  p.dst = 0x20;
  p.cmdSet = 0x20;
  p.cmdId = 0x02;
  p.payload.assign(24, 0);
  p.payload[15] = 0x2d;  // outputWatts = 45
  p.payload[17] = 0x60;  // inputWatts = 96
  return p;
}

void test_encode_inner_v2_matches_golden(void) {
  auto got = encodePacketV2(makePdPacket());
  TEST_ASSERT_EQUAL_UINT32(kPdInner.size(), got.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kPdInner.data(), got.data(), kPdInner.size());
}

void test_encode_outer_data_matches_golden(void) {
  auto got = encodeEncPacket(FrameType::kData, kPdInner.data(), kPdInner.size());
  TEST_ASSERT_EQUAL_UINT32(kPdOuter.size(), got.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kPdOuter.data(), got.data(), kPdOuter.size());
}

void test_encode_outer_command_matches_golden(void) {
  // Command frame: payload "01 00" + 40-byte (zeroed) public key.
  std::vector<uint8_t> cmd = {0x01, 0x00};
  cmd.resize(2 + 40, 0);
  auto got = encodeEncPacket(FrameType::kCommand, cmd.data(), cmd.size());
  static const std::vector<uint8_t> kCmdOuter = V({
      0x5a, 0x5a, 0x00, 0x01, 0x2c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x6b, 0x39});
  TEST_ASSERT_EQUAL_UINT32(kCmdOuter.size(), got.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kCmdOuter.data(), got.data(), kCmdOuter.size());
}

void test_decode_inner_roundtrip(void) {
  Packet out;
  TEST_ASSERT_TRUE(decodePacketV2(kPdInner.data(), kPdInner.size(), out));
  TEST_ASSERT_EQUAL_HEX32(0x11223344, out.seq);
  TEST_ASSERT_EQUAL_HEX8(0x02, out.src);
  TEST_ASSERT_EQUAL_HEX8(0x20, out.dst);
  TEST_ASSERT_EQUAL_HEX8(0x20, out.cmdSet);
  TEST_ASSERT_EQUAL_HEX8(0x02, out.cmdId);
  TEST_ASSERT_EQUAL_UINT32(24, out.payload.size());
}

void test_decode_outer_then_inner(void) {
  EncFrame ef;
  TEST_ASSERT_TRUE(decodeEncPacket(kPdOuter.data(), kPdOuter.size(), ef));
  TEST_ASSERT_EQUAL(FrameType::kData, ef.type);
  Packet out;
  TEST_ASSERT_TRUE(decodePacketV2(ef.payload.data(), ef.payload.size(), out));
  TEST_ASSERT_EQUAL_HEX8(0x02, out.src);
}

void test_decode_rejects_bad_crc(void) {
  auto bad = kPdOuter;
  bad[10] ^= 0xFF;  // corrupt a payload byte; trailer CRC no longer matches
  EncFrame ef;
  TEST_ASSERT_FALSE(decodeEncPacket(bad.data(), bad.size(), ef));

  auto badInner = kPdInner;
  badInner[4] ^= 0xFF;  // corrupt the header CRC8
  Packet out;
  TEST_ASSERT_FALSE(decodePacketV2(badInner.data(), badInner.size(), out));
}

void test_decode_rejects_short_buffer(void) {
  EncFrame ef;
  TEST_ASSERT_FALSE(decodeEncPacket(kPdOuter.data(), 3, ef));
  Packet out;
  TEST_ASSERT_FALSE(decodePacketV2(kPdInner.data(), 5, out));
}

void test_telemetry_pd_watts(void) {
  Packet p;
  TEST_ASSERT_TRUE(decodePacketV2(kPdInner.data(), kPdInner.size(), p));
  Telemetry t;
  TEST_ASSERT_TRUE(applyTelemetry(p, t));
  TEST_ASSERT_TRUE(t.haveOutputWatts);
  TEST_ASSERT_EQUAL_UINT16(45, t.outputWatts);
  TEST_ASSERT_TRUE(t.haveInputWatts);
  TEST_ASSERT_EQUAL_UINT16(96, t.inputWatts);
  TEST_ASSERT_FALSE(t.haveSoc);
}

void test_telemetry_bms_soc(void) {
  Packet p;
  p.src = 0x03;
  p.cmdSet = 0x20;
  p.cmdId = 0x32;
  p.payload.assign(60, 0);
  // f32 96.5 little-endian at offset 53: 0x42C10000
  p.payload[53] = 0x00;
  p.payload[54] = 0x00;
  p.payload[55] = 0xC1;
  p.payload[56] = 0x42;
  Telemetry t;
  TEST_ASSERT_TRUE(applyTelemetry(p, t));
  TEST_ASSERT_TRUE(t.haveSoc);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 96.5f, t.soc);
}

void test_telemetry_ems_remaining(void) {
  Packet p;
  p.src = 0x03;
  p.cmdSet = 0x20;
  p.cmdId = 0x02;
  p.payload.assign(25, 0);
  p.payload[17] = 80;  // chargeRemain = 80 min (0x50)
  p.payload[21] = 200;  // dischargeRemain = 200 min (0xC8)
  Telemetry t;
  TEST_ASSERT_TRUE(applyTelemetry(p, t));
  TEST_ASSERT_TRUE(t.haveChargeRemainMin);
  TEST_ASSERT_EQUAL_UINT32(80, t.chargeRemainMin);
  TEST_ASSERT_TRUE(t.haveDischargeRemainMin);
  TEST_ASSERT_EQUAL_UINT32(200, t.dischargeRemainMin);
}

void test_telemetry_short_payload_is_partial(void) {
  // A PD frame too short to reach the inputWatts offset yields only output.
  Packet p;
  p.src = 0x02;
  p.cmdSet = 0x20;
  p.cmdId = 0x02;
  p.payload.assign(17, 0);  // reaches offset 15 (output) but not 17 (input)
  p.payload[15] = 10;
  Telemetry t;
  TEST_ASSERT_TRUE(applyTelemetry(p, t));
  TEST_ASSERT_TRUE(t.haveOutputWatts);
  TEST_ASSERT_EQUAL_UINT16(10, t.outputWatts);
  TEST_ASSERT_FALSE(t.haveInputWatts);
}

void test_telemetry_ignores_unknown_pack(void) {
  Packet p;
  p.src = 0x99;
  p.cmdSet = 0x99;
  p.cmdId = 0x99;
  p.payload.assign(64, 0);
  Telemetry t;
  TEST_ASSERT_FALSE(applyTelemetry(p, t));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc8_check_value);
  RUN_TEST(test_crc16_check_value);
  RUN_TEST(test_encode_inner_v2_matches_golden);
  RUN_TEST(test_encode_outer_data_matches_golden);
  RUN_TEST(test_encode_outer_command_matches_golden);
  RUN_TEST(test_decode_inner_roundtrip);
  RUN_TEST(test_decode_outer_then_inner);
  RUN_TEST(test_decode_rejects_bad_crc);
  RUN_TEST(test_decode_rejects_short_buffer);
  RUN_TEST(test_telemetry_pd_watts);
  RUN_TEST(test_telemetry_bms_soc);
  RUN_TEST(test_telemetry_ems_remaining);
  RUN_TEST(test_telemetry_short_payload_is_partial);
  RUN_TEST(test_telemetry_ignores_unknown_pack);
  return UNITY_END();
}
