#include "ReadingStats.h"

#include <Arduino.h>  // millis()
#include <BufferedPrint.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <ctime>

#include "ReadingStatsFile.h"

namespace {
// Puts a finished temporary file in place of the store's file.
bool swapIn(const std::string& path, const std::string& tmpPath) {
  Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("RST", "Could not rename %s into place", tmpPath.c_str());
    return false;
  }
  return true;
}

// Where a history that cannot be read is set aside: reading-stats.json -> reading-stats.corrupt.json.
std::string asidePathFor(const std::string& path) {
  constexpr char kSuffix[] = ".json";
  const size_t n = sizeof(kSuffix) - 1;
  if (path.size() > n && path.compare(path.size() - n, n, kSuffix) == 0) {
    return path.substr(0, path.size() - n) + ".corrupt.json";
  }
  return path + ".corrupt";
}

// A swap interrupted between removing the old file and renaming the verified new one into place
// (power lost at the session end) leaves only the temporary file, and it holds the whole history.
// Put it back before anything reads the history, or the next write would start an empty one over
// it. A temporary file that does not read back is a write cut short, never taken for the history.
void recoverInterruptedSwap(const std::string& path) {
  const std::string tmpPath = path + ".tmp";
  if (Storage.exists(path.c_str()) || !Storage.exists(tmpPath.c_str())) return;
  {
    FsFile in;
    if (!Storage.openFileForRead("RST", tmpPath.c_str(), in)) return;
    ReadingStatsFile::Summary check;
    if (ReadingStatsFile::scan(in, check, ReadingStatsFile::ScanRequest{}) != ReadingStatsFile::ScanResult::Ok) return;
  }
  if (Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("RST", "Recovered the history from %s after an interrupted swap", tmpPath.c_str());
  }
}

// Diagnostics for the "write done" line: time, calls and bytes that reached the card.
class TimedPrint final : public Print {
 public:
  explicit TimedPrint(Print& out) : out_(out) {}
  size_t write(const uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, const size_t n) override {
    const unsigned long t = millis();
    const size_t done = out_.write(data, n);
    ms += millis() - t;
    ++calls;
    bytes += n;
    return done;
  }
  unsigned long ms = 0;
  uint32_t calls = 0;
  size_t bytes = 0;

 private:
  Print& out_;
};

std::string parentDirOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

// Add `seconds` to the bucket for `dayIndex` in `days`, inserting in sorted
// position if absent. dayIndex == 0 ("unknown day") is silently skipped here —
// the caller decides whether to credit unknown-day reading to a sentinel
// bucket or drop it entirely.
void mergeDay(std::vector<DayBucket>& days, uint16_t dayIndex, uint32_t seconds, const size_t maxDays) {
  if (dayIndex == 0 || seconds == 0) return;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) {
    it->seconds += seconds;
  } else {
    days.insert(it, {dayIndex, seconds});
  }
  // Sorted ascending, so the oldest buckets are at the front.
  if (days.size() > maxDays) days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - maxDays));
}

// Take `seconds` back out of the bucket for `dayIndex`, dropping the bucket once it is empty: an
// empty bucket would still count as a reading day in the longest-streak walk.
void unmergeDay(std::vector<DayBucket>& days, const uint16_t dayIndex, const uint32_t seconds) {
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it == days.end() || it->dayIndex != dayIndex) return;
  if (it->seconds > seconds) {
    it->seconds -= seconds;
  } else {
    days.erase(it);
  }
}

// Length of the run of consecutive reading days that ends on `day`, from a sorted day map.
uint16_t runEndingAt(const std::vector<DayBucket>& days, const uint16_t day) {
  auto it =
      std::lower_bound(days.begin(), days.end(), day, [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it == days.end() || it->dayIndex != day) return 0;
  uint16_t run = 1;
  while (it != days.begin()) {
    const auto prev = it - 1;
    if (prev->dayIndex + 1 != it->dayIndex) break;
    ++run;
    it = prev;
  }
  return run;
}

uint16_t dayIndexFromLocaltime(const struct tm& t) {
  // Days since 1970-01-01 by Y/M/D in local time. Uses the proleptic
  // Gregorian calendar — close enough for a 65k-day uint16 range (≈179
  // years). We deliberately do NOT call mktime() to avoid DST round-trip
  // surprises near transition midnights.
  const int year = t.tm_year + 1900;
  const int month = t.tm_mon + 1;
  const int day = t.tm_mday;
  // Howard Hinnant's days-from-civil, lightly inlined.
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + static_cast<long>(doe) - 719468L;
  if (days < 1 || days > 65535) return 0;  // outside our uint16_t window
  return static_cast<uint16_t>(days);
}

}  // namespace

uint16_t localDayIndexFromEpoch(time_t epoch) {
  if (epoch == 0) return 0;
  struct tm t{};
  localtime_r(&epoch, &t);
  return dayIndexFromLocaltime(t);
}

uint16_t currentLocalDayIndex() {
  if (!HalClock::isSynced()) return 0;
  return localDayIndexFromEpoch(HalClock::now());
}

ReadingStatsStore ReadingStatsStore::instance;

// ---- The arithmetic ---------------------------------------------------------------------------

uint32_t ReadingStatsStore::secondsOn(const std::vector<DayBucket>& days, const uint16_t dayIndex) {
  if (dayIndex == 0) return 0;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) return it->seconds;
  return 0;
}

uint16_t ReadingStatsStore::currentStreakIn(const std::vector<DayBucket>& days, const uint16_t today) {
  if (today == 0 || days.empty()) return 0;
  // 1-day grace: if there's no reading today, the streak may still end at
  // yesterday. After that the chain is broken.
  uint16_t anchor = today;
  if (secondsOn(days, anchor) == 0) {
    anchor -= 1;
    if (secondsOn(days, anchor) == 0) return 0;
  }
  uint16_t streak = 0;
  while (anchor > 0 && secondsOn(days, anchor) > 0) {
    streak += 1;
    if (anchor == 1) break;
    anchor -= 1;
  }
  return streak;
}

uint16_t ReadingStatsStore::longestStreakIn(const std::vector<DayBucket>& days, const uint16_t record) {
  if (days.empty()) return record;
  uint16_t longest = std::max<uint16_t>(1, record);
  uint16_t run = 1;
  for (size_t i = 1; i < days.size(); ++i) {
    if (days[i].dayIndex == days[i - 1].dayIndex + 1) {
      run += 1;
      if (run > longest) longest = run;
    } else {
      run = 1;
    }
  }
  return longest;
}

bool ReadingStatsStore::evictsBefore(const time_t aLastRead, const uint32_t aSeconds, const time_t bLastRead,
                                     const uint32_t bSeconds) {
  if (aLastRead != bLastRead) return aLastRead < bLastRead;
  return aSeconds < bSeconds;
}

float ReadingStatsStore::pooledSecondsPerPercent(const uint32_t globalTotalSeconds, const uint32_t countedSeconds,
                                                 const uint32_t countedPercents) {
  if (globalTotalSeconds < MIN_GLOBAL_SECONDS_FOR_RATE) return 0.0f;
  if (countedPercents == 0 || countedSeconds == 0) return 0.0f;
  return static_cast<float>(countedSeconds) / static_cast<float>(countedPercents);
}

float ReadingStatsStore::ownSecondsPerPercent(const uint32_t totalSeconds, const uint8_t progress) {
  if (!countsTowardPace(progress) || totalSeconds == 0) return 0.0f;
  return static_cast<float>(totalSeconds) / static_cast<float>(progress);
}

uint32_t ReadingStatsStore::etaSeconds(const float secondsPerPercent, float remainingPercent) {
  if (remainingPercent <= 0.0f) return 0;
  if (remainingPercent > 100.0f) remainingPercent = 100.0f;
  if (secondsPerPercent <= 0.0f) return 0;
  return static_cast<uint32_t>(remainingPercent * secondsPerPercent + 0.5f);
}

void ReadingStatsStore::trimBookDays(std::vector<DayBucket>& days) {
  if (days.size() > kMaxBookDays)
    days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - kMaxBookDays));
}

void ReadingStatsStore::trimGlobalDays(std::vector<DayBucket>& days, uint16_t& record) {
  if (days.size() <= kMaxGlobalDays) return;
  // The record streak may live in the buckets about to go: measure before trimming.
  record = longestStreakIn(days, record);
  days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - kMaxGlobalDays));
}

void ReadingStatsStore::takeOut(const BookReadingStats& book, ReadingTotals& totals) {
  totals.totalSeconds -= std::min(totals.totalSeconds, book.totalSeconds);
  totals.totalSessions -= std::min(totals.totalSessions, book.sessions);
  totals.totalPagesTurned -= std::min(totals.totalPagesTurned, book.pagesTurned);
  for (const DayBucket& day : book.days) unmergeDay(totals.globalDays, day.dayIndex, day.seconds);
}

void ReadingStatsStore::applySession(BookReadingStats& book, ReadingTotals& totals, const std::string& title,
                                     const std::string& author, const uint32_t sessionSeconds,
                                     const uint32_t sessionPagesTurned, const uint8_t progress,
                                     const time_t walltimeEpoch) {
  if (!title.empty()) book.title = title;
  if (!author.empty()) book.author = author;
  book.totalSeconds += sessionSeconds;
  book.pagesTurned += sessionPagesTurned;
  book.progress = progress;
  if (sessionSeconds > 0) {
    book.sessions += 1;
    totals.totalSessions += 1;
  }
  if (walltimeEpoch != 0) {
    if (book.firstReadEpoch == 0) book.firstReadEpoch = walltimeEpoch;
    book.lastReadEpoch = walltimeEpoch;
    const uint16_t day = localDayIndexFromEpoch(walltimeEpoch);
    mergeDay(book.days, day, sessionSeconds, kMaxBookDays);
    mergeDay(totals.globalDays, day, sessionSeconds, kMaxGlobalDays);
    // Fold this day's run into the persisted longest streak while the whole run is still in the
    // window (a run longer than the window is already the record).
    const uint16_t run = runEndingAt(totals.globalDays, day);
    if (run > totals.longestStreak) totals.longestStreak = run;
  }
  totals.totalSeconds += sessionSeconds;
  totals.totalPagesTurned += sessionPagesTurned;
}

void ReadingStatsStore::applyFinish(BookReadingStats& book, const std::string& title, const std::string& author,
                                    const time_t walltimeEpoch) {
  if (!title.empty()) book.title = title;
  if (!author.empty()) book.author = author;
  book.finishedCount += 1;
  book.progress = 100;
  if (walltimeEpoch != 0) {
    book.lastFinishedEpoch = walltimeEpoch;
    if (book.lastReadEpoch < walltimeEpoch) book.lastReadEpoch = walltimeEpoch;
  }
}

// ---- Updates ----------------------------------------------------------------------------------

ReadingStatsStore::WriteResult ReadingStatsStore::recordSession(const std::string& docId, const std::string& title,
                                                                const std::string& author,
                                                                const uint32_t sessionSeconds,
                                                                const uint32_t sessionPagesTurned,
                                                                const uint8_t progress, const time_t walltimeEpoch) {
  return write(Edit::Session, docId, [&](BookReadingStats& book, ReadingTotals& totals, const bool existed) {
    if (!existed && sessionSeconds == 0) return false;  // nothing to record
    applySession(book, totals, title, author, sessionSeconds, sessionPagesTurned, progress, walltimeEpoch);
    return true;
  });
}

ReadingStatsStore::WriteResult ReadingStatsStore::markFinished(const std::string& docId, const std::string& title,
                                                               const std::string& author, const time_t walltimeEpoch) {
  return write(Edit::Finish, docId, [&](BookReadingStats& book, ReadingTotals&, bool) {
    applyFinish(book, title, author, walltimeEpoch);
    return true;
  });
}

ReadingStatsStore::WriteResult ReadingStatsStore::removeBook(const std::string& docId) {
  return write(Edit::Remove, docId, [](BookReadingStats& book, ReadingTotals& totals, bool) {
    takeOut(book, totals);
    return true;
  });
}

ReadingStatsStore::WriteResult ReadingStatsStore::write(
    const Edit edit, const std::string& docId,
    const std::function<bool(BookReadingStats&, ReadingTotals&, bool)>& apply) {
  if (docId.empty()) return WriteResult::NotFound;
  [[maybe_unused]] const uint32_t started = millis();

  // 1. Scan: the header, the book, and the cap's victim should the book be new.
  ReadingStatsFile::ScanRequest request;
  request.findDocId = docId;
  request.wantVictim = edit != Edit::Remove;
  ReadingStatsFile::Summary summary;
  const auto result = scanFile(request, summary);
  // After the scan: it may have recovered the file from an interrupted swap.
  bool haveInput = Storage.exists(path_.c_str());
  if (result == ReadingStatsFile::ScanResult::NoMemory) return WriteResult::NoMemory;
  if (result == ReadingStatsFile::ScanResult::IoError) return WriteResult::Failed;
  // A removal has nothing to remove from an unreadable file; only a write that adds reading starts
  // a fresh history over it.
  if (result == ReadingStatsFile::ScanResult::Malformed && edit == Edit::Remove) return WriteResult::Failed;
  if (result == ReadingStatsFile::ScanResult::Malformed) {
    // Permanent: set it aside for forensics rather than lose it or stall on it for ever, and start
    // a fresh history with this change.
    const std::string aside = asidePathFor(path_);
    Storage.remove(aside.c_str());
    if (!Storage.rename(path_.c_str(), aside.c_str())) {
      LOG_ERR("RST", "History file unreadable and could not be set aside; left as is");
      return WriteResult::Failed;
    }
    LOG_ERR("RST", "History file unreadable; set aside as %s, starting a fresh history", aside.c_str());
    summary = ReadingStatsFile::Summary{};
    haveInput = false;
  }
  if (!summary.found && edit == Edit::Remove) return WriteResult::NotFound;
  [[maybe_unused]] const uint32_t scanned = millis();

  // 2. Merge the one book in memory.
  BookReadingStats book = summary.found ? summary.target : BookReadingStats{};
  if (!summary.found) book.docId = docId;
  ReadingStatsFile::Rewrite rewrite;
  rewrite.totals = summary;  // the ReadingTotals part
  if (!apply(book, rewrite.totals, summary.found)) return WriteResult::Done;
  uint32_t expectedBooks = summary.bookCount;
  std::string evicted;
  if (edit == Edit::Remove) {
    rewrite.dropAt = summary.targetFirst;
    --expectedBooks;
  } else if (summary.found) {
    rewrite.replaceAt = summary.targetFirst;
    rewrite.replacement = &book;
  } else {
    rewrite.append = &book;
    ++expectedBooks;
    if (summary.bookCount >= kMaxBooks && summary.hasVictim) {
      rewrite.dropAt = summary.victimFirst;
      evicted = summary.victimDocId;
      --expectedBooks;
      LOG_INF("RST", "Book cap (%u) reached; dropping the least recently read: %s", static_cast<unsigned>(kMaxBooks),
              evicted.c_str());
    }
  }

  // 3. Write the temporary file.
  Storage.mkdir(parentDirOf(path_).c_str());
  const std::string tmpPath = path_ + ".tmp";
  bool written = false;
  [[maybe_unused]] unsigned long sdMs = 0;
  [[maybe_unused]] uint32_t sdCalls = 0;
  [[maybe_unused]] size_t sdBytes = 0;
  {
    FsFile in;
    const bool inOpen = haveInput && Storage.openFileForRead("RST", path_.c_str(), in) && in.size() > 0;
    FsFile out;
    if (!Storage.openFileForWrite("RST", tmpPath.c_str(), out)) return WriteResult::Failed;
    // The copy goes out a byte at a time; batch it into few SD calls.
    TimedPrint timed(out);
    BufferedPrint buffered(timed, 1024);
    const auto copied = ReadingStatsFile::writeRewrite(inOpen ? &in : nullptr, rewrite, buffered);
    written = buffered.flushBuffer() && copied == ReadingStatsFile::ScanResult::Ok;
    sdMs = timed.ms;
    sdCalls = timed.calls;
    sdBytes = timed.bytes;
  }
  [[maybe_unused]] const uint32_t copiedAt = millis();

  // 4. Read it back; swap it in only if it holds what it should.
  ReadingStatsFile::Summary check;
  if (written) {
    ReadingStatsFile::ScanRequest verify;
    verify.findDocId = docId;
    FsFile back;
    written = Storage.openFileForRead("RST", tmpPath.c_str(), back) &&
              ReadingStatsFile::scan(back, check, verify) == ReadingStatsFile::ScanResult::Ok &&
              check.bookCount == expectedBooks &&
              (edit == Edit::Remove ? !check.found : (check.found && check.target.totalSeconds == book.totalSeconds));
  }
  [[maybe_unused]] const uint32_t verified = millis();
  if (!written) {
    LOG_ERR("RST", "Rewritten history did not read back; left as is");
    Storage.remove(tmpPath.c_str());
    return WriteResult::Failed;
  }
  if (!swapIn(path_, tmpPath)) return WriteResult::Failed;

  // The cache follows the file without a scan of its own.
  if (edit == Edit::Remove) {
    forgetRecent(docId);
  } else {
    rememberRecent(book);
  }
  if (!evicted.empty()) forgetRecent(evicted);
  pooledPace_ = pooledSecondsPerPercent(check.totalSeconds, check.paceSeconds, check.pacePercents);
  paceKnown_ = true;
  [[maybe_unused]] const uint32_t done = millis();
  LOG_INF("RST",
          "write done in %lu ms: scan %lu, copy %lu (sd %lu ms in %u writes, %u B), verify %lu, swap %lu "
          "(%u books, free=%lu contig=%lu)",
          static_cast<unsigned long>(done - started), static_cast<unsigned long>(scanned - started),
          static_cast<unsigned long>(copiedAt - scanned), sdMs, static_cast<unsigned>(sdCalls),
          static_cast<unsigned>(sdBytes), static_cast<unsigned long>(verified - copiedAt),
          static_cast<unsigned long>(done - verified), static_cast<unsigned>(expectedBooks),
          static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  return WriteResult::Done;
}

void ReadingStatsStore::rememberRecent(const BookReadingStats& book) {
  ReadingStatsFile::RecentSnapshot snapshot;
  snapshot.docId = book.docId;
  snapshot.known = true;
  snapshot.totalSeconds = book.totalSeconds;
  snapshot.knownDays = static_cast<uint16_t>(std::min(book.days.size(), kMaxBookDays));
  snapshot.lastReadEpoch = book.lastReadEpoch;
  snapshot.progress = book.progress;
  forgetRecent(book.docId);
  // The book just read is the likeliest one on Home: it goes to the front, the oldest entry goes.
  recent_.insert(recent_.begin(), std::move(snapshot));
  if (recent_.size() > kRecentCacheSize) recent_.pop_back();
}

void ReadingStatsStore::forgetRecent(const std::string& docId) {
  recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                               [&docId](const ReadingStatsFile::RecentSnapshot& s) { return s.docId == docId; }),
                recent_.end());
}

// ---- Queries ----------------------------------------------------------------------------------

ReadingStatsFile::ScanResult ReadingStatsStore::scanFile(const ReadingStatsFile::ScanRequest& request,
                                                         ReadingStatsFile::Summary& summary) const {
  summary = ReadingStatsFile::Summary{};
  recoverInterruptedSwap(path_);
  const auto emptyHistory = [&]() {
    for (const auto& id : request.recentDocIds) {
      ReadingStatsFile::RecentSnapshot unknown;
      unknown.docId = id;
      summary.recents.push_back(std::move(unknown));
    }
    return ReadingStatsFile::ScanResult::Ok;
  };
  if (!Storage.exists(path_.c_str())) return emptyHistory();
  FsFile in;
  if (!Storage.openFileForRead("RST", path_.c_str(), in)) return ReadingStatsFile::ScanResult::IoError;
  if (in.size() == 0) return emptyHistory();
  return ReadingStatsFile::scan(in, summary, request);
}

ReadingStatsFile::ScanResult ReadingStatsStore::querySummary(ReadingStatsFile::Summary& out,
                                                             const bool withIndex) const {
  ReadingStatsFile::ScanRequest request;
  request.wantIndex = withIndex;
  return scanFile(request, out);
}

ReadingStatsFile::ScanResult ReadingStatsStore::queryBook(const std::string& docId, BookQuery& out) const {
  out = BookQuery{};
  ReadingStatsFile::ScanRequest request;
  request.findDocId = docId;
  ReadingStatsFile::Summary summary;
  const auto result = scanFile(request, summary);
  if (result != ReadingStatsFile::ScanResult::Ok) return result;
  out.found = summary.found;
  out.book = std::move(summary.target);
  out.pooledPace = pooledSecondsPerPercent(summary.totalSeconds, summary.paceSeconds, summary.pacePercents);
  return result;
}

ReadingStatsFile::ScanResult ReadingStatsStore::queryBookAt(const uint32_t offset, BookReadingStats& book) const {
  FsFile in;
  if (!Storage.openFileForRead("RST", path_.c_str(), in)) return ReadingStatsFile::ScanResult::IoError;
  return ReadingStatsFile::readBookAt(in, offset, book);
}

void ReadingStatsStore::prefetchRecent(const std::vector<std::string>& docIds) {
  ReadingStatsFile::ScanRequest request;
  for (const auto& id : docIds) {
    if (!id.empty() && recent(id) == nullptr) request.recentDocIds.push_back(id);
  }
  if (request.recentDocIds.empty() && paceKnown_) return;
  ReadingStatsFile::Summary summary;
  const auto result = scanFile(request, summary);
  if (result != ReadingStatsFile::ScanResult::Ok) {
    LOG_ERR("RST", "prefetchRecent: scan failed (%u); Home draws no history this time", static_cast<unsigned>(result));
    return;
  }
  for (auto& snapshot : summary.recents) recent_.push_back(std::move(snapshot));
  pooledPace_ = pooledSecondsPerPercent(summary.totalSeconds, summary.paceSeconds, summary.pacePercents);
  paceKnown_ = true;
  for (auto it = recent_.begin(); recent_.size() > kRecentCacheSize && it != recent_.end();) {
    if (std::find(docIds.begin(), docIds.end(), it->docId) == docIds.end()) {
      it = recent_.erase(it);
    } else {
      ++it;
    }
  }
}

const ReadingStatsFile::RecentSnapshot* ReadingStatsStore::recent(const std::string& docId) const {
  for (const auto& snapshot : recent_) {
    if (snapshot.docId == docId) return &snapshot;
  }
  return nullptr;
}
