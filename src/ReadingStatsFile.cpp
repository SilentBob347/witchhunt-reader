#include "ReadingStatsFile.h"

#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ReadingStatsFile {
namespace {

constexpr size_t kNone = std::numeric_limits<size_t>::max();
constexpr size_t kReadBlock = 1024;

void emit(Print& out, const char* text, const size_t len) { out.write(reinterpret_cast<const uint8_t*>(text), len); }

void emit(Print& out, const char* text) { emit(out, text, strlen(text)); }

void emitDays(Print& out, const std::vector<DayBucket>& days) {
  emit(out, "[");
  char pair[32];
  for (size_t i = 0; i < days.size(); ++i) {
    const int n = snprintf(pair, sizeof(pair), "%s[%u,%lu]", i == 0 ? "" : ",", days[i].dayIndex,
                           static_cast<unsigned long>(days[i].seconds));
    emit(out, pair, static_cast<size_t>(n));
  }
  emit(out, "]");
}

uint32_t toCount(const char* text) {
  const long long v = strtoll(text, nullptr, 10);
  if (v < 0) return 0;
  if (v > static_cast<long long>(std::numeric_limits<uint32_t>::max())) return std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(v);
}

// Walks the file with the SAX parser, keeping track of where it is in the stats layout, and hands
// the subclasses what they need: the top-level counters, the global day buckets, and each book's
// counters and days together with the offsets of its braces. Fed a byte at a time so that every
// event knows the offset of the byte that caused it, and onByte() sees each byte after the parser
// has.
//
//   { "totalSeconds": n, ..., "globalDays": [[d, s], ...], "books": [ { ..., "days": [[d, s]] } ] }
//   depth 1                   2            3              2          3            4  5
class Scanner {
 public:
  struct Book {
    std::string docId;
    uint32_t totalSeconds = 0;
    uint32_t pagesTurned = 0;
    uint32_t sessions = 0;
    uint8_t progress = 0;
    uint16_t finishedCount = 0;
    bool hasFinishedCount = false;
    bool finishedFlag = false;  // the legacy bool, read only when finishedCount is absent
    std::vector<DayBucket> days;
    size_t first = 0;  // offset of '{'
    bool hasFields = false;
  };

  virtual ~Scanner() = default;

  // False when the parser cannot be allocated, the file cannot be read, or it is not a well-formed
  // stats file: a single JSON object that closes.
  bool run(HalFile& in) {
    auto parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks());
    // Large reads: every file call costs ~1.5 ms on SD whatever its size (BufferedFileIO.h), and a
    // full history is ~100 KB.
    auto block = makeUniqueNoThrow<char[]>(kReadBlock);
    if (!parser || !block) {
      LOG_ERR("RSF", "OOM: stats file parser");
      return false;
    }
    if (!in.seekSet(0)) return false;
    char* const bytes = block.get();
    size_t offset = 0;
    for (;;) {
      const int n = in.read(bytes, kReadBlock);
      if (n < 0) return false;
      if (n == 0) break;
      for (int i = 0; i < n; ++i, ++offset) {
        offset_ = offset;
        parser->feed(&bytes[i], 1);
        onByte(bytes[i], offset, insideBooks(offset));
      }
    }
    return !parser->hasError() && rootClosed_ && depth_ == 0;
  }

 protected:
  enum class Top : uint8_t { Other, TotalSeconds, TotalSessions, TotalPagesTurned, LongestStreak, GlobalDays, Books };

  virtual void onCounter(Top, uint32_t) {}
  virtual void onGlobalDay(uint16_t, uint32_t) {}
  virtual void onBookStart(size_t) {}
  // While the parser takes in the book's closing brace, before onByte() sees it.
  virtual void onBookEnd(const Book&, size_t) {}
  // `inBooks`: strictly between the brackets of the books array.
  virtual void onByte(char, size_t, bool) {}

 private:
  enum class Field : uint8_t {
    Other,
    DocId,
    TotalSeconds,
    PagesTurned,
    Sessions,
    Progress,
    FinishedCount,
    Finished,
    Days
  };

  static Top topFor(const char* key) {
    if (strcmp(key, "totalSeconds") == 0) return Top::TotalSeconds;
    if (strcmp(key, "totalSessions") == 0) return Top::TotalSessions;
    if (strcmp(key, "totalPagesTurned") == 0) return Top::TotalPagesTurned;
    if (strcmp(key, "longestStreak") == 0) return Top::LongestStreak;
    if (strcmp(key, "globalDays") == 0) return Top::GlobalDays;
    if (strcmp(key, "books") == 0) return Top::Books;
    return Top::Other;
  }

  static Field fieldFor(const char* key) {
    if (strcmp(key, "docId") == 0) return Field::DocId;
    if (strcmp(key, "totalSeconds") == 0) return Field::TotalSeconds;
    if (strcmp(key, "pagesTurned") == 0) return Field::PagesTurned;
    if (strcmp(key, "sessions") == 0) return Field::Sessions;
    if (strcmp(key, "progress") == 0) return Field::Progress;
    if (strcmp(key, "finishedCount") == 0) return Field::FinishedCount;
    if (strcmp(key, "finished") == 0) return Field::Finished;
    if (strcmp(key, "days") == 0) return Field::Days;
    return Field::Other;
  }

  bool inBooksArray() const { return top_ == Top::Books; }
  bool atBook() const { return inBooksArray() && depth_ == 3; }
  bool atDayPair() const {
    return (top_ == Top::GlobalDays && depth_ == 3) || (inBooksArray() && field_ == Field::Days && depth_ == 5);
  }

  bool insideBooks(const size_t offset) const {
    return booksOpen_ != kNone && offset > booksOpen_ && (booksClose_ == kNone || offset < booksClose_);
  }

  void objectStart() {
    ++depth_;
    if (depth_ == 1 && !rootSeen_) rootSeen_ = true;
    if (atBook()) {
      book_.docId.clear();
      book_.totalSeconds = book_.pagesTurned = book_.sessions = 0;
      book_.progress = 0;
      book_.finishedCount = 0;
      book_.hasFinishedCount = book_.finishedFlag = book_.hasFields = false;
      book_.days.clear();
      book_.first = offset_;
      field_ = Field::Other;
      onBookStart(offset_);
    }
  }

  void objectEnd() {
    if (atBook()) onBookEnd(book_, offset_);
    if (depth_ == 1 && rootSeen_) rootClosed_ = true;
    if (depth_ > 0) --depth_;
  }

  void arrayStart() {
    ++depth_;
    if (depth_ == 2 && inBooksArray() && booksOpen_ == kNone) booksOpen_ = offset_;
    if (atDayPair()) {
      pairIndex_ = 0;
      pairDay_ = 0;
      pairSeconds_ = 0;
    }
  }

  void arrayEnd() {
    if (atDayPair() && pairIndex_ >= 2 && pairDay_ != 0 && pairSeconds_ != 0) {
      // Same filter as the loader: a zero day or zero seconds is not a bucket.
      if (top_ == Top::GlobalDays) {
        onGlobalDay(pairDay_, pairSeconds_);
      } else {
        book_.days.push_back({pairDay_, pairSeconds_});
      }
    }
    if (depth_ == 2 && inBooksArray() && booksClose_ == kNone) booksClose_ = offset_;
    if (depth_ > 0) --depth_;
  }

  void key(const char* text) {
    if (depth_ == 1) {
      top_ = topFor(text);
    } else if (atBook()) {
      field_ = fieldFor(text);
      book_.hasFields = true;
    }
  }

  void number(const char* text) {
    if (depth_ == 1) {
      onCounter(top_, toCount(text));
    } else if (atDayPair()) {
      if (pairIndex_ == 0) pairDay_ = static_cast<uint16_t>(std::min<uint32_t>(toCount(text), 0xFFFF));
      if (pairIndex_ == 1) pairSeconds_ = toCount(text);
      ++pairIndex_;
    } else if (atBook()) {
      const uint32_t v = toCount(text);
      switch (field_) {
        case Field::TotalSeconds:
          book_.totalSeconds = v;
          break;
        case Field::PagesTurned:
          book_.pagesTurned = v;
          break;
        case Field::Sessions:
          book_.sessions = v;
          break;
        case Field::Progress:
          book_.progress = static_cast<uint8_t>(std::min<uint32_t>(v, 0xFF));
          break;
        case Field::FinishedCount:
          book_.finishedCount = static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFF));
          book_.hasFinishedCount = true;
          break;
        default:
          break;
      }
    }
  }

  void string(const char* text) {
    if (atBook() && field_ == Field::DocId) book_.docId = text;
  }

  void boolean(const bool value) {
    if (atBook() && field_ == Field::Finished) book_.finishedFlag = value;
  }

  JsonCallbacks callbacks() {
    JsonCallbacks cb{};
    cb.ctx = this;
    cb.onKey = [](void* ctx, const char* v, size_t) { static_cast<Scanner*>(ctx)->key(v); };
    cb.onString = [](void* ctx, const char* v, size_t) { static_cast<Scanner*>(ctx)->string(v); };
    cb.onNumber = [](void* ctx, const char* v, size_t) { static_cast<Scanner*>(ctx)->number(v); };
    cb.onBool = [](void* ctx, bool v) { static_cast<Scanner*>(ctx)->boolean(v); };
    cb.onObjectStart = [](void* ctx) { static_cast<Scanner*>(ctx)->objectStart(); };
    cb.onObjectEnd = [](void* ctx) { static_cast<Scanner*>(ctx)->objectEnd(); };
    cb.onArrayStart = [](void* ctx) { static_cast<Scanner*>(ctx)->arrayStart(); };
    cb.onArrayEnd = [](void* ctx) { static_cast<Scanner*>(ctx)->arrayEnd(); };
    return cb;
  }

  size_t offset_ = 0;
  int depth_ = 0;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  Top top_ = Top::Other;
  Field field_ = Field::Other;
  size_t booksOpen_ = kNone;
  size_t booksClose_ = kNone;
  uint8_t pairIndex_ = 0;
  uint16_t pairDay_ = 0;
  uint32_t pairSeconds_ = 0;
  Book book_;
};

class SummaryScan final : public Scanner {
 public:
  SummaryScan(Summary& summary, const std::string& findDocId) : summary_(summary), findDocId_(findDocId) {}

  // The byte range that removes the target, taking one separating comma with it: the one before it
  // when it has a predecessor, else the one after it.
  void settleCut() {
    if (!summary_.found) return;
    if (prevLast_ != kNone) {
      summary_.cutFirst = prevLast_ + 1;
      summary_.cutLast = targetLast_;
    } else if (nextFirst_ != kNone) {
      summary_.cutFirst = targetFirst_;
      summary_.cutLast = nextFirst_ - 1;
    } else {
      summary_.cutFirst = targetFirst_;
      summary_.cutLast = targetLast_;
    }
  }

 private:
  void onCounter(const Top top, const uint32_t value) override {
    switch (top) {
      case Top::TotalSeconds:
        summary_.totalSeconds = value;
        break;
      case Top::TotalSessions:
        summary_.totalSessions = value;
        break;
      case Top::TotalPagesTurned:
        summary_.totalPagesTurned = value;
        break;
      case Top::LongestStreak:
        summary_.longestStreak = static_cast<uint16_t>(std::min<uint32_t>(value, 0xFFFF));
        break;
      default:
        break;
    }
  }

  void onGlobalDay(const uint16_t day, const uint32_t seconds) override {
    summary_.globalDays.push_back({day, seconds});
  }

  void onBookStart(const size_t first) override {
    if (summary_.found && nextFirst_ == kNone) nextFirst_ = first;
  }

  void onBookEnd(const Book& book, const size_t last) override {
    // The loader skips an entry without a docId; so does everything counted here.
    if (!book.docId.empty()) {
      ++summary_.bookCount;
      if (book.hasFinishedCount ? book.finishedCount > 0 : book.finishedFlag) ++summary_.finishedBookCount;
      if (ReadingStatsStore::countsTowardPace(book.progress)) {
        summary_.paceSeconds += book.totalSeconds;
        summary_.pacePercents += book.progress;
      }
      if (!summary_.found && !findDocId_.empty() && book.docId == findDocId_) {
        summary_.found = true;
        summary_.target.docId = book.docId;
        summary_.target.totalSeconds = book.totalSeconds;
        summary_.target.sessions = book.sessions;
        summary_.target.pagesTurned = book.pagesTurned;
        summary_.target.days = book.days;
        // What a load would have kept of it, which is what removeBook() would take out.
        ReadingStatsStore::trimBookDays(summary_.target.days);
        targetFirst_ = book.first;
        targetLast_ = last;
        prevLast_ = lastBookLast_;
      }
    }
    lastBookLast_ = last;
  }

  Summary& summary_;
  const std::string& findDocId_;
  size_t lastBookLast_ = kNone;
  size_t prevLast_ = kNone;
  size_t targetFirst_ = kNone;
  size_t targetLast_ = kNone;
  size_t nextFirst_ = kNone;
};

// Copies the books array's contents through, adding each book's time-to-finish before its closing
// brace.
class DashboardCopy final : public Scanner {
 public:
  DashboardCopy(Print& out, const float pooledRate) : out_(out), pooledRate_(pooledRate) {}

 private:
  void onBookEnd(const Book& book, size_t) override {
    const float own = ReadingStatsStore::ownSecondsPerPercent(book.totalSeconds, book.progress);
    const float remaining = book.progress < 100 ? 100.0f - static_cast<float>(book.progress) : 0.0f;
    const uint32_t eta = ReadingStatsStore::etaSeconds(own > 0.0f ? own : pooledRate_, remaining);
    const int n = snprintf(pending_, sizeof(pending_), "%s\"etaSeconds\":%lu", book.hasFields ? "," : "",
                           static_cast<unsigned long>(eta));
    pendingLen_ = n > 0 ? static_cast<size_t>(n) : 0;
  }

  void onByte(const char c, size_t, const bool inBooks) override {
    if (!inBooks) return;
    if (pendingLen_ > 0) {
      // This is the brace that closed the book.
      emit(out_, pending_, pendingLen_);
      pendingLen_ = 0;
    }
    out_.write(static_cast<uint8_t>(c));
  }

  Print& out_;
  const float pooledRate_;
  char pending_[32] = {};
  size_t pendingLen_ = 0;
};

// Copies the books array's contents through, less one byte range.
class CopyWithout final : public Scanner {
 public:
  CopyWithout(Print& out, const size_t cutFirst, const size_t cutLast)
      : out_(out), cutFirst_(cutFirst), cutLast_(cutLast) {}

 private:
  void onByte(const char c, const size_t offset, const bool inBooks) override {
    if (inBooks && (offset < cutFirst_ || offset > cutLast_)) out_.write(static_cast<uint8_t>(c));
  }

  Print& out_;
  const size_t cutFirst_;
  const size_t cutLast_;
};

}  // namespace

bool summarize(HalFile& in, Summary& summary, const std::string& findDocId) {
  summary = Summary{};
  SummaryScan scan(summary, findDocId);
  if (!scan.run(in)) return false;
  // What a load would hold: the newest kMaxGlobalDays buckets, the record folded in first.
  ReadingStatsStore::trimGlobalDays(summary.globalDays, summary.longestStreak);
  scan.settleCut();
  return true;
}

void writeDashboard(HalFile& in, const Summary& summary, const uint16_t today, Print& out) {
  char head[192];
  int n = snprintf(head, sizeof(head),
                   "{\"totalSeconds\":%lu,\"totalSessions\":%lu,\"totalPagesTurned\":%lu,\"bookCount\":%lu,"
                   "\"finishedBookCount\":%lu,\"todayDayIndex\":%u",
                   static_cast<unsigned long>(summary.totalSeconds), static_cast<unsigned long>(summary.totalSessions),
                   static_cast<unsigned long>(summary.totalPagesTurned), static_cast<unsigned long>(summary.bookCount),
                   static_cast<unsigned long>(summary.finishedBookCount), today);
  emit(out, head, static_cast<size_t>(n));
  if (today != 0 && !summary.globalDays.empty()) {
    n = snprintf(head, sizeof(head), ",\"currentStreak\":%u,\"longestStreak\":%u",
                 ReadingStatsStore::currentStreakIn(summary.globalDays, today),
                 ReadingStatsStore::longestStreakIn(summary.globalDays, summary.longestStreak));
    emit(out, head, static_cast<size_t>(n));
  }
  emit(out, ",\"globalDays\":");
  emitDays(out, summary.globalDays);
  emit(out, ",\"books\":[");
  DashboardCopy copy(
      out, ReadingStatsStore::pooledSecondsPerPercent(summary.totalSeconds, summary.paceSeconds, summary.pacePercents));
  if (!copy.run(in)) {
    LOG_ERR("RSF", "Stats file changed or failed between passes; dashboard truncated");
  }
  emit(out, "]}");
}

void writeWithoutTarget(HalFile& in, const Summary& summary, Print& out) {
  uint32_t totalSeconds = summary.totalSeconds;
  uint32_t totalSessions = summary.totalSessions;
  uint32_t totalPagesTurned = summary.totalPagesTurned;
  std::vector<DayBucket> globalDays = summary.globalDays;
  ReadingStatsStore::takeOut(summary.target, totalSeconds, totalSessions, totalPagesTurned, globalDays);

  char head[160];
  const int n = snprintf(head, sizeof(head),
                         "{\"totalSeconds\":%lu,\"totalSessions\":%lu,\"totalPagesTurned\":%lu,\"longestStreak\":%u,"
                         "\"globalDays\":",
                         static_cast<unsigned long>(totalSeconds), static_cast<unsigned long>(totalSessions),
                         static_cast<unsigned long>(totalPagesTurned), summary.longestStreak);
  emit(out, head, static_cast<size_t>(n));
  emitDays(out, globalDays);
  emit(out, ",\"books\":[");
  CopyWithout copy(out, summary.cutFirst, summary.cutLast);
  if (!copy.run(in)) {
    LOG_ERR("RSF", "Stats file changed or failed between passes; copy truncated");
  }
  emit(out, "]}");
}

}  // namespace ReadingStatsFile
