// InfluxDB line-protocol codec + on-flash record format — pure, host-testable.
//
// Two representations, both plain text, one function each way:
//
//   on flash (a "record"):  "<flag><ts> <fields>\n"
//   on the wire (a "line"): "<measurement>,device=<tag> <fields> <epoch>\n"
//
// The <fields> substring ("fridge_temp_c=4i,eco_soc_pct=87.4,...") is identical
// in both, so uploading is just prefixing the measurement/tag and appending a
// resolved timestamp — no re-encoding of the sample.
//
// The record's timestamp carries a flag: 'A' = absolute epoch seconds (written
// once SNTP has synced), 'R' = seconds since boot (written before the first sync
// of this boot). A relative record is placed on the real timeline at upload time
// by adding the boot epoch; if its boot never synced it can't be resolved and is
// dropped (counted). See the storage/timestamp discussion in
// plans/2026-07-24-01-telemetry-logging-upload.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "telemetry/sample.h"

namespace telemetry {

// Result codes shared by the encoders below (all return an int char-count on
// success, or one of these negatives).
enum : int {
  kEncNoFit = -1,        // output buffer too small
  kEncUnresolvable = -2  // relative record, but this boot never synced
};

// Encode just the field set: "fridge_temp_c=4i,fridge_setpoint_c=-18i,..." with
// no trailing newline. Integer-valued fields carry the line-protocol 'i' suffix;
// eco_soc_pct is a float (1 decimal). Only present fields are emitted, joined by
// commas. Returns chars written (excluding NUL), or kEncNoFit.
int encodeFields(const Sample& s, char* out, size_t cap);

// Encode a storage record line (with trailing '\n'): "<flag><ts> <fields>\n".
// absolute=true -> 'A' + epoch seconds; false -> 'R' + seconds since boot.
// Returns chars written (excluding NUL), or kEncNoFit.
int encodeRecord(const Sample& s, bool absolute, uint32_t ts, char* out,
                 size_t cap);

// A storage record parsed out of a flash line. `fields` points into the source
// buffer (not copied), so the source must outlive use.
struct Record {
  bool absolute = false;
  uint32_t ts = 0;
  const char* fields = nullptr;
  size_t fieldsLen = 0;
};

// Parse one record line (a single "<flag><ts> <fields>" with or without the
// trailing '\n'). Returns false on any malformed input.
bool parseRecord(const char* line, size_t len, Record& out);

// Compose the wire line for a parsed record: "<meas>,device=<tag> <fields>
// <epoch>\n". For a relative record, epoch = bootEpoch + r.ts; if
// bootEpochValid is false the record is unresolvable (kEncUnresolvable).
// Absolute records ignore bootEpoch. Returns chars written (excluding NUL), or
// kEncNoFit / kEncUnresolvable.
int recordToWireLine(const Record& r, const char* measurement, const char* tag,
                     uint32_t bootEpoch, bool bootEpochValid, char* out,
                     size_t cap);

}  // namespace telemetry
