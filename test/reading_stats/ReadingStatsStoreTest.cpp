// ReadingStatsStore against real files: the streamed queries, the recent-books cache and (from the
// streamed-writes task on) every update, through the file-backed HalStorage shim.
#include <gtest/gtest.h>

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

using ScanResult = ReadingStatsFile::ScanResult;

const std::string kBookA =
    R"({"docId":"a","title":"Book A","author":"X","totalSeconds":1000,"pagesTurned":17,"sessions":2,)"
    R"("firstReadEpoch":0,"lastReadEpoch":0,"progress":25,"finishedCount":0,"lastFinishedEpoch":0,"finished":false,)"
    R"("days":[[20463,600],[20464,400]]})";
const std::string kBookB =
    R"({"docId":"b","title":"B","author":"","totalSeconds":300,"pagesTurned":5,"sessions":1,"firstReadEpoch":0,)"
    R"("lastReadEpoch":0,"progress":40,"finishedCount":1,"lastFinishedEpoch":0,"finished":true,"days":[[20463,300]]})";
const std::string kFile = R"({"totalSeconds":1300,"totalSessions":3,"totalPagesTurned":22,"longestStreak":2,)"
                          R"("globalDays":[[20463,900],[20464,400]],"books":[)" +
                          kBookA + "," + kBookB + "]}";

constexpr time_t kNoon = 1768046400;
constexpr time_t kDay = 86400;

// A history at the book cap in which "id7" was read longest ago; 60 s each, 6000 s in all.
std::string fullHistory() {
  std::string books;
  for (size_t i = 0; i < ReadingStatsStore::kMaxBooks; ++i) {
    if (i > 0) books += ",";
    const long long lastRead = i == 7 ? 1000 : 2000 + static_cast<long long>(i);
    books += R"({"docId":"id)" + std::to_string(i) + R"(","totalSeconds":60,"lastReadEpoch":)" +
             std::to_string(lastRead) + R"(,"days":[]})";
  }
  return R"({"totalSeconds":6000,"totalSessions":100,"books":[)" + books + "]}";
}

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ =
        std::filesystem::temp_directory_path() / (std::string("rst-") + info->test_suite_name() + "-" + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    path_ = (dir_ / "reading-stats.json").generic_string();
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  void writeFile(const std::string& text) const { std::ofstream(path_, std::ios::binary) << text; }
  std::string readFile() const {
    std::ifstream f(path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  }

  std::filesystem::path dir_;
  std::string path_;
};

}  // namespace

TEST_F(StoreTest, SummaryOfAnAbsentFileIsAnEmptyHistory) {
  ReadingStatsStore store(path_);
  ReadingStatsFile::Summary summary;

  EXPECT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 0u);
  EXPECT_EQ(summary.totalSeconds, 0u);
}

TEST_F(StoreTest, EmptyFileIsAnEmptyHistory) {
  writeFile("");
  ReadingStatsStore store(path_);
  ReadingStatsFile::Summary summary;

  EXPECT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 0u);
}

TEST_F(StoreTest, QueryBookFindsItAndTheGlobalPace) {
  writeFile(kFile);
  ReadingStatsStore store(path_);
  ReadingStatsStore::BookQuery query;

  ASSERT_EQ(store.queryBook("b", query), ScanResult::Ok);

  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.totalSeconds, 300u);
  // (1000 s + 300 s) over (25 % + 40 %) = 20 s per percent.
  EXPECT_FLOAT_EQ(query.pooledPace, 20.0f);
}

TEST_F(StoreTest, QueryBookAtReadsTheIndexedEntry) {
  writeFile(kFile);
  ReadingStatsStore store(path_);
  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary, /*withIndex=*/true), ScanResult::Ok);
  ASSERT_EQ(summary.byTime.size(), 2u);
  BookReadingStats book;

  ASSERT_EQ(store.queryBookAt(summary.byTime[0].offset, book), ScanResult::Ok);

  EXPECT_EQ(book.docId, "a");
  EXPECT_EQ(book.title, "Book A");
}

TEST_F(StoreTest, PrefetchRemembersBooksWithoutHistory) {
  writeFile(kFile);
  ReadingStatsStore store(path_);

  store.prefetchRecent({"b", "zz"});

  const auto* b = store.recent("b");
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(b->known);
  EXPECT_EQ(b->totalSeconds, 300u);
  const auto* zz = store.recent("zz");
  ASSERT_NE(zz, nullptr);
  EXPECT_FALSE(zz->known);
  EXPECT_FLOAT_EQ(store.recentPooledPace(), 20.0f);
}

TEST_F(StoreTest, WarmPrefetchDoesNotReadTheCard) {
  writeFile(kFile);
  ReadingStatsStore store(path_);
  store.prefetchRecent({"b", "zz"});
  // A second prefetch for the same books must be served from the cache: with the file gone, a
  // rescan would report both as unknown.
  std::filesystem::remove(path_);

  store.prefetchRecent({"b", "zz"});

  ASSERT_NE(store.recent("b"), nullptr);
  EXPECT_TRUE(store.recent("b")->known);
}

TEST_F(StoreTest, CacheIsBounded) {
  writeFile(kFile);
  ReadingStatsStore store(path_);
  std::vector<std::string> many;
  for (int i = 0; i < 20; ++i) many.push_back("id" + std::to_string(i));
  store.prefetchRecent(many);

  store.prefetchRecent({"b"});

  EXPECT_NE(store.recent("b"), nullptr);
  size_t cached = 0;
  for (const auto& id : many) cached += store.recent(id) != nullptr ? 1 : 0;
  EXPECT_LE(cached + 1, ReadingStatsStore::kRecentCacheSize);
}

TEST_F(StoreTest, FirstSessionCreatesTheFile) {
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.recordSession("a", "Book A", "X", 600, 10, 20, kNoon), ReadingStatsStore::WriteResult::Done);

  ReadingStatsStore::BookQuery query;
  ASSERT_EQ(store.queryBook("a", query), ScanResult::Ok);
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.totalSeconds, 600u);
  EXPECT_EQ(query.book.title, "Book A");
  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 600u);
}

TEST_F(StoreTest, EmptyFileTakesTheFirstSession) {
  writeFile("");
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.recordSession("a", "Book A", "", 60, 1, 1, kNoon), ReadingStatsStore::WriteResult::Done);

  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 1u);
}

TEST_F(StoreTest, SessionMergesIntoItsBookInPlace) {
  writeFile(kFile);
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.recordSession("b", "B", "", 150, 3, 50, kNoon + kDay), ReadingStatsStore::WriteResult::Done);

  ReadingStatsFile::Summary summary;
  ReadingStatsFile::ScanRequest request;
  request.findDocId = "b";
  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("T", path_.c_str(), in));
  ASSERT_EQ(ReadingStatsFile::scan(in, summary, request), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 2u);
  EXPECT_EQ(summary.totalSeconds, 1450u);
  EXPECT_EQ(summary.totalSessions, 4u);
  ASSERT_TRUE(summary.found);
  EXPECT_EQ(summary.target.totalSeconds, 450u);
  EXPECT_EQ(summary.target.progress, 50);
  // Still second in the file: updated where it was, not moved to the end.
  EXPECT_GT(summary.targetFirst, readFile().find("\"docId\":\"a\""));
}

TEST_F(StoreTest, ZeroSecondSessionForANewBookWritesNothing) {
  ReadingStatsStore store(path_);

  EXPECT_EQ(store.recordSession("a", "Book A", "", 0, 0, 3, kNoon), ReadingStatsStore::WriteResult::Done);

  EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(StoreTest, NewBookAtTheCapEvictsTheLeastRecentlyRead) {
  writeFile(fullHistory());
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.recordSession("new", "New", "", 30, 1, 1, kNoon), ReadingStatsStore::WriteResult::Done);

  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, ReadingStatsStore::kMaxBooks);
  // The evicted book's reading still counts: eviction frees the slot, not the history's totals.
  EXPECT_EQ(summary.totalSeconds, 6030u);
  ReadingStatsStore::BookQuery gone;
  ASSERT_EQ(store.queryBook("id7", gone), ScanResult::Ok);
  EXPECT_FALSE(gone.found);
  ReadingStatsStore::BookQuery added;
  ASSERT_EQ(store.queryBook("new", added), ScanResult::Ok);
  EXPECT_TRUE(added.found);
}

TEST_F(StoreTest, MarkFinishedRespectsTheCap) {
  // The resident store's markFinished() never evicted, so a finish could grow the history past the
  // cap; the shared write path applies it.
  writeFile(fullHistory());
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.markFinished("new", "New", "", kNoon), ReadingStatsStore::WriteResult::Done);

  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, ReadingStatsStore::kMaxBooks);
  ReadingStatsStore::BookQuery gone;
  ASSERT_EQ(store.queryBook("id7", gone), ScanResult::Ok);
  EXPECT_FALSE(gone.found);
}

TEST_F(StoreTest, MarkFinishedCountsAndCreatesTheEntry) {
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.markFinished("a", "Book A", "", kNoon), ReadingStatsStore::WriteResult::Done);

  ReadingStatsStore::BookQuery query;
  ASSERT_EQ(store.queryBook("a", query), ScanResult::Ok);
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.finishedCount, 1);
  EXPECT_EQ(query.book.progress, 100);
}

TEST_F(StoreTest, RemoveTakesTheBookOutOfTheTotals) {
  writeFile(kFile);
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.removeBook("a"), ReadingStatsStore::WriteResult::Done);

  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 300u);
  EXPECT_EQ(store.removeBook("a"), ReadingStatsStore::WriteResult::NotFound);
}

TEST_F(StoreTest, MalformedFileIsSetAsideAndAFreshHistoryStarts) {
  writeFile(R"({"totalSeconds":12,"books":[{"docId":)");
  ReadingStatsStore store(path_);

  ASSERT_EQ(store.recordSession("a", "Book A", "", 60, 1, 1, kNoon), ReadingStatsStore::WriteResult::Done);

  EXPECT_TRUE(std::filesystem::exists(dir_ / "reading-stats.corrupt.json"));
  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 1u);
  EXPECT_EQ(summary.totalSeconds, 60u);
}

TEST_F(StoreTest, AwkwardTitleSurvivesARewrite) {
  ReadingStatsStore store(path_);
  const std::string title = "Say \"hi\" \\ back\nslash";
  ASSERT_EQ(store.recordSession("a", title, "", 60, 1, 1, kNoon), ReadingStatsStore::WriteResult::Done);
  // A control character must not break the file either (it reads back escaped, not decoded).
  ASSERT_EQ(store.recordSession("b", std::string("ctl\x01"), "", 60, 1, 1, kNoon),
            ReadingStatsStore::WriteResult::Done);

  ReadingStatsStore::BookQuery query;
  ASSERT_EQ(store.queryBook("a", query), ScanResult::Ok);
  ASSERT_TRUE(query.found);
  EXPECT_EQ(query.book.title, title);
  ReadingStatsFile::Summary summary;
  ASSERT_EQ(store.querySummary(summary), ScanResult::Ok);
  EXPECT_EQ(summary.bookCount, 2u);
}

TEST_F(StoreTest, WritesKeepTheCacheCurrent) {
  writeFile(kFile);
  ReadingStatsStore store(path_);
  store.prefetchRecent({"a", "b"});

  ASSERT_EQ(store.recordSession("a", "Book A", "X", 200, 2, 30, kNoon), ReadingStatsStore::WriteResult::Done);
  ASSERT_EQ(store.removeBook("b"), ReadingStatsStore::WriteResult::Done);

  ASSERT_NE(store.recent("a"), nullptr);
  EXPECT_EQ(store.recent("a")->totalSeconds, 1200u);
  EXPECT_EQ(store.recent("a")->progress, 30);
  EXPECT_EQ(store.recent("b"), nullptr);
  // Only "a" left, at 30 %: its own pace, 1200 s / 30 % = 40 s per percent.
  EXPECT_FLOAT_EQ(store.recentPooledPace(), 40.0f);
}
