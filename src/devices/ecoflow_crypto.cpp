#include "devices/ecoflow_crypto.h"

#include <string.h>

#include <uECC.h>

#include "devices/ecoflow_keytable.h"

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_system.h>  // esp_random
#else
#include <stdlib.h>  // rand
#endif

namespace ecoflow {

// ===================== MD5 (RFC 1321, compact) =====================
namespace {

struct Md5Ctx {
  uint32_t a, b, c, d;
  uint64_t len;
  uint8_t buf[64];
  size_t idx;
};

inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

const uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5Block(Md5Ctx& c, const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++)
    m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
           ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
  uint32_t A = c.a, B = c.b, C = c.c, D = c.d;
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) { f = (B & C) | (~B & D); g = i; }
    else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) & 15; }
    else { f = C ^ (B | ~D); g = (7 * i) & 15; }
    uint32_t tmp = D;
    D = C;
    C = B;
    B = B + rotl(A + f + kK[i] + m[g], kS[i]);
    A = tmp;
  }
  c.a += A; c.b += B; c.c += C; c.d += D;
}

void md5Init(Md5Ctx& c) {
  c.a = 0x67452301; c.b = 0xefcdab89; c.c = 0x98badcfe; c.d = 0x10325476;
  c.len = 0; c.idx = 0;
}
void md5Update(Md5Ctx& c, const uint8_t* data, size_t len) {
  c.len += len;
  for (size_t i = 0; i < len; i++) {
    c.buf[c.idx++] = data[i];
    if (c.idx == 64) { md5Block(c, c.buf); c.idx = 0; }
  }
}
void md5Final(Md5Ctx& c, uint8_t out[16]) {
  uint64_t bits = c.len * 8;
  uint8_t pad = 0x80;
  md5Update(c, &pad, 1);
  uint8_t zero = 0;
  while (c.idx != 56) md5Update(c, &zero, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (8 * i));
  md5Update(c, lb, 8);
  uint32_t v[4] = {c.a, c.b, c.c, c.d};
  for (int i = 0; i < 4; i++) {
    out[i * 4] = (uint8_t)v[i];
    out[i * 4 + 1] = (uint8_t)(v[i] >> 8);
    out[i * 4 + 2] = (uint8_t)(v[i] >> 16);
    out[i * 4 + 3] = (uint8_t)(v[i] >> 24);
  }
}

}  // namespace

void md5(const uint8_t* data, size_t len, uint8_t out[16]) {
  Md5Ctx c;
  md5Init(c);
  md5Update(c, data, len);
  md5Final(c, out);
}

// ===================== AES-128 (FIPS-197) =====================
namespace {

const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

uint8_t kInvSbox[256];
bool kInvReady = false;
void buildInvSbox() {
  if (kInvReady) return;
  for (int i = 0; i < 256; i++) kInvSbox[kSbox[i]] = (uint8_t)i;
  kInvReady = true;
}

inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1) p ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return p;
}

struct Aes128 {
  uint8_t rk[176];
  void expand(const uint8_t key[16]) {
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4];
      memcpy(t, rk + i - 4, 4);
      if (i % 16 == 0) {
        uint8_t tmp = t[0];
        t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
        t[1] = kSbox[t[2]];
        t[2] = kSbox[t[3]];
        t[3] = kSbox[tmp];
        rcon = xtime(rcon);
      }
      for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
  }
  void encryptBlock(uint8_t s[16]) const {
    addRoundKey(s, 0);
    for (int r = 1; r < 10; r++) {
      subBytes(s); shiftRows(s); mixColumns(s); addRoundKey(s, r);
    }
    subBytes(s); shiftRows(s); addRoundKey(s, 10);
  }
  void decryptBlock(uint8_t s[16]) const {
    addRoundKey(s, 10);
    for (int r = 9; r >= 1; r--) {
      invShiftRows(s); invSubBytes(s); addRoundKey(s, r); invMixColumns(s);
    }
    invShiftRows(s); invSubBytes(s); addRoundKey(s, 0);
  }
  void addRoundKey(uint8_t s[16], int r) const {
    for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
  }
  static void subBytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];
  }
  static void invSubBytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] = kInvSbox[s[i]];
  }
  static void shiftRows(uint8_t s[16]) {
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
  }
  static void invShiftRows(uint8_t s[16]) {
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
  }
  static void mixColumns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
      uint8_t* col = s + c * 4;
      uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
      col[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
      col[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
      col[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
      col[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
  }
  static void invMixColumns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
      uint8_t* col = s + c * 4;
      uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
      col[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
      col[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
      col[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
      col[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
  }
};

}  // namespace

void aes128CbcEncrypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, size_t len, uint8_t* out) {
  Aes128 aes;
  aes.expand(key);
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t off = 0; off < len; off += 16) {
    uint8_t blk[16];
    for (int i = 0; i < 16; i++) blk[i] = in[off + i] ^ prev[i];
    aes.encryptBlock(blk);
    memcpy(out + off, blk, 16);
    memcpy(prev, blk, 16);
  }
}

void aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, size_t len, uint8_t* out) {
  buildInvSbox();
  Aes128 aes;
  aes.expand(key);
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t off = 0; off < len; off += 16) {
    uint8_t blk[16];
    memcpy(blk, in + off, 16);
    uint8_t cipher[16];
    memcpy(cipher, blk, 16);
    aes.decryptBlock(blk);
    for (int i = 0; i < 16; i++) out[off + i] = blk[i] ^ prev[i];
    memcpy(prev, cipher, 16);
  }
}

std::vector<uint8_t> aes128CbcEncryptPkcs7(const uint8_t key[16],
                                           const uint8_t iv[16],
                                           const uint8_t* in, size_t len) {
  size_t padLen = 16 - (len % 16);  // always 1..16 (adds a full block if aligned)
  std::vector<uint8_t> buf(len + padLen);
  memcpy(buf.data(), in, len);
  for (size_t i = 0; i < padLen; i++) buf[len + i] = (uint8_t)padLen;
  std::vector<uint8_t> out(buf.size());
  aes128CbcEncrypt(key, iv, buf.data(), buf.size(), out.data());
  return out;
}

bool aes128CbcDecryptPkcs7(const uint8_t key[16], const uint8_t iv[16],
                           const uint8_t* in, size_t len,
                           std::vector<uint8_t>& out) {
  size_t whole = len - (len % 16);  // ignore trailing partial block
  if (whole == 0) return false;
  std::vector<uint8_t> buf(whole);
  aes128CbcDecrypt(key, iv, in, whole, buf.data());
  uint8_t pad = buf[whole - 1];
  if (pad == 0 || pad > 16 || pad > whole) return false;
  for (size_t i = 0; i < pad; i++)
    if (buf[whole - 1 - i] != pad) return false;
  out.assign(buf.begin(), buf.begin() + (whole - pad));
  return true;
}

// ===================== secp160r1 ECDH =====================
namespace {
int ueccRng(uint8_t* dest, unsigned size) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  while (size >= 4) {
    uint32_t r = esp_random();
    memcpy(dest, &r, 4);
    dest += 4;
    size -= 4;
  }
  if (size) {
    uint32_t r = esp_random();
    memcpy(dest, &r, size);
  }
#else
  for (unsigned i = 0; i < size; i++) dest[i] = (uint8_t)(rand() & 0xFF);
#endif
  return 1;
}
}  // namespace

bool ecdhSharedSecret(const uint8_t priv[21], const uint8_t peerPub[40],
                      uint8_t out[20]) {
  return uECC_shared_secret(peerPub, priv, out, uECC_secp160r1()) == 1;
}

bool ecdhMakeKey(uint8_t priv[21], uint8_t pub[40]) {
  uECC_set_rng(&ueccRng);
  return uECC_make_key(pub, priv, uECC_secp160r1()) == 1;
}

// ===================== EcoFlow derivations =====================

void deriveIvFromShared(const uint8_t shared[20], uint8_t iv[16]) {
  md5(shared, 20, iv);
}

void genSessionKey(const uint8_t seed[2], const uint8_t srand[16],
                   uint8_t out[16]) {
  size_t pos = (size_t)seed[0] * 0x10 + (size_t)((seed[1] - 1) & 0xFF) * 0x100;
  uint8_t blob[32];
  memcpy(blob, kEcoflowKeyTable + pos, 16);
  memcpy(blob + 16, srand, 16);
  md5(blob, 32, out);
}

void buildAuthPayload(const char* userId, const char* serial, uint8_t out[32]) {
  uint8_t combined[64];
  size_t ul = strlen(userId), sl = strlen(serial);
  if (ul + sl > sizeof(combined)) { memset(out, 0, 32); return; }
  memcpy(combined, userId, ul);
  memcpy(combined + ul, serial, sl);
  uint8_t digest[16];
  md5(combined, ul + sl, digest);
  static const char* kHex = "0123456789ABCDEF";
  for (int i = 0; i < 16; i++) {
    out[i * 2] = (uint8_t)kHex[digest[i] >> 4];
    out[i * 2 + 1] = (uint8_t)kHex[digest[i] & 0xF];
  }
}

}  // namespace ecoflow
