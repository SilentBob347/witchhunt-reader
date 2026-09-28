// ReadingStatsStore against real files: the streamed queries, the recent-books cache and (from the
// streamed-writes task on) every update, through the file-backed HalStorage shim.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "HalClock.h"
#include "JsonSettingsIO.h"
#include "ReadingStats.h"

// Link stubs. The resident store's load/save still reference the JSON layer until the streamed
// writes replace them; nothing here calls them.
namespace HalClock {
time_t now() { return 0; }
bool isSynced() { return false; }
}  // namespace HalClock

namespace JsonSettingsIO {
bool saveReadingStats(const ReadingStatsStore&, HalFile&) { return false; }
ReadingStatsLoad loadReadingStats(ReadingStatsStore&, HalFile&) { return ReadingStatsLoad::Corrupt; }
}  // namespace JsonSettingsIO

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
