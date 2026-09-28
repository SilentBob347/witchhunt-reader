#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ReadingStatsTypes.h"

// The reading-stats file, streamed rather than loaded. For the web server, which runs next to
// Wi-Fi with too little heap for ReadingStatsStore's full load (a JSON document of the whole file
// plus every book's vectors, ~1.5 KB a book at up to kMaxBooks books). Each call here holds one
// parser, a small read buffer and the global day buckets, however many books the file has.
//
// Book entries are never decoded and re-encoded: they are copied through byte for byte, so a
// title's escapes and length survive untouched. Only the numbers and the docId are read.
namespace ReadingStatsFile {

constexpr char kPath[] = "/.crosspoint/reading-stats.json";

// How a pass over the file ended. NoMemory and IoError are transient — the file may be fine and a
// later pass may succeed; Malformed is the file's own fault.
enum class ScanResult : uint8_t { Ok, NoMemory, IoError, Malformed };

// One book's place in the file, for the list ordered by time.
struct IndexEntry {
  uint32_t totalSeconds = 0;
  uint32_t offset = 0;  // byte offset of the entry's opening brace
};

// What Home draws for one recent book.
struct RecentSnapshot {
  std::string docId;
  bool known = false;  // false: the history holds no entry for this book
  uint32_t totalSeconds = 0;
  uint16_t knownDays = 0;  // dated reading days, as many as a load would keep
  time_t lastReadEpoch = 0;
  uint8_t progress = 0;
};

// What a pass collects beyond the global figures.
struct ScanRequest {
  std::string findDocId;                  // Summary::target
  bool wantIndex = false;                 // Summary::byTime
  bool wantVictim = false;                // Summary::hasVictim / victimDocId / victimFirst
  std::vector<std::string> recentDocIds;  // Summary::recents, one per id, in this order
};

// One pass over the file: the global figures and whatever the request asked for. Days are
// filtered and trimmed the way a load would.
struct Summary : ReadingTotals {
  uint32_t bookCount = 0;
  uint32_t finishedBookCount = 0;
  // Over the books far enough in to count toward the global pace (see ReadingStatsStore).
  uint32_t paceSeconds = 0;
  uint32_t pacePercents = 0;

  // The requested book, every field decoded (title and author for display only).
  bool found = false;
  BookReadingStats target;
  size_t targetFirst = 0;  // offset of its opening brace

  std::vector<IndexEntry> byTime;  // descending by time; equal times in file order

  // The book the cap would evict (ReadingStatsStore::evictsBefore order).
  bool hasVictim = false;
  std::string victimDocId;
  size_t victimFirst = 0;

  std::vector<RecentSnapshot> recents;
};

ScanResult scan(HalFile& in, Summary& summary, const ScanRequest& request);

// scan() for the global figures and, optionally, one book. False unless the scan was Ok.
bool summarize(HalFile& in, Summary& summary, const std::string& findDocId = "");

// Decodes the single entry whose opening brace is at `offset` (an IndexEntry::offset).
ScanResult readBookAt(HalFile& in, size_t offset, BookReadingStats& book);

// One book as a JSON object, in the field order the device has always written. Strings are
// escaped the way ArduinoJson does: quote, backslash and control characters; UTF-8 passes through.
void writeBook(Print& out, const BookReadingStats& book);

constexpr size_t kNoEntry = static_cast<size_t>(-1);

// A rewrite of the file: new global figures and at most one entry replaced, one dropped and one
// appended. Entries are identified by the offset of their opening brace, as a scan reported them.
struct Rewrite {
  ReadingTotals totals;
  size_t replaceAt = kNoEntry;
  const BookReadingStats* replacement = nullptr;
  size_t dropAt = kNoEntry;
  const BookReadingStats* append = nullptr;
};

// Writes the whole new file to `out`: every untouched entry copied byte for byte, separators
// re-emitted. `in` may be null (no existing entries). Anything but Ok means the copy stopped early;
// the caller must not use the output.
ScanResult writeRewrite(HalFile* in, const Rewrite& rewrite, Print& out);

// The /api/stats payload: the global figures, streaks when `today` is known, and every book as
// stored plus its time-to-finish estimate (etaSeconds). `summary` must come from the same file.
void writeDashboard(HalFile& in, const Summary& summary, uint16_t today, Print& out);

// The file again without summary.target, its contribution taken out of the totals the way
// ReadingStatsStore::removeBook() does. Requires summary.found.
void writeWithoutTarget(HalFile& in, const Summary& summary, Print& out);

}  // namespace ReadingStatsFile
