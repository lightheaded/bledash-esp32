#include "telemetry/ring_log.h"

#include <stdio.h>
#include <string.h>

namespace telemetry {

RingLog::RingLog(FileStore& fs, const char* segA, const char* segB,
                 const char* meta, size_t maxSegBytes)
    : fs_(fs),
      segA_(segA),
      segB_(segB),
      meta_(meta),
      maxSegBytes_(maxSegBytes) {}

void RingLog::begin() {
  // Meta line: "v1 <active> <cursorSeg> <cursorOff> <dropped>".
  uint8_t buf[128];
  size_t n = fs_.read(meta_, 0, buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    unsigned active = 0, cseg = 0, dropped = 0;
    unsigned long coff = 0;
    if (sscanf((const char*)buf, "v1 %u %u %lu %u", &active, &cseg, &coff,
               &dropped) == 4 &&
        active <= 1 && cseg <= 1) {
      active_ = (uint8_t)active;
      cursorSeg_ = (uint8_t)cseg;
      cursorOff_ = (size_t)coff;
      dropped_ = (uint32_t)dropped;
      // Clamp a cursor that outruns the file it names (e.g. a segment shrank via
      // an out-of-band reformat) so we never read past EOF.
      size_t sz = fs_.size(seg(cursorSeg_));
      if (cursorOff_ > sz) cursorOff_ = sz;
      return;
    }
  }
  // No usable meta: start from a clean slate so we never upload stale bytes left
  // by a prior firmware. A reflash/format is a fresh start by design.
  fs_.writeAll(segA_, nullptr, 0);
  fs_.writeAll(segB_, nullptr, 0);
  active_ = 0;
  cursorSeg_ = 0;
  cursorOff_ = 0;
  dropped_ = 0;
  persistMeta();
}

void RingLog::persistMeta() {
  char line[128];
  int n = snprintf(line, sizeof(line), "v1 %u %u %lu %u\n", (unsigned)active_,
                   (unsigned)cursorSeg_, (unsigned long)cursorOff_,
                   (unsigned)dropped_);
  if (n > 0) fs_.writeAll(meta_, (const uint8_t*)line, (size_t)n);
}

void RingLog::rotate() {
  // Evict the OTHER segment (the oldest generation) and reuse it for new writes.
  uint8_t toTruncate = active_ ^ 1;

  // If the cursor still sits inside the segment we're about to evict, its
  // un-uploaded tail is being lost — count those records. (When the cursor has
  // already advanced into the active segment, older was fully drained, so
  // nothing is lost.)
  if (cursorSeg_ == toTruncate) {
    size_t off = cursorOff_;
    std::string line;
    size_t len;
    while ((len = readLine(toTruncate, off, line)) > 0) {
      dropped_++;
      off += len;
    }
  }

  fs_.writeAll(seg(toTruncate), nullptr, 0);

  // The evicted segment's data is gone; if the cursor pointed into it, restart
  // at the beginning of what is now the older segment (the old active).
  if (cursorSeg_ == toTruncate) {
    cursorSeg_ = active_;
    cursorOff_ = 0;
  }
  active_ = toTruncate;
  persistMeta();
}

bool RingLog::append(const uint8_t* rec, size_t len) {
  if (len == 0 || len > maxSegBytes_) return false;
  if (fs_.size(seg(active_)) + len > maxSegBytes_) rotate();
  return fs_.append(seg(active_), rec, len);
}

size_t RingLog::readLine(uint8_t s, size_t offset, std::string& line) {
  uint8_t buf[kMaxRecordLen];
  size_t n = fs_.read(seg(s), offset, buf, sizeof(buf));
  if (n == 0) return 0;
  // Find the newline that terminates this record.
  for (size_t i = 0; i < n; i++) {
    if (buf[i] == '\n') {
      line.assign((const char*)buf, i);  // exclude the '\n'
      return i + 1;                        // on-disk length includes the '\n'
    }
  }
  // No newline within kMaxRecordLen: a partial tail (last write torn) or EOF
  // without terminator. Treat as no complete record.
  return 0;
}

RingLog::Batch RingLog::readBatch(size_t maxLines, size_t maxBytes,
                                  std::vector<std::string>& lines) {
  uint8_t s = cursorSeg_;
  size_t off = cursorOff_;
  const uint8_t older = active_ ^ 1;
  size_t bytes = 0;
  size_t count = 0;
  std::string line;

  while (count < maxLines && bytes < maxBytes) {
    if (s == older && off >= fs_.size(seg(older))) {
      // Older segment exhausted; continue into the active segment.
      s = active_;
      off = 0;
      continue;
    }
    if (s == active_ && off >= fs_.size(seg(active_))) break;  // caught up

    size_t len = readLine(s, off, line);
    if (len == 0) {
      // No complete record here. If we're in the older segment, jump to active;
      // otherwise we're done.
      if (s == older) {
        s = active_;
        off = 0;
        continue;
      }
      break;
    }
    lines.push_back(line);
    off += len;
    bytes += len;
    count++;
  }
  Batch b;
  b.seg = s;
  b.off = off;
  b.lineCount = count;
  return b;
}

void RingLog::commit(const Batch& b) {
  if (b.lineCount == 0) return;
  cursorSeg_ = b.seg;
  cursorOff_ = b.off;
  persistMeta();
}

size_t RingLog::pendingBytes() {
  const uint8_t older = active_ ^ 1;
  size_t total = 0;
  if (cursorSeg_ == older) {
    size_t os = fs_.size(seg(older));
    total += (cursorOff_ < os) ? (os - cursorOff_) : 0;
    total += fs_.size(seg(active_));
  } else {  // cursor in active segment
    size_t as = fs_.size(seg(active_));
    total += (cursorOff_ < as) ? (as - cursorOff_) : 0;
  }
  return total;
}

}  // namespace telemetry
