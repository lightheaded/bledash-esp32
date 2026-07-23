// Resolves whether the EcoFlow GATT (Tier 2) translation units should compile.
//
// The advanced GATT path is opt-in via ECOFLOW_GATT in config.h. When it's off
// we want the heavy sources (session, crypto, the 64 KB key table, micro-ecc)
// to not be compiled at all, so the default build matches the v0.1.0 footprint.
// build_src_filter can't read a config.h macro, so instead each GATT-only .cpp
// wraps its body in `#if ECOFLOW_ENABLE_GATT_TU` using this header.
//
// Truth table:
//   - Device build, config.h present:
//       ECOFLOW_GATT == 0  -> disabled (TUs empty, stripped)
//       ECOFLOW_GATT == 1  -> enabled
//   - Native unit tests (no config.h on the include path): enabled, so the host
//     tests always build the codec/crypto regardless of any device config.
#pragma once

#if defined(__has_include)
#  if __has_include("config.h")
#    include "config.h"
#  endif
#endif

#if defined(ECOFLOW_GATT) && (ECOFLOW_GATT == 0)
#  define ECOFLOW_ENABLE_GATT_TU 0
#else
#  define ECOFLOW_ENABLE_GATT_TU 1
#endif
