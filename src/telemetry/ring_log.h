// Two-segment ring log with a persisted upload cursor — pure, host-testable.
//
// The backbone of the telemetry feature: samples are appended as newline-
// terminated records to an "active" segment; when it fills, the OTHER segment
// (the oldest generation) is truncated and reused, so the pair holds a sliding
// window of roughly the last two weeks (~50 B/min, two ~600 KB segments). See
// plans/2026-07-24-01-telemetry-logging-upload.md.
//
// An upload cursor (segment + byte offset) tracks how far the uploader has
// drained. It advances ONLY when the caller commit()s after a 2xx, so a dropped
// connection re-sends rather than loses. Chronological read order is
// older-segment-then-active; if the log wraps while the uploader is far behind
// (offline > ~2 weeks), the evicted un-uploaded records are counted in
// droppedRecords() rather than silently lost from the count.
//
// All persistence goes through the FileStore abstraction so the logic runs
// under `pio test -e native` against an in-memory fake; the device binds it to
// LittleFS (see telemetry_littlefs.h). NOT reentrant: the owner calls append()
// and the readBatch()/commit() pair from the same loop, never interleaved.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace telemetry {

// Minimal byte-oriented file interface. Paths are opaque keys (the device maps
// them to LittleFS filenames). All ops are synchronous.
class FileStore {
 public:
  virtual ~FileStore() = default;
  // Append bytes to a file, creating it if absent. Returns false on I/O error.
  virtual bool append(const char* path, const uint8_t* data, size_t len) = 0;
  // File size in bytes; 0 if it doesn't exist.
  virtual size_t size(const char* path) = 0;
  // Read up to len bytes at offset into buf; returns bytes actually read (0 at
  // or past EOF).
  virtual size_t read(const char* path, size_t offset, uint8_t* buf,
                      size_t len) = 0;
  // Replace a file's whole contents (len 0 empties/creates it). Returns false on
  // I/O error.
  virtual bool writeAll(const char* path, const uint8_t* data, size_t len) = 0;
};

class RingLog {
 public:
  // segA/segB: the two segment paths. meta: the tiny state file (active segment
  // + cursor + dropped count). maxSegBytes: rotate when a segment would exceed
  // this. Paths must outlive the RingLog (string literals from config).
  RingLog(FileStore& fs, const char* segA, const char* segB, const char* meta,
          size_t maxSegBytes);

  // Load persisted state, or start clean (both segments emptied) if the meta
  // file is missing or malformed. Call once before use.
  void begin();

  // Append one complete, newline-terminated record. Rotates first if it
  // wouldn't fit in the active segment. Returns false on I/O error or if the
  // record alone exceeds maxSegBytes.
  bool append(const uint8_t* rec, size_t len);

  // A pending upload batch. Opaque cursor (seg,off) is where the cursor lands
  // once commit()ed; lineCount records were collected into the caller's vector.
  struct Batch {
    uint8_t seg = 0;
    size_t off = 0;
    size_t lineCount = 0;
    bool empty() const { return lineCount == 0; }
  };

  // Collect up to maxLines complete records (and at most ~maxBytes of them)
  // starting at the cursor, in chronological order, appending each (without its
  // trailing newline) to `lines`. Does not change state. Feed lines to the
  // uploader; on success call commit(batch).
  Batch readBatch(size_t maxLines, size_t maxBytes,
                  std::vector<std::string>& lines);

  // Advance the cursor to the batch's end and persist it. Call after a 2xx.
  void commit(const Batch& b);

  // Bytes not yet uploaded (diagnostics / drain summaries).
  size_t pendingBytes();
  // Records evicted by wrap before they were uploaded (offline too long).
  uint32_t droppedRecords() const { return dropped_; }

  // Test/diagnostic accessors.
  uint8_t activeSegment() const { return active_; }

 private:
  const char* seg(uint8_t i) const { return i == 0 ? segA_ : segB_; }
  void rotate();
  void persistMeta();
  // Read one newline-terminated record from seg s at offset; returns its on-disk
  // length (incl '\n') and fills `line` (without '\n'). Returns 0 if no complete
  // record is available at offset (EOF or partial tail).
  size_t readLine(uint8_t s, size_t offset, std::string& line);

  FileStore& fs_;
  const char* segA_;
  const char* segB_;
  const char* meta_;
  size_t maxSegBytes_;

  uint8_t active_ = 0;      // segment currently appended to
  uint8_t cursorSeg_ = 0;   // upload cursor segment
  size_t cursorOff_ = 0;    // upload cursor byte offset
  uint32_t dropped_ = 0;    // records lost to wrap before upload

  static constexpr size_t kMaxRecordLen = 200;  // generous; records are ~60 B
};

}  // namespace telemetry
