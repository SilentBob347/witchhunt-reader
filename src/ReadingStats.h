#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "ReadingStatsFile.h"
#include "ReadingStatsTypes.h"

// Helpers — both return 0 when HalClock is unsynced (caller should skip).
uint16_t localDayIndexFromEpoch(time_t epoch);
uint16_t currentLocalDayIndex();

// The reading history: one file, /.crosspoint/reading-stats.json (ReadingStatsFile::kPath), all
// books and the global figures.
//
// The store holds no history. Every query streams the file (ReadingStatsFile) and returns only
// what was asked for; every update is a streamed rewrite — scan, merge the one book, write a
// temporary file, read it back, swap it in — so memory does not grow with the number of books.
// Loading the whole history used to cost ~1.5 KB a book at load and failed at the session end at
// roughly 25-30 books, losing that session (memory audit 2026-09, F8). The only resident state is
// the recent-books cache, which every update keeps current.
//
// Deleting a book from the card never touches its history: only removeBook() takes a book out.
class ReadingStatsStore {
  static ReadingStatsStore instance;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  explicit ReadingStatsStore(std::string path = ReadingStatsFile::kPath) : path_(std::move(path)) {}

  // ---- Updates ----------------------------------------------------------------------------------
  enum class WriteResult : uint8_t { Done, NotFound, NoMemory, Failed };

  // A finished session: creates the book's entry when the session has seconds (a book opened and
  // closed without reading is not history), evicting the least recently read book past kMaxBooks;
  // a zero-second session still moves an existing book's progress.
  WriteResult recordSession(const std::string& docId, const std::string& title, const std::string& author,
                            uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress,
                            time_t walltimeEpoch);

  // One more finish; creates the entry when needed, under the same cap.
  WriteResult markFinished(const std::string& docId, const std::string& title, const std::string& author,
                           time_t walltimeEpoch);

  // Forget one book: its entry goes and its time, sessions, pages and day buckets come back out of
  // the totals, so the stats read as if it had never been read. Exact for everything the screens
  // show — a book keeps its newest kMaxBookDays buckets, which always cover the 30-day sparkline
  // and the current streak. The persisted longest-streak record stays.
  WriteResult removeBook(const std::string& docId);

  // ---- Streamed queries -------------------------------------------------------------------------
  //
  // Each reads the file once and returns only what was asked for; nothing stays resident. An absent
  // or empty file is an empty history (Ok). Call from the loop task, never from render().
  struct BookQuery {
    bool found = false;
    BookReadingStats book;
    float pooledPace = 0.0f;  // the global pace, for a book without one of its own
  };
  ReadingStatsFile::ScanResult querySummary(ReadingStatsFile::Summary& out, bool withIndex = false) const;
  ReadingStatsFile::ScanResult queryBook(const std::string& docId, BookQuery& out) const;
  ReadingStatsFile::ScanResult queryBookAt(uint32_t offset, BookReadingStats& book) const;

  // ---- Recent-books cache -----------------------------------------------------------------------
  //
  // What Home draws for its recent books, so the themes read no file from render(). Home calls
  // prefetchRecent() on entry; it scans only for books not cached yet (books without history are
  // cached as unknown). Bounded: the books asked for stay, others go past kRecentCacheSize.
  static constexpr size_t kRecentCacheSize = 12;
  void prefetchRecent(const std::vector<std::string>& docIds);
  const ReadingStatsFile::RecentSnapshot* recent(const std::string& docId) const;
  float recentPooledPace() const { return pooledPace_; }

  // ---- Reading speed / time-to-finish -----------------------------------------------------------
  //
  // Average seconds spent reading per 1% of book progress: a book's own once it has covered enough
  // ground to be meaningful, else the global average over the books that have. The minimum-progress
  // gate keeps 30 seconds at 2% from extrapolating to a 25-minute book.
  static constexpr uint8_t MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE = 3;  // %
  static constexpr uint32_t MIN_GLOBAL_SECONDS_FOR_RATE = 60;        // s

  // ---- The arithmetic, on plain data ------------------------------------------------------------
  //
  // Shared by the streamed writes, the screens and ReadingStatsFile, so the device screens and the
  // web dashboard agree on every figure.
  static uint32_t secondsOn(const std::vector<DayBucket>& days, uint16_t dayIndex);
  static uint16_t currentStreakIn(const std::vector<DayBucket>& days, uint16_t today);
  // The longest run in `days`, or the persisted `record` when that is longer.
  static uint16_t longestStreakIn(const std::vector<DayBucket>& days, uint16_t record);
  static bool countsTowardPace(const uint8_t progress) { return progress >= MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE; }
  // The cap's eviction order: the least recently read book goes first (an entry never read with
  // the clock set, lastReadEpoch 0, is the oldest); on the same date, the one with less time.
  static bool evictsBefore(time_t aLastRead, uint32_t aSeconds, time_t bLastRead, uint32_t bSeconds);
  // The global pace from the sums over the books that count toward it; 0 below
  // MIN_GLOBAL_SECONDS_FOR_RATE of reading overall.
  static float pooledSecondsPerPercent(uint32_t globalTotalSeconds, uint32_t countedSeconds, uint32_t countedPercents);
  // A book's own pace, or 0 when it has not covered enough ground to have one.
  static float ownSecondsPerPercent(uint32_t totalSeconds, uint8_t progress);
  static uint32_t etaSeconds(float secondsPerPercent, float remainingPercent);
  // Past the caps, the oldest buckets go; the global trim first folds the streak they held into
  // `record`, since the record may live in them.
  static void trimGlobalDays(std::vector<DayBucket>& days, uint16_t& record);
  static void trimBookDays(std::vector<DayBucket>& days);
  // Takes one book's contribution back out of the global figures (removal's arithmetic).
  static void takeOut(const BookReadingStats& book, ReadingTotals& totals);
  // A finished session applied to one book and the global figures (the session end's arithmetic):
  // counters, the book's and the global day buckets, the streak record. `title` / `author` replace
  // the book's only when non-empty.
  static void applySession(BookReadingStats& book, ReadingTotals& totals, const std::string& title,
                           const std::string& author, uint32_t sessionSeconds, uint32_t sessionPagesTurned,
                           uint8_t progress, time_t walltimeEpoch);
  // One more finish of the book.
  static void applyFinish(BookReadingStats& book, const std::string& title, const std::string& author,
                          time_t walltimeEpoch);

  // Bounds (memory audit 2026-09, R8). Past these caps the least recently read book goes, and the
  // oldest day buckets go; the sparkline needs 30 days and the streak walk needs the current run,
  // both well inside the global window, and the longest streak is kept as a number.
  static constexpr size_t kMaxBooks = 100;
  static constexpr size_t kMaxBookDays = 60;
  static constexpr size_t kMaxGlobalDays = 400;

 private:
  enum class Edit : uint8_t { Session, Finish, Remove };
  // The streamed update shared by the three writes. `apply` changes the book and the totals and
  // returns false when there is nothing to write.
  WriteResult write(Edit edit, const std::string& docId,
                    const std::function<bool(BookReadingStats& book, ReadingTotals& totals, bool existed)>& apply);
  void rememberRecent(const BookReadingStats& book);
  void forgetRecent(const std::string& docId);
  ReadingStatsFile::ScanResult scanFile(const ReadingStatsFile::ScanRequest& request,
                                        ReadingStatsFile::Summary& summary) const;

  std::string path_;
  std::vector<ReadingStatsFile::RecentSnapshot> recent_;
  float pooledPace_ = 0.0f;
  bool paceKnown_ = false;
};

#define READING_STATS ReadingStatsStore::getInstance()
