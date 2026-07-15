// EcoFlow handshake crypto — Arduino-free so it unit-tests on the host.
// Portable vendored MD5 + AES-128 (data volumes are tiny; HW accel is moot) and
// secp160r1 ECDH via micro-ecc. Algorithm details: plans/2026-07-15-01-...md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace ecoflow {

// --- MD5 (RFC 1321) ---
void md5(const uint8_t* data, size_t len, uint8_t out[16]);

// --- AES-128-CBC ---
// No-padding variants: len must be a multiple of 16.
void aes128CbcEncrypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, size_t len, uint8_t* out);
void aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, size_t len, uint8_t* out);
// PKCS#7 variants — what the EcoFlow data frames use.
std::vector<uint8_t> aes128CbcEncryptPkcs7(const uint8_t key[16],
                                           const uint8_t iv[16],
                                           const uint8_t* in, size_t len);
// Decrypts whole 16-byte blocks (trailing partial bytes ignored, matching the
// reference) and strips PKCS#7. Returns false on bad padding / empty input.
bool aes128CbcDecryptPkcs7(const uint8_t key[16], const uint8_t iv[16],
                           const uint8_t* in, size_t len,
                           std::vector<uint8_t>& out);

// --- secp160r1 ECDH (micro-ecc) ---
// Shared secret = 20-byte big-endian X of priv*peerPub. priv is 21 bytes
// (secp160r1's order exceeds 2^160), peerPub is 40 bytes (X||Y). No RNG needed.
bool ecdhSharedSecret(const uint8_t priv[21], const uint8_t peerPub[40],
                      uint8_t out[20]);
// Generate an ephemeral keypair (priv 21B, pub 40B). Uses esp_random on device,
// rand() on host. Returns false on failure.
bool ecdhMakeKey(uint8_t priv[21], uint8_t pub[40]);

// --- EcoFlow-specific derivations ---
// Session IV = MD5(shared secret).
void deriveIvFromShared(const uint8_t shared[20], uint8_t iv[16]);
// Session key = MD5( keyTable[pos:pos+16] || srand[0:16] ), where
// pos = seed[0]*0x10 + ((seed[1]-1)&0xFF)*0x100.
void genSessionKey(const uint8_t seed[2], const uint8_t srand[16],
                   uint8_t out[16]);
// Auth payload = 32 ASCII bytes = UPPERCASE hex of MD5(userId || serial).
void buildAuthPayload(const char* userId, const char* serial, uint8_t out[32]);

}  // namespace ecoflow
