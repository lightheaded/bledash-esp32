// Host unit tests for the telemetry line-protocol codec (encodeFields,
// encodeRecord, parseRecord, recordToWireLine). Run: `pio test -e native`.
#include <unity.h>

#include <string.h>

#include "telemetry/line_protocol.h"

using namespace telemetry;

static Sample fullSample() {
  Sample s;
  s.haveFridgeTemp = true;
  s.fridgeTempC = 4;
  s.haveFridgeSetpoint = true;
  s.fridgeSetpointC = -18;
  s.haveFridgeOn = true;
  s.fridgeOn = true;
  s.haveSoc = true;
  s.socPct = 87.4f;
  s.haveInWatts = true;
  s.inWatts = 0;
  s.haveOutWatts = true;
  s.outWatts = 52;
  s.haveRemainMin = true;
  s.remainMin = 433;
  return s;
}

void test_encode_fields_full(void) {
  char out[256];
  int n = encodeFields(fullSample(), out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING(
      "fridge_temp_c=4i,fridge_setpoint_c=-18i,fridge_on=1i,eco_soc_pct=87.4,"
      "eco_in_w=0i,eco_out_w=52i,eco_remain_min=433i",
      out);
  TEST_ASSERT_EQUAL_INT((int)strlen(out), n);
}

void test_encode_fields_partial_no_leading_comma(void) {
  // Only fridge fields valid (compressor off, no EcoFlow session) — a common
  // "parked and offline" sample. No leading/trailing comma, no EcoFlow fields.
  Sample s;
  s.haveFridgeTemp = true;
  s.fridgeTempC = -2;
  s.haveFridgeOn = true;
  s.fridgeOn = false;
  char out[128];
  int n = encodeFields(s, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING("fridge_temp_c=-2i,fridge_on=0i", out);
}

void test_encode_fields_only_soc(void) {
  Sample s;
  s.haveSoc = true;
  s.socPct = 100.0f;
  char out[64];
  int n = encodeFields(s, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING("eco_soc_pct=100.0", out);
}

void test_encode_fields_buffer_too_small(void) {
  char out[8];
  int n = encodeFields(fullSample(), out, sizeof(out));
  TEST_ASSERT_EQUAL_INT(kEncNoFit, n);
}

void test_encode_record_absolute(void) {
  Sample s;
  s.haveFridgeTemp = true;
  s.fridgeTempC = 4;
  char out[128];
  int n = encodeRecord(s, /*absolute=*/true, 1753364000u, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING("A1753364000 fridge_temp_c=4i\n", out);
}

void test_encode_record_relative(void) {
  Sample s;
  s.haveSoc = true;
  s.socPct = 50.0f;
  char out[128];
  int n = encodeRecord(s, /*absolute=*/false, 123u, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING("R123 eco_soc_pct=50.0\n", out);
}

void test_parse_record_absolute(void) {
  const char* line = "A1753364000 fridge_temp_c=4i,fridge_on=1i\n";
  Record r;
  TEST_ASSERT_TRUE(parseRecord(line, strlen(line), r));
  TEST_ASSERT_TRUE(r.absolute);
  TEST_ASSERT_EQUAL_UINT32(1753364000u, r.ts);
  TEST_ASSERT_EQUAL_UINT(strlen("fridge_temp_c=4i,fridge_on=1i"), r.fieldsLen);
  TEST_ASSERT_EQUAL_INT(0, strncmp(r.fields, "fridge_temp_c=4i,fridge_on=1i",
                                   r.fieldsLen));
}

void test_parse_record_relative_no_newline(void) {
  const char* line = "R42 eco_soc_pct=88.0";
  Record r;
  TEST_ASSERT_TRUE(parseRecord(line, strlen(line), r));
  TEST_ASSERT_FALSE(r.absolute);
  TEST_ASSERT_EQUAL_UINT32(42u, r.ts);
}

void test_parse_record_rejects_malformed(void) {
  Record r;
  TEST_ASSERT_FALSE(parseRecord("X123 f=1i", 9, r));     // bad flag
  TEST_ASSERT_FALSE(parseRecord("A f=1i", 6, r));        // no digits
  TEST_ASSERT_FALSE(parseRecord("A123", 4, r));          // no space/fields
  TEST_ASSERT_FALSE(parseRecord("A123 ", 5, r));         // empty field set
  TEST_ASSERT_FALSE(parseRecord("A12x3 f=1i", 10, r));   // non-digit in ts
  TEST_ASSERT_FALSE(parseRecord("", 0, r));              // empty
}

void test_record_to_wire_absolute(void) {
  const char* line = "A1753364000 fridge_temp_c=4i,fridge_on=1i\n";
  Record r;
  TEST_ASSERT_TRUE(parseRecord(line, strlen(line), r));
  char out[256];
  int n = recordToWireLine(r, "bledash", "car", /*bootEpoch=*/0,
                           /*bootEpochValid=*/false, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING(
      "bledash,device=car fridge_temp_c=4i,fridge_on=1i 1753364000\n", out);
}

void test_record_to_wire_relative_resolved(void) {
  const char* line = "R100 eco_soc_pct=75.0";
  Record r;
  TEST_ASSERT_TRUE(parseRecord(line, strlen(line), r));
  char out[256];
  int n = recordToWireLine(r, "bledash", "car", /*bootEpoch=*/1753360000u,
                           /*bootEpochValid=*/true, out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL_STRING("bledash,device=car eco_soc_pct=75.0 1753360100\n",
                           out);
}

void test_record_to_wire_relative_unresolvable(void) {
  const char* line = "R100 eco_soc_pct=75.0";
  Record r;
  TEST_ASSERT_TRUE(parseRecord(line, strlen(line), r));
  char out[256];
  int n = recordToWireLine(r, "bledash", "car", 0, /*bootEpochValid=*/false,
                           out, sizeof(out));
  TEST_ASSERT_EQUAL_INT(kEncUnresolvable, n);
}

void test_record_to_wire_buffer_too_small(void) {
  const char* line = "A1753364000 fridge_temp_c=4i";
  Record r;
  TEST_ASSERT_TRUE(parseRecord(line, strlen(line), r));
  char out[16];
  int n = recordToWireLine(r, "bledash", "car", 0, false, out, sizeof(out));
  TEST_ASSERT_EQUAL_INT(kEncNoFit, n);
}

// Round-trip: encode a sample to a record, parse it, place it on the wire, and
// confirm the field substring survived untouched.
void test_roundtrip_sample_to_wire(void) {
  char rec[256];
  int rn = encodeRecord(fullSample(), true, 1700000000u, rec, sizeof(rec));
  TEST_ASSERT_GREATER_THAN(0, rn);
  Record r;
  TEST_ASSERT_TRUE(parseRecord(rec, (size_t)rn, r));
  char wire[256];
  int wn = recordToWireLine(r, "bledash", "car", 0, false, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN(0, wn);
  TEST_ASSERT_EQUAL_STRING(
      "bledash,device=car "
      "fridge_temp_c=4i,fridge_setpoint_c=-18i,fridge_on=1i,eco_soc_pct=87.4,"
      "eco_in_w=0i,eco_out_w=52i,eco_remain_min=433i 1700000000\n",
      wire);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_fields_full);
  RUN_TEST(test_encode_fields_partial_no_leading_comma);
  RUN_TEST(test_encode_fields_only_soc);
  RUN_TEST(test_encode_fields_buffer_too_small);
  RUN_TEST(test_encode_record_absolute);
  RUN_TEST(test_encode_record_relative);
  RUN_TEST(test_parse_record_absolute);
  RUN_TEST(test_parse_record_relative_no_newline);
  RUN_TEST(test_parse_record_rejects_malformed);
  RUN_TEST(test_record_to_wire_absolute);
  RUN_TEST(test_record_to_wire_relative_resolved);
  RUN_TEST(test_record_to_wire_relative_unresolvable);
  RUN_TEST(test_record_to_wire_buffer_too_small);
  RUN_TEST(test_roundtrip_sample_to_wire);
  return UNITY_END();
}
