// Host unit tests for the EcoFlow handshake crypto (ecoflow_crypto). Golden
// values were produced independently: MD5 via Python hashlib, AES-128-CBC via
// pyca/cryptography, secp160r1 ECDH via a standalone pure-Python curve
// implementation, and genSessionKey/auth from the reference algorithm against
// the real key table. Run: `pio test -e native`.
#include <unity.h>

#include <string.h>

#include <vector>

#include "devices/ecoflow_crypto.h"

using namespace ecoflow;

// ---- golden vectors (generated; see plan / scratchpad) ----
static const uint8_t kEcdhPrivA[] = {
    0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33};
static const uint8_t kEcdhPubB[] = {
    0xdf, 0x8f, 0x36, 0x74, 0x05, 0x8b, 0xbc, 0x44, 0x23, 0x28, 0xb4, 0xd5, 0x88,
    0xfe, 0xe4, 0x13, 0x88, 0xf3, 0x79, 0x11, 0xd4, 0x39, 0xa3, 0x84, 0x44, 0x7b,
    0xf7, 0x24, 0x7c, 0x52, 0x77, 0xbb, 0x66, 0xf6, 0xf9, 0x6c, 0xe1, 0x52, 0xeb,
    0xee};
static const uint8_t kEcdhSharedX[] = {
    0xfb, 0xda, 0xed, 0x9e, 0x72, 0xad, 0xff, 0x27, 0x25, 0x91,
    0xa3, 0x6a, 0x08, 0x7f, 0x23, 0x0a, 0xa0, 0xf5, 0xdb, 0x29};
static const uint8_t kEcdhIv[] = {0xbc, 0x1b, 0xb2, 0xdf, 0xce, 0x55, 0xad, 0x3c,
                                  0xc7, 0xd5, 0xa1, 0xe2, 0x89, 0xc4, 0x83, 0x89};
static const uint8_t kMd5Msg[] = {0x62, 0x6c, 0x65, 0x64, 0x61, 0x73, 0x68, 0x2d,
                                  0x65, 0x63, 0x6f, 0x66, 0x6c, 0x6f, 0x77};
static const uint8_t kMd5Digest[] = {0xc1, 0x6d, 0x9c, 0x64, 0x7a, 0x9d, 0x81, 0xfb,
                                     0x57, 0x0f, 0x93, 0xb6, 0x53, 0xac, 0xb7, 0x36};
static const uint8_t kAesKey[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const uint8_t kAesIv[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t kAesPlain[] = {0x45, 0x63, 0x6f, 0x46, 0x6c, 0x6f, 0x77, 0x20,
                                    0x52, 0x69, 0x76, 0x65, 0x72, 0x32, 0x21, 0x21};
static const uint8_t kAesCipher[] = {0x53, 0x9c, 0x3d, 0xa5, 0x95, 0x51, 0x2c, 0x21,
                                     0x1b, 0x93, 0xdd, 0x5e, 0x13, 0x06, 0xaf, 0x53};
static const uint8_t kSeed[] = {0x2a, 0x07};
static const uint8_t kSrand[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t kSessionKey[] = {0x9f, 0x99, 0x34, 0x33, 0x1c, 0x57, 0x23, 0x90,
                                      0x8a, 0x46, 0x1f, 0xb7, 0xc0, 0xa7, 0xe0, 0xed};
static const char kAuthUserId[] = "1234567890123456789";
static const char kAuthSerial[] = "R611ZE1AXJ3K0907";
static const uint8_t kAuthPayload[] = {
    0x39, 0x46, 0x43, 0x38, 0x37, 0x38, 0x42, 0x30, 0x34, 0x37, 0x34,
    0x33, 0x41, 0x43, 0x45, 0x39, 0x31, 0x42, 0x44, 0x36, 0x34, 0x32,
    0x36, 0x41, 0x32, 0x39, 0x41, 0x33, 0x35, 0x42, 0x35, 0x46};

void test_md5_vector(void) {
  uint8_t out[16];
  md5(kMd5Msg, sizeof(kMd5Msg), out);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMd5Digest, out, 16);
}

void test_md5_empty(void) {
  // MD5("") = d41d8cd98f00b204e9800998ecf8427e
  static const uint8_t kEmpty[] = {0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
                                   0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e};
  uint8_t out[16];
  md5((const uint8_t*)"", 0, out);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kEmpty, out, 16);
}

void test_aes_cbc_block(void) {
  uint8_t ct[16];
  aes128CbcEncrypt(kAesKey, kAesIv, kAesPlain, 16, ct);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kAesCipher, ct, 16);
  uint8_t pt[16];
  aes128CbcDecrypt(kAesKey, kAesIv, kAesCipher, 16, pt);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kAesPlain, pt, 16);
}

void test_aes_cbc_pkcs7_roundtrip(void) {
  const char* msg = "authenticate me, EcoFlow";  // 24 bytes -> pads to 32
  auto ct = aes128CbcEncryptPkcs7(kAesKey, kAesIv, (const uint8_t*)msg, strlen(msg));
  TEST_ASSERT_EQUAL_UINT32(32, ct.size());
  std::vector<uint8_t> pt;
  TEST_ASSERT_TRUE(aes128CbcDecryptPkcs7(kAesKey, kAesIv, ct.data(), ct.size(), pt));
  TEST_ASSERT_EQUAL_UINT32(strlen(msg), pt.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, pt.data(), strlen(msg));
}

void test_aes_cbc_pkcs7_block_aligned_adds_block(void) {
  // 16-byte input must gain a full 16-byte padding block.
  auto ct = aes128CbcEncryptPkcs7(kAesKey, kAesIv, kAesPlain, 16);
  TEST_ASSERT_EQUAL_UINT32(32, ct.size());
  std::vector<uint8_t> pt;
  TEST_ASSERT_TRUE(aes128CbcDecryptPkcs7(kAesKey, kAesIv, ct.data(), ct.size(), pt));
  TEST_ASSERT_EQUAL_UINT32(16, pt.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kAesPlain, pt.data(), 16);
}

void test_aes_pkcs7_rejects_bad_padding(void) {
  uint8_t bad[16];
  aes128CbcEncrypt(kAesKey, kAesIv, kAesPlain, 16, bad);  // plaintext isn't padded
  std::vector<uint8_t> out;
  // Decrypting a block whose plaintext doesn't end in valid PKCS7 must fail.
  TEST_ASSERT_FALSE(aes128CbcDecryptPkcs7(kAesKey, kAesIv, bad, 16, out));
}

void test_ecdh_shared_secret_matches_golden(void) {
  uint8_t out[20];
  TEST_ASSERT_TRUE(ecdhSharedSecret(kEcdhPrivA, kEcdhPubB, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kEcdhSharedX, out, 20);
}

void test_ecdh_roundtrip_generated_keys(void) {
  // Two fresh keypairs must agree on a shared secret (validates make_key + the
  // curve wiring end to end, independent of the fixed golden vector).
  uint8_t privA[21], pubA[40], privB[21], pubB[40];
  TEST_ASSERT_TRUE(ecdhMakeKey(privA, pubA));
  TEST_ASSERT_TRUE(ecdhMakeKey(privB, pubB));
  uint8_t sA[20], sB[20];
  TEST_ASSERT_TRUE(ecdhSharedSecret(privA, pubB, sA));
  TEST_ASSERT_TRUE(ecdhSharedSecret(privB, pubA, sB));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(sA, sB, 20);
}

void test_derive_iv_from_shared(void) {
  uint8_t iv[16];
  deriveIvFromShared(kEcdhSharedX, iv);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kEcdhIv, iv, 16);
}

void test_gen_session_key_matches_golden(void) {
  uint8_t out[16];
  genSessionKey(kSeed, kSrand, out);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSessionKey, out, 16);
}

void test_build_auth_payload_matches_golden(void) {
  uint8_t out[32];
  buildAuthPayload(kAuthUserId, kAuthSerial, out);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kAuthPayload, out, 32);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_md5_vector);
  RUN_TEST(test_md5_empty);
  RUN_TEST(test_aes_cbc_block);
  RUN_TEST(test_aes_cbc_pkcs7_roundtrip);
  RUN_TEST(test_aes_cbc_pkcs7_block_aligned_adds_block);
  RUN_TEST(test_aes_pkcs7_rejects_bad_padding);
  RUN_TEST(test_ecdh_shared_secret_matches_golden);
  RUN_TEST(test_ecdh_roundtrip_generated_keys);
  RUN_TEST(test_derive_iv_from_shared);
  RUN_TEST(test_gen_session_key_matches_golden);
  RUN_TEST(test_build_auth_payload_matches_golden);
  return UNITY_END();
}
