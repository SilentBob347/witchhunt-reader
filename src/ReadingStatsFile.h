#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ReadingStats.h"

// The reading-stats file, streamed rather than loaded. For the web server, which runs next to
// Wi-Fi with too little heap for ReadingStatsStore's full load (a JSON document of the whole file
// plus every book's vectors, ~1.5 KB a book at up to kMaxBooks books). Each call here holds one
// parser, a small read buffer and the global day buckets, however many books the file has.
//
// Book entries are never decoded and re-encoded: they are copied through byte for byte, so a
// title's escapes and length survive untouched. Only the numbers and the docId are read.
namespace ReadingStatsFile {

constexpr char kPath[] = "/.crosspoint/reading-stats.json";

// One pass over the file: the global figures the dashboard shows and, when asked for one, where a
// book sits and what it contributed. Days are filtered and trimmed the way a load would.
struct Summary {
  uint32_t totalSeconds = 0;
  uint32_t totalSessions = 0;
  uint32_t totalPagesTurned = 0;
  uint16_t longestStreak = 0;  // the persisted record
  std::vector<DayBucket> globalDays;
  uint32_t bookCount = 0;
  uint32_t finishedBookCount = 0;
  // Over the books far enough in to count toward the global pace (see ReadingStatsStore).
  uint32_t paceSeconds = 0;
  uint32_t pacePercents = 0;

  // The book summarize() was asked to find. Counters and days only; title and author are not read.
  bool found = false;
  BookReadingStats target;
  // Inclusive byte range that removes it from the books array, one separating comma included.
  size_t cutFirst = 0;
  size_t cutLast = 0;
};

// False when `in` is not a well-formed stats file (truncated, or not JSON at all).
bool summarize(HalFile& in, Summary& summary, const std::string& findDocId = "");

// The /api/stats payload: the global figures, streaks when `today` is known, and every book as
// stored plus its time-to-finish estimate (etaSeconds). `summary` must come from the same file.
void writeDashboard(HalFile& in, const Summary& summary, uint16_t today, Print& out);

// The file again without summary.target, its contribution taken out of the totals the way
// ReadingStatsStore::removeBook() does. Requires summary.found.
void writeWithoutTarget(HalFile& in, const Summary& summary, Print& out);

}  // namespace ReadingStatsFile
