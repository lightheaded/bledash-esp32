// EcoFlow session-key table — a fixed 65280-byte vendor constant consumed by
// EcoflowCrypto::genSessionKey(). See ecoflow_keytable.cpp for provenance.
#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr size_t kEcoflowKeyTableLen = 0xFF00;  // 65280
extern const uint8_t kEcoflowKeyTable[kEcoflowKeyTableLen];
