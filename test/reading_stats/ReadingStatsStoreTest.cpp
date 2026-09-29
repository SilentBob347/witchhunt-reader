// ReadingStatsStore against real files: every update, query and web payload on reading-stats.bin,
// through the file-backed HalStorage shim in a temporary directory. The crash tests rebuild the card
// the way a power cut would leave it and check that the history reads as it did before the update.
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "HalClock.h"
#include "ReadingStats.h"

// Link stubs: ReadingStats.cpp reaches the clock only for "today".
namespace HalClock {
time_t now() { return 0; }
bool isSynced() { return false; }
}  // namespace HalClock

namespace {

using ReadResult = ReadingStatsStore::ReadResult;
using WriteResult = ReadingStatsStore::WriteResult;

// 2026-01-10 12:00 UTC: day 20463 in any zone within eleven hours of UTC.
constexpr time_t kNoon = 1768046400;
constexpr time_t kDay = 86400;
constexpr size_t kSeqAt = 8;  // the header's seq (ReadingStatsSlotFile.cpp)

// A document id as KOReaderDocumentId makes them: 32 lowercase hex characters.
std::string id(const unsigned n) {
  char text[33];
  snprintf(text, sizeof(text), "%032x", n);
  return text;
}

class StringPrint : public Print {
 public:
  std::string text;
  size_t write(uint8_t b) override {
    text.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    text.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

std::string readBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::string& path, const std::string& bytes) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

uint32_t seqOfCopy(const std::string& bytes, const size_t copy) {
  const auto* p = reinterpret_cast<const uint8_t*>(bytes.data() + copy * ReadingStatsSlotFile::kMetaSize + kSeqAt);
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ =
        std::filesystem::temp_directory_path() / (std::string("rst-") + info->test_suite_name() + "-" + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    path_ = (dir_ / "reading-stats.bin").generic_string();
    legacy_ = (dir_ / "reading-stats.json").generic_string();
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static ReadingStatsStore::Summary summaryOf(ReadingStatsStore& store, const bool withIndex = false) {
    ReadingStatsStore::Summary summary;
    EXPECT_EQ(store.querySummary(summary, withIndex), ReadResult::Ok);
    return summary;
  }

  static ReadingStatsStore::BookQuery bookOf(ReadingStatsStore& store, const std::string& docId) {
    ReadingStatsStore::BookQuery query;
    EXPECT_EQ(store.queryBook(docId, query), ReadResult::Ok);
    return query;
  }

  // Two books: 1000 s at 25 % and 300 s at 40 %, so a global pace of 1300 s / 65 % = 20 s per %.
  static void twoBooks(ReadingStatsStore& store) {
    ASSERT_EQ(store.recordSession(id(1), "Book A", "X", 1000, 17, 25, kNoon), WriteResult::Done);
    ASSERT_EQ(store.recordSession(id(2), "B", "", 300, 5, 40, kNoon), WriteResult::Done);
  }

  // The cap's worth of books, 60 s each; id(7) read longest ago.
  static void fullHistory(ReadingStatsStore& store) {
    for (unsigned i = 0; i < ReadingStatsStore::kMaxBooks; ++i) {
      const time_t wall = i == 7 ? kNoon - kDay : kNoon + static_cast<time_t>(i);
      ASSERT_EQ(store.recordSession(id(i), "T", "", 60, 1, 5, wall), WriteResult::Done);
    }
  }

  std::filesystem::path dir_;
  std::string path_;
  std::string legacy_;
};

}  // namespace

TEST_F(StoreTest, SummaryOfNoHistoryIsEmpty) {
  ReadingStatsStore store(path_, legacy_);

  const auto summary = summaryOf(store);

  EXPECT_EQ(summary.bookCount, 0u);
  EXPECT_EQ(summary.totalSeconds, 0u);
  EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(StoreTest, FirstSessionCreatesTheFile) {
  ReadingStatsStore store(path_, legacy_);

  ASSERT_EQ(store.recordSession(id(1), "Book A", "X", 600, 10, 20, kNoon), WriteResult::Done);

  EXPECT_EQ(std::filesystem::file_size(path_), ReadingStatsSlotFile::kFileSize);
  const auto query = bookOf(store, id(1));
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.totalSeconds, 600u);
  EXPECT_EQ(query.book.title, "Book A");
  EXPECT_EQ(query.book.author, "X");
  const auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 600u);
}

TEST_F(StoreTest, ZeroSecondSessionForANewBookWritesNothing) {
  ReadingStatsStore store(path_, legacy_);

  EXPECT_EQ(store.recordSession(id(1), "Book A", "", 0, 0, 3, kNoon), WriteResult::Done);

  EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(StoreTest, SessionsMergeIntoTheirBook) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_EQ(store.recordSession(id(1), "Book A", "X", 600, 10, 20, kNoon), WriteResult::Done);
  ASSERT_EQ(store.recordSession(id(2), "B", "", 100, 2, 10, kNoon), WriteResult::Done);

  ASSERT_EQ(store.recordSession(id(1), "Book A", "X", 150, 3, 50, kNoon + kDay), WriteResult::Done);

  const auto query = bookOf(store, id(1));
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.totalSeconds, 750u);
  EXPECT_EQ(query.book.sessions, 2u);
  EXPECT_EQ(query.book.progress, 50);
  EXPECT_EQ(query.book.days.size(), 2u);
  const auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 2u);
  EXPECT_EQ(summary.totalSeconds, 850u);
  EXPECT_EQ(summary.totalSessions, 3u);
}

TEST_F(StoreTest, QueryBookFindsItAndTheGlobalPace) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));

  const auto query = bookOf(store, id(2));

  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.totalSeconds, 300u);
  EXPECT_FLOAT_EQ(query.pooledPace, 20.0f);
}

TEST_F(StoreTest, QueryBooksAtReadsTheIndexedEntries) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));
  const auto summary = summaryOf(store, /*withIndex=*/true);
  ASSERT_EQ(summary.byTime.size(), 2u);
  std::vector<BookReadingStats> books;

  ASSERT_EQ(store.queryBooksAt(summary.byTime, 0, 2, summary.seq, books), ReadResult::Ok);

  ASSERT_EQ(books.size(), 2u);
  EXPECT_EQ(books[0].docId, id(1));  // 1000 s: first by time
  EXPECT_EQ(books[0].title, "Book A");
  EXPECT_EQ(books[1].docId, id(2));
}

TEST_F(StoreTest, QueryBooksAtNoticesAChangedHistory) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));
  const auto summary = summaryOf(store, /*withIndex=*/true);
  // The book's old slot still holds a valid copy of it: only the meta's seq tells the index is old.
  ASSERT_EQ(store.recordSession(id(1), "Book A", "X", 60, 1, 26, kNoon), WriteResult::Done);
  std::vector<BookReadingStats> books;

  EXPECT_EQ(store.queryBooksAt(summary.byTime, 0, 2, summary.seq, books), ReadResult::Stale);
}

TEST_F(StoreTest, PrefetchReportsKnownAndUnknownBooks) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));

  store.prefetchRecent({id(2), id(9), "not-a-document-id"});

  const auto* b = store.recent(id(2));
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(b->known);
  EXPECT_EQ(b->totalSeconds, 300u);
  EXPECT_EQ(b->progress, 40);
  EXPECT_EQ(b->knownDays, 1);
  ASSERT_NE(store.recent(id(9)), nullptr);
  EXPECT_FALSE(store.recent(id(9))->known);
  ASSERT_NE(store.recent("not-a-document-id"), nullptr);
  EXPECT_FALSE(store.recent("not-a-document-id")->known);
  EXPECT_EQ(store.recent(id(1)), nullptr);  // not asked for
  EXPECT_FLOAT_EQ(store.recentPooledPace(), 20.0f);
}

TEST_F(StoreTest, PrefetchShowsTheLatestFigures) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));
  store.prefetchRecent({id(2)});

  ASSERT_EQ(store.recordSession(id(2), "B", "", 200, 2, 45, kNoon), WriteResult::Done);
  store.prefetchRecent({id(2)});

  ASSERT_NE(store.recent(id(2)), nullptr);
  EXPECT_EQ(store.recent(id(2))->totalSeconds, 500u);
  EXPECT_EQ(store.recent(id(2))->progress, 45);
}

TEST_F(StoreTest, NewBookAtTheCapEvictsTheLeastRecentlyRead) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(fullHistory(store));

  ASSERT_EQ(store.recordSession(id(100), "New", "", 30, 1, 1, kNoon + kDay), WriteResult::Done);

  const auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, ReadingStatsStore::kMaxBooks);
  // The evicted book's reading still counts: eviction frees the entry, not the history's totals.
  EXPECT_EQ(summary.totalSeconds, 6030u);
  EXPECT_FALSE(bookOf(store, id(7)).found);
  EXPECT_TRUE(bookOf(store, id(100)).found);
}

TEST_F(StoreTest, ManyUpdatesOnAFullHistoryLeakNoSlot) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(fullHistory(store));
  uint32_t seconds = 100 * 60;

  for (unsigned round = 0; round < 300; ++round) {
    // Every third round a book never seen, which evicts; the other two re-read the newest one.
    const unsigned book = 1000 + round - round % 3;
    ASSERT_EQ(store.recordSession(id(book), "T", "", 30, 1, 5, kNoon + 1000 + round), WriteResult::Done);
    seconds += 30;
  }

  const auto summary = summaryOf(store, /*withIndex=*/true);
  EXPECT_EQ(summary.bookCount, ReadingStatsStore::kMaxBooks);
  EXPECT_EQ(summary.totalSeconds, seconds);
  std::vector<BookReadingStats> books;
  ASSERT_EQ(store.queryBooksAt(summary.byTime, 0, summary.byTime.size(), summary.seq, books), ReadResult::Ok);
  ASSERT_EQ(books.size(), ReadingStatsStore::kMaxBooks);
  for (const auto& book : books) EXPECT_EQ(book.title, "T");
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path_.c_str(), file));
  auto meta = ReadingStatsSlotFile::Meta::create();
  uint8_t live = ReadingStatsSlotFile::kNoCopy;
  ASSERT_EQ(ReadingStatsSlotFile::loadMeta(file, *meta, live), ReadingStatsSlotFile::Load::Ok);
  EXPECT_EQ(meta->bookCount(), ReadingStatsStore::kMaxBooks);
  EXPECT_NE(meta->freeSlot(), ReadingStatsSlotFile::kNoSlot);
}

TEST_F(StoreTest, MarkFinishedRespectsTheCap) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(fullHistory(store));

  ASSERT_EQ(store.markFinished(id(100), "New", "", kNoon + kDay), WriteResult::Done);

  EXPECT_EQ(summaryOf(store).bookCount, ReadingStatsStore::kMaxBooks);
  EXPECT_FALSE(bookOf(store, id(7)).found);
}

TEST_F(StoreTest, MarkFinishedCountsAndCreatesTheEntry) {
  ReadingStatsStore store(path_, legacy_);

  ASSERT_EQ(store.markFinished(id(1), "Book A", "", kNoon), WriteResult::Done);

  const auto query = bookOf(store, id(1));
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.finishedCount, 1);
  EXPECT_EQ(query.book.progress, 100);
  EXPECT_EQ(summaryOf(store).finishedBookCount, 1u);
}

TEST_F(StoreTest, MarkFinishedOnABookWithRealReading) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_EQ(store.recordSession(id(1), "Real", "A", 5400, 90, 97, kNoon), WriteResult::Done);

  ASSERT_EQ(store.markFinished(id(1), "Real", "A", kNoon + kDay), WriteResult::Done);

  const auto query = bookOf(store, id(1));
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.finishedCount, 1);
  EXPECT_EQ(query.book.lastFinishedEpoch, kNoon + kDay);
  EXPECT_EQ(query.book.totalSeconds, 5400u);
}

TEST_F(StoreTest, SessionOnAFinishedBookIsRecorded) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_EQ(store.recordSession(id(1), "Real", "A", 5400, 90, 100, kNoon), WriteResult::Done);
  ASSERT_EQ(store.markFinished(id(1), "Real", "A", kNoon), WriteResult::Done);

  ASSERT_EQ(store.recordSession(id(1), "Real", "A", 600, 10, 100, kNoon + kDay), WriteResult::Done);

  const auto query = bookOf(store, id(1));
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.totalSeconds, 6000u);
  EXPECT_EQ(query.book.finishedCount, 1);
}

TEST_F(StoreTest, RemoveTakesTheBookOutOfTheTotals) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));

  ASSERT_EQ(store.removeBook(id(1)), WriteResult::Done);

  const auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 300u);
  EXPECT_EQ(summary.totalSessions, 1u);
  EXPECT_EQ(store.removeBook(id(1)), WriteResult::NotFound);
}

TEST_F(StoreTest, RemoveFromNoHistoryIsNotFound) {
  ReadingStatsStore store(path_, legacy_);

  EXPECT_EQ(store.removeBook(id(1)), WriteResult::NotFound);

  EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(StoreTest, WhatIsNotADocumentIdIsNeverStored) {
  ReadingStatsStore store(path_, legacy_);

  EXPECT_EQ(store.recordSession("a", "A", "", 60, 1, 1, kNoon), WriteResult::Failed);
  EXPECT_EQ(store.removeBook("a"), WriteResult::NotFound);
  EXPECT_FALSE(bookOf(store, "a").found);
  EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(StoreTest, CorruptFileIsSetAsideAndAFreshHistoryStarts) {
  writeBytes(path_, "junk");
  ReadingStatsStore store(path_, legacy_);
  ReadingStatsStore::Summary summary;
  EXPECT_EQ(store.querySummary(summary), ReadResult::Corrupt);

  ASSERT_EQ(store.recordSession(id(1), "Book A", "", 60, 1, 1, kNoon), WriteResult::Done);

  EXPECT_EQ(readBytes((dir_ / "reading-stats.corrupt.bin").generic_string()), "junk");
  summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 60u);
}

TEST_F(StoreTest, RemoveOnACorruptFileFailsAndLeavesIt) {
  writeBytes(path_, "junk");
  ReadingStatsStore store(path_, legacy_);

  EXPECT_EQ(store.removeBook(id(1)), WriteResult::Failed);

  EXPECT_EQ(readBytes(path_), "junk");
}

TEST_F(StoreTest, AwkwardTitlesSurviveExactly) {
  ReadingStatsStore store(path_, legacy_);
  const std::string title = "Say \"hi\" \\ back\nslash";
  const std::string control = std::string("ctl") + '\x01';

  ASSERT_EQ(store.recordSession(id(1), title, "", 60, 1, 1, kNoon), WriteResult::Done);
  ASSERT_EQ(store.recordSession(id(2), control, "", 60, 1, 1, kNoon), WriteResult::Done);

  EXPECT_EQ(bookOf(store, id(1)).book.title, title);
  EXPECT_EQ(bookOf(store, id(2)).book.title, control);
}

TEST_F(StoreTest, LongTitleIsCutWhole) {
  ReadingStatsStore store(path_, legacy_);
  const std::string e = "\xC3\xA9";
  const std::string title = std::string(ReadingStatsSlotFile::kTitleMax - 1, 't') + e + "tail";

  ASSERT_EQ(store.recordSession(id(1), title, "", 60, 1, 1, kNoon), WriteResult::Done);

  EXPECT_EQ(bookOf(store, id(1)).book.title, std::string(ReadingStatsSlotFile::kTitleMax - 1, 't'));
}

TEST_F(StoreTest, CrashAfterTheSlotWriteLeavesTheOldHistory) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_EQ(store.recordSession(id(1), "A", "", 600, 5, 10, kNoon), WriteResult::Done);
  const std::string before = readBytes(path_);
  ASSERT_EQ(store.recordSession(id(2), "B", "", 300, 3, 5, kNoon), WriteResult::Done);

  // Power lost after the slot write, before the meta write: the new slot is on the card, the
  // meta copies are as they were.
  std::string cut = readBytes(path_);
  cut.replace(0, ReadingStatsSlotFile::kSlotsOffset, before, 0, ReadingStatsSlotFile::kSlotsOffset);
  writeBytes(path_, cut);

  auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 600u);
  EXPECT_FALSE(bookOf(store, id(2)).found);
  // And the history takes the next session as if nothing had happened.
  ASSERT_EQ(store.recordSession(id(2), "B", "", 300, 3, 5, kNoon), WriteResult::Done);
  summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 2u);
  EXPECT_EQ(summary.totalSeconds, 900u);
}

TEST_F(StoreTest, TornMetaFallsBackToTheOlderCopy) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_EQ(store.recordSession(id(1), "A", "", 600, 5, 10, kNoon), WriteResult::Done);
  ASSERT_EQ(store.recordSession(id(2), "B", "", 300, 3, 5, kNoon), WriteResult::Done);

  // The meta write of the second session torn by a power cut: its copy fails its CRC.
  std::string torn = readBytes(path_);
  const size_t newest = seqOfCopy(torn, 1) > seqOfCopy(torn, 0) ? 1 : 0;
  torn[newest * ReadingStatsSlotFile::kMetaSize + 100] ^= 0x01;
  writeBytes(path_, torn);

  const auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 600u);
  EXPECT_TRUE(bookOf(store, id(1)).found);
}

TEST_F(StoreTest, TornMetaThenAnotherSessionKeepsGoing) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_EQ(store.recordSession(id(1), "A", "", 600, 5, 10, kNoon), WriteResult::Done);
  ASSERT_EQ(store.recordSession(id(2), "B", "", 300, 3, 5, kNoon), WriteResult::Done);
  std::string torn = readBytes(path_);
  const size_t newest = seqOfCopy(torn, 1) > seqOfCopy(torn, 0) ? 1 : 0;
  torn[newest * ReadingStatsSlotFile::kMetaSize + 100] ^= 0x01;
  writeBytes(path_, torn);

  ASSERT_EQ(store.recordSession(id(3), "C", "", 50, 1, 5, kNoon), WriteResult::Done);

  const auto summary = summaryOf(store);
  EXPECT_EQ(summary.bookCount, 2u);
  EXPECT_EQ(summary.totalSeconds, 650u);
  EXPECT_TRUE(bookOf(store, id(1)).found);
  EXPECT_TRUE(bookOf(store, id(3)).found);
}

TEST_F(StoreTest, StaleTemporaryFileIsDiscarded) {
  writeBytes(path_ + ".tmp", "half a history");
  ReadingStatsStore store(path_, legacy_);

  ASSERT_EQ(store.recordSession(id(1), "A", "", 60, 1, 1, kNoon), WriteResult::Done);

  EXPECT_FALSE(std::filesystem::exists(path_ + ".tmp"));
  EXPECT_EQ(summaryOf(store).bookCount, 1u);
}

TEST_F(StoreTest, LegacyHistoryIsLeftAloneUntilImported) {
  writeBytes(legacy_, "{}");
  ReadingStatsStore store(path_, legacy_);

  EXPECT_EQ(store.recordSession(id(1), "A", "", 60, 1, 1, kNoon), WriteResult::Failed);

  EXPECT_FALSE(std::filesystem::exists(path_));
  EXPECT_EQ(readBytes(legacy_), "{}");
}

TEST_F(StoreTest, DashboardGivesEveryBookItsTimeToFinish) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));
  // At 1 % no pace of its own: it takes the global one.
  ASSERT_EQ(store.recordSession(id(3), "C", "", 50, 1, 1, kNoon), WriteResult::Done);
  StringPrint out;

  ASSERT_EQ(store.writeDashboard(out, 0), ReadResult::Ok);

  EXPECT_EQ(out.text.rfind(R"({"totalSeconds":1350,"totalSessions":3,"totalPagesTurned":23,"bookCount":3,)"
                           R"("finishedBookCount":0,"todayDayIndex":0,)",
                           0),
            0u);
  // A: 1000 s / 25 % = 40 s/% x 75 %. B: 300 s / 40 % = 7.5 s/% x 60 %. C: 20 s/% x 99 %.
  EXPECT_NE(out.text.find(R"("etaSeconds":3000})"), std::string::npos);
  EXPECT_NE(out.text.find(R"("etaSeconds":450})"), std::string::npos);
  EXPECT_NE(out.text.find(R"("etaSeconds":1980})"), std::string::npos);
  EXPECT_EQ(out.text.substr(out.text.size() - 2), "]}");
}

TEST_F(StoreTest, ExportIsTheJsonFileFormat) {
  ReadingStatsStore store(path_, legacy_);
  ASSERT_NO_FATAL_FAILURE(twoBooks(store));
  const auto summary = summaryOf(store);
  StringPrint expected;
  ReadingStatsFile::writeFileHead(expected, summary);
  ReadingStatsFile::writeBook(expected, bookOf(store, id(1)).book);
  ReadingStatsFile::writeBookSeparator(expected);
  ReadingStatsFile::writeBook(expected, bookOf(store, id(2)).book);
  ReadingStatsFile::writeTail(expected);
  StringPrint out;

  ASSERT_EQ(store.writeExport(out), ReadResult::Ok);

  EXPECT_EQ(out.text, expected.text);
  EXPECT_EQ(out.text.rfind(R"({"totalSeconds":1300,"totalSessions":2,"totalPagesTurned":22,)", 0), 0u);
}

TEST_F(StoreTest, ExportOfNoHistoryIsAnEmptyFile) {
  ReadingStatsStore store(path_, legacy_);
  StringPrint out;

  ASSERT_EQ(store.writeExport(out), ReadResult::Ok);

  EXPECT_EQ(out.text, R"({"totalSeconds":0,"totalSessions":0,"totalPagesTurned":0,"longestStreak":0,)"
                      R"("globalDays":[],"books":[]})");
}
