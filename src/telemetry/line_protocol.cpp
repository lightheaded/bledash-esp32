#include "telemetry/line_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace telemetry {
namespace {

// Bounded appender: writes into out[*off..cap) and advances *off, returning
// false the first time something wouldn't fit. Once false, callers stop. Always
// leaves room for a terminating NUL (written by the caller at the end).
struct Appender {
  char* out;
  size_t cap;
  size_t off = 0;
  bool ok = true;

  Appender(char* o, size_t c) : out(o), cap(c) {}

  bool str(const char* s) {
    if (!ok) return false;
    size_t n = strlen(s);
    if (off + n + 1 > cap) return (ok = false);
    memcpy(out + off, s, n);
    off += n;
    return true;
  }
  bool ch(char c) {
    if (!ok) return false;
    if (off + 1 + 1 > cap) return (ok = false);
    out[off++] = c;
    return true;
  }
  // Append via snprintf; fmt must produce a bounded, short result.
  bool fmt(const char* f, ...) __attribute__((format(printf, 2, 3)));
};

bool Appender::fmt(const char* f, ...) {
  if (!ok) return false;
  va_list ap;
  va_start(ap, f);
  int n = vsnprintf(out + off, cap - off, f, ap);
  va_end(ap);
  if (n < 0 || (size_t)n + 1 > cap - off) return (ok = false);
  off += (size_t)n;
  return true;
}

// Emit one "name=value" field, prefixing a comma when it isn't the first.
void field(Appender& a, bool& first, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void field(Appender& a, bool& first, const char* fmt, ...) {
  if (!a.ok) return;
  if (!first) a.ch(',');
  first = false;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(a.out + a.off, a.cap - a.off, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n + 1 > a.cap - a.off) {
    a.ok = false;
    return;
  }
  a.off += (size_t)n;
}

}  // namespace

int encodeFields(const Sample& s, char* out, size_t cap) {
  Appender a{out, cap};
  bool first = true;
  if (s.haveFridgeTemp) field(a, first, "fridge_temp_c=%di", (int)s.fridgeTempC);
  if (s.haveFridgeSetpoint)
    field(a, first, "fridge_setpoint_c=%di", (int)s.fridgeSetpointC);
  if (s.haveFridgeOn) field(a, first, "fridge_on=%di", s.fridgeOn ? 1 : 0);
  if (s.haveSoc) field(a, first, "eco_soc_pct=%.1f", (double)s.socPct);
  if (s.haveInWatts) field(a, first, "eco_in_w=%ui", (unsigned)s.inWatts);
  if (s.haveOutWatts) field(a, first, "eco_out_w=%ui", (unsigned)s.outWatts);
  if (s.haveRemainMin)
    field(a, first, "eco_remain_min=%ui", (unsigned)s.remainMin);
  if (!a.ok) return kEncNoFit;
  out[a.off] = '\0';
  return (int)a.off;
}

int encodeRecord(const Sample& s, bool absolute, uint32_t ts, char* out,
                 size_t cap) {
  Appender a{out, cap};
  a.ch(absolute ? 'A' : 'R');
  a.fmt("%lu", (unsigned long)ts);
  a.ch(' ');
  if (!a.ok) return kEncNoFit;
  int n = encodeFields(s, out + a.off, cap - a.off);
  if (n < 0) return kEncNoFit;
  a.off += (size_t)n;
  a.ch('\n');
  if (!a.ok) return kEncNoFit;
  out[a.off] = '\0';
  return (int)a.off;
}

bool parseRecord(const char* line, size_t len, Record& out) {
  // Drop a trailing newline if present.
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
  if (len < 3) return false;  // need at least flag + 1 digit + ' '
  char flag = line[0];
  if (flag != 'A' && flag != 'R') return false;

  // Parse the timestamp digits up to the space.
  size_t i = 1;
  uint64_t ts = 0;
  bool anyDigit = false;
  for (; i < len && line[i] != ' '; i++) {
    if (line[i] < '0' || line[i] > '9') return false;
    ts = ts * 10 + (uint64_t)(line[i] - '0');
    if (ts > 0xFFFFFFFFull) return false;  // overflow guard
    anyDigit = true;
  }
  if (!anyDigit || i >= len || line[i] != ' ') return false;
  i++;  // skip the space
  if (i >= len) return false;  // empty field set

  out.absolute = (flag == 'A');
  out.ts = (uint32_t)ts;
  out.fields = line + i;
  out.fieldsLen = len - i;
  return true;
}

int recordToWireLine(const Record& r, const char* measurement, const char* tag,
                     uint32_t bootEpoch, bool bootEpochValid, char* out,
                     size_t cap) {
  uint32_t epoch;
  if (r.absolute) {
    epoch = r.ts;
  } else {
    if (!bootEpochValid) return kEncUnresolvable;
    epoch = bootEpoch + r.ts;
  }

  Appender a{out, cap};
  a.str(measurement);
  a.str(",device=");
  a.str(tag);
  a.ch(' ');
  // Copy the field substring verbatim (it is not NUL-terminated).
  if (a.ok) {
    if (a.off + r.fieldsLen + 1 > cap) {
      a.ok = false;
    } else {
      memcpy(a.out + a.off, r.fields, r.fieldsLen);
      a.off += r.fieldsLen;
    }
  }
  a.ch(' ');
  a.fmt("%lu", (unsigned long)epoch);
  a.ch('\n');
  if (!a.ok) return kEncNoFit;
  out[a.off] = '\0';
  return (int)a.off;
}

}  // namespace telemetry
