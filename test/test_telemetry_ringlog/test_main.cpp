// Host unit tests for the telemetry ring logger (RingLog) against an in-memory
// FileStore fake. Exercises the edge cases the plan calls out: segment wrap +
// eviction, partial upload with cursor commit, reboot mid-file (reload from
// persisted meta), and dropped-record accounting when the log wraps past an
// un-drained cursor. Run: `pio test -e native`.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "telemetry/line_protocol.h"
#include "telemetry/ring_log.h"

using namespace telemetry;

// --- In-memory FileStore -----------------------------------------------------
class MemStore : public FileStore {
 public:
  bool append(const char* path, const uint8_t* data, size_t len) override {
    auto& f = files_[path];
    f.insert(f.end(), data, data + len);
    return true;
  }
  size_t size(const char* path) override {
    auto it = files_.find(path);
    return it == files_.end() ? 0 : it->second.size();
  }
  size_t read(const char* path, size_t offset, uint8_t* buf,
              size_t len) override {
    auto it = files_.find(path);
    if (it == files_.end() || offset >= it->second.size()) return 0;
    size_t n = it->second.size() - offset;
    if (n > len) n = len;
    memcpy(buf, it->second.data() + offset, n);
    return n;
  }
  bool writeAll(const char* path, const uint8_t* data, size_t len) override {
    auto& f = files_[path];
    f.assign(data, data + len);
    return true;
  }

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
};

static const char* kSegA = "log.a";
static const char* kSegB = "log.b";
static const char* kMeta = "tlog.meta";

// Append a record "A<ts> x=1i\n". ts in [10,99] keeps every record 9 bytes wide,
// so segment capacities are exact and easy to reason about.
static bool appendTs(RingLog& log, unsigned ts) {
  char rec[32];
  int n = snprintf(rec, sizeof(rec), "A%u x=1i\n", ts);
  return log.append((const uint8_t*)rec, (size_t)n);
}

// Pull every pending record and return the parsed timestamps (ascending), then
// commit so a subsequent call sees only new data.
static std::vector<unsigned> drainTs(RingLog& log) {
  std::vector<std::string> lines;
  auto b = log.readBatch(1000, 1u << 20, lines);
  std::vector<unsigned> ts;
  for (auto& l : lines) {
    Record r;
    TEST_ASSERT_TRUE(parseRecord(l.data(), l.size(), r));
    ts.push_back(r.ts);
  }
  log.commit(b);
  return ts;
}

void test_basic_append_drain(void) {
  MemStore fs;
  RingLog log(fs, kSegA, kSegB, kMeta, 4096);
  log.begin();
  for (unsigned t = 10; t < 15; t++) TEST_ASSERT_TRUE(appendTs(log, t));
  auto ts = drainTs(log);
  TEST_ASSERT_EQUAL_UINT(5, ts.size());
  for (unsigned i = 0; i < ts.size(); i++)
    TEST_ASSERT_EQUAL_UINT(10 + i, ts[i]);
  // Fully drained: nothing left.
  TEST_ASSERT_EQUAL_UINT(0, drainTs(log).size());
}

void test_partial_upload_cursor_advances(void) {
  MemStore fs;
  RingLog log(fs, kSegA, kSegB, kMeta, 4096);
  log.begin();
  for (unsigned t = 10; t < 16; t++) TEST_ASSERT_TRUE(appendTs(log, t));

  // First batch of 3, commit.
  std::vector<std::string> lines;
  auto b1 = log.readBatch(3, 1u << 20, lines);
  TEST_ASSERT_EQUAL_UINT(3, b1.lineCount);
  log.commit(b1);

  // Next batch picks up exactly where we left off.
  auto rest = drainTs(log);
  TEST_ASSERT_EQUAL_UINT(3, rest.size());
  TEST_ASSERT_EQUAL_UINT(13, rest[0]);
  TEST_ASSERT_EQUAL_UINT(15, rest[2]);
}

void test_readbatch_without_commit_is_idempotent(void) {
  MemStore fs;
  RingLog log(fs, kSegA, kSegB, kMeta, 4096);
  log.begin();
  for (unsigned t = 10; t < 13; t++) appendTs(log, t);
  std::vector<std::string> a, b;
  log.readBatch(1000, 1u << 20, a);
  log.readBatch(1000, 1u << 20, b);  // no commit between -> same data
  TEST_ASSERT_EQUAL_UINT(3, a.size());
  TEST_ASSERT_EQUAL_UINT(3, b.size());
}

void test_wrap_evicts_oldest_and_counts_dropped(void) {
  MemStore fs;
  // 9 bytes/record; 36-byte segment => 4 records per segment.
  RingLog log(fs, kSegA, kSegB, kMeta, 36);
  log.begin();
  const unsigned kFirst = 10, kCount = 30;
  for (unsigned t = kFirst; t < kFirst + kCount; t++)
    TEST_ASSERT_TRUE(appendTs(log, t));

  // Never uploaded during the run, so the wrap evicted the oldest records.
  std::vector<std::string> lines;
  auto b = log.readBatch(1000, 1u << 20, lines);
  std::vector<unsigned> ts;
  for (auto& l : lines) {
    Record r;
    TEST_ASSERT_TRUE(parseRecord(l.data(), l.size(), r));
    ts.push_back(r.ts);
  }

  TEST_ASSERT_GREATER_THAN_UINT(0, ts.size());
  // Newest record always survives.
  TEST_ASSERT_EQUAL_UINT(kFirst + kCount - 1, ts.back());
  // Retained records are a contiguous ascending suffix of the append stream.
  for (size_t i = 1; i < ts.size(); i++)
    TEST_ASSERT_EQUAL_UINT(ts[i - 1] + 1, ts[i]);
  // Everything not retained was accounted as dropped.
  TEST_ASSERT_EQUAL_UINT(kCount - ts.size(), log.droppedRecords());
}

void test_reboot_reloads_cursor_and_active(void) {
  MemStore fs;
  {
    RingLog log(fs, kSegA, kSegB, kMeta, 4096);
    log.begin();
    for (unsigned t = 10; t < 14; t++) appendTs(log, t);
    // Upload/commit the first two.
    std::vector<std::string> lines;
    auto b = log.readBatch(2, 1u << 20, lines);
    log.commit(b);
  }
  // "Reboot": a fresh RingLog over the same store reloads persisted meta.
  RingLog log2(fs, kSegA, kSegB, kMeta, 4096);
  log2.begin();
  auto rest = drainTs(log2);
  TEST_ASSERT_EQUAL_UINT(2, rest.size());
  TEST_ASSERT_EQUAL_UINT(12, rest[0]);
  TEST_ASSERT_EQUAL_UINT(13, rest[1]);
  // Logging continues after the reboot.
  TEST_ASSERT_TRUE(appendTs(log2, 14));
  auto more = drainTs(log2);
  TEST_ASSERT_EQUAL_UINT(1, more.size());
  TEST_ASSERT_EQUAL_UINT(14, more[0]);
}

void test_reboot_survives_a_wrap(void) {
  MemStore fs;
  {
    RingLog log(fs, kSegA, kSegB, kMeta, 36);  // 4 records/segment
    log.begin();
    for (unsigned t = 10; t < 20; t++) appendTs(log, t);  // wraps
  }
  RingLog log2(fs, kSegA, kSegB, kMeta, 36);
  log2.begin();
  std::vector<std::string> lines;
  auto b = log2.readBatch(1000, 1u << 20, lines);
  TEST_ASSERT_GREATER_THAN_UINT(0, lines.size());
  Record r;
  TEST_ASSERT_TRUE(parseRecord(lines.back().data(), lines.back().size(), r));
  TEST_ASSERT_EQUAL_UINT(19, r.ts);  // newest preserved across the reboot
}

void test_no_meta_starts_clean(void) {
  MemStore fs;
  // Pre-seed a segment with junk from a "previous firmware" and no meta file.
  const char* junk = "garbage\n";
  fs.writeAll(kSegA, (const uint8_t*)junk, strlen(junk));
  RingLog log(fs, kSegA, kSegB, kMeta, 4096);
  log.begin();  // no valid meta -> wipes segments
  TEST_ASSERT_EQUAL_UINT(0, drainTs(log).size());
  appendTs(log, 10);
  auto ts = drainTs(log);
  TEST_ASSERT_EQUAL_UINT(1, ts.size());
  TEST_ASSERT_EQUAL_UINT(10, ts[0]);
}

void test_append_rejects_oversize_record(void) {
  MemStore fs;
  RingLog log(fs, kSegA, kSegB, kMeta, 16);
  log.begin();
  char big[64];
  memset(big, 'x', sizeof(big));
  TEST_ASSERT_FALSE(log.append((const uint8_t*)big, sizeof(big)));
}

void test_pending_bytes_tracks_backlog(void) {
  MemStore fs;
  RingLog log(fs, kSegA, kSegB, kMeta, 4096);
  log.begin();
  TEST_ASSERT_EQUAL_UINT(0, log.pendingBytes());
  for (unsigned t = 10; t < 13; t++) appendTs(log, t);  // 3 * 9 bytes
  TEST_ASSERT_EQUAL_UINT(27, log.pendingBytes());
  std::vector<std::string> lines;
  auto b = log.readBatch(1000, 1u << 20, lines);
  log.commit(b);
  TEST_ASSERT_EQUAL_UINT(0, log.pendingBytes());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_basic_append_drain);
  RUN_TEST(test_partial_upload_cursor_advances);
  RUN_TEST(test_readbatch_without_commit_is_idempotent);
  RUN_TEST(test_wrap_evicts_oldest_and_counts_dropped);
  RUN_TEST(test_reboot_reloads_cursor_and_active);
  RUN_TEST(test_reboot_survives_a_wrap);
  RUN_TEST(test_no_meta_starts_clean);
  RUN_TEST(test_append_rejects_oversize_record);
  RUN_TEST(test_pending_bytes_tracks_backlog);
  return UNITY_END();
}
