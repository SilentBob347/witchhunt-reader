// ReadingStatsStore::removeBook() -- the "remove this book from reading stats" action behind the
// device's per-book stats screen and the web dashboard. Removing a book takes its contribution
// back out of the global totals and day buckets, so what the stats screens show afterwards is the
// history as if the book had never been read; only the persisted longest-streak record stays.
#include <gtest/gtest.h>

#include <vector>

#include "HalClock.h"
#include "JsonSettingsIO.h"
#include "ReadingStats.h"

// Link stubs. ReadingStats.cpp reaches the clock only for "today" and the JSON layer only on file
// load/save; these tests drive the store in memory and never touch either.
namespace HalClock {
time_t now() { return 0; }
bool isSynced() { return false; }
}  // namespace HalClock

namespace JsonSettingsIO {
bool saveReadingStats(const ReadingStatsStore&, HalFile&) { return false; }
ReadingStatsLoad loadReadingStats(ReadingStatsStore&, HalFile&) { return ReadingStatsLoad::Corrupt; }
}  // namespace JsonSettingsIO

namespace {

constexpr time_t kDay = 86400;
// 2026-01-10 12:00 UTC. Noon, so consecutive multiples of kDay land on consecutive local days in
// whatever timezone the host runs in.
constexpr time_t kNoon = 1768046400;

uint32_t secondsReadOn(const ReadingStatsStore& store, const time_t epoch) {
  return store.getSecondsForDay(localDayIndexFromEpoch(epoch));
}

}  // namespace

TEST(ReadingStatsRemoveBook, DropsOnlyThatBook) {
  ReadingStatsStore store;
  store.recordSession("a", "Book A", "", 600, 10, 20, kNoon);
  store.recordSession("b", "Book B", "", 300, 5, 40, kNoon);

  EXPECT_TRUE(store.removeBook("a"));

  EXPECT_EQ(store.findBook("a"), nullptr);
  ASSERT_NE(store.findBook("b"), nullptr);
  EXPECT_EQ(store.findBook("b")->totalSeconds, 300u);
  EXPECT_EQ(store.getBookCount(), 1u);
}

TEST(ReadingStatsRemoveBook, TakesItsTimeSessionsAndPagesOutOfTheTotals) {
  ReadingStatsStore store;
  store.recordSession("a", "Book A", "", 600, 10, 20, kNoon);
  store.recordSession("a", "Book A", "", 400, 7, 25, kNoon + kDay);
  store.recordSession("b", "Book B", "", 300, 5, 40, kNoon);

  store.removeBook("a");

  EXPECT_EQ(store.getGlobalTotalSeconds(), 300u);
  EXPECT_EQ(store.getGlobalTotalSessions(), 1u);
  EXPECT_EQ(store.getGlobalTotalPagesTurned(), 5u);
}

TEST(ReadingStatsRemoveBook, TakesItsReadingOutOfEachDay) {
  ReadingStatsStore store;
  store.recordSession("a", "Book A", "", 600, 10, 20, kNoon);
  store.recordSession("a", "Book A", "", 400, 7, 25, kNoon + kDay);
  store.recordSession("b", "Book B", "", 300, 5, 40, kNoon + kDay);

  store.removeBook("a");

  EXPECT_EQ(secondsReadOn(store, kNoon), 0u);
  EXPECT_EQ(secondsReadOn(store, kNoon + kDay), 300u);
  // The day only book A was read on goes entirely rather than staying as an empty bucket, which
  // the web dashboard would still receive and the longest-streak walk would still count.
  EXPECT_EQ(store.getGlobalDays().size(), 1u);
}

TEST(ReadingStatsRemoveBook, BreaksTheCurrentStreakItCarried) {
  ReadingStatsStore store;
  store.recordSession("b", "Book B", "", 300, 5, 40, kNoon);
  store.recordSession("a", "Book A", "", 600, 10, 20, kNoon + kDay);
  store.recordSession("a", "Book A", "", 400, 7, 25, kNoon + 2 * kDay);
  const uint16_t today = localDayIndexFromEpoch(kNoon + 2 * kDay);
  ASSERT_EQ(store.computeCurrentStreak(today), 3u);

  store.removeBook("a");

  EXPECT_EQ(store.computeCurrentStreak(today), 0u);
}

TEST(ReadingStatsRemoveBook, KeepsTheLongestStreakRecord) {
  // The record is a number, not derivable from the day buckets once they age out, so there is
  // nothing to take the book back out of: it stays what it was.
  ReadingStatsStore store;
  store.recordSession("a", "Book A", "", 600, 10, 20, kNoon);
  store.recordSession("a", "Book A", "", 400, 7, 25, kNoon + kDay);
  store.recordSession("a", "Book A", "", 200, 3, 30, kNoon + 2 * kDay);
  store.recordSession("b", "Book B", "", 300, 5, 40, kNoon);

  store.removeBook("a");

  EXPECT_EQ(store.getLongestStreakSeen(), 3u);
  EXPECT_EQ(store.computeLongestStreak(), 3u);
}

TEST(ReadingStatsRemoveBook, UnknownBookChangesNothing) {
  ReadingStatsStore store;
  store.recordSession("a", "Book A", "", 600, 10, 20, kNoon);

  EXPECT_FALSE(store.removeBook("nope"));

  EXPECT_EQ(store.getBookCount(), 1u);
  EXPECT_EQ(store.getGlobalTotalSeconds(), 600u);
  EXPECT_EQ(secondsReadOn(store, kNoon), 600u);
}

TEST(ReadingStatsRemoveBook, NeverWrapsTotalsBelowZero) {
  // The history file sits on the SD card and the web dashboard offers it for download; a file
  // edited by hand can carry a book with more time than the global total. Removing that book must
  // leave zero, not four billion seconds.
  ReadingStatsStore store;
  BookReadingStats book;
  book.docId = "a";
  book.totalSeconds = 500;
  book.sessions = 3;
  book.pagesTurned = 40;
  book.days = {{localDayIndexFromEpoch(kNoon), 500}};
  std::vector<BookReadingStats> books;
  books.push_back(book);
  store.replaceLoaded(std::move(books), {{localDayIndexFromEpoch(kNoon), 200}}, 200, 1, 10, 0);

  store.removeBook("a");

  EXPECT_EQ(store.getGlobalTotalSeconds(), 0u);
  EXPECT_EQ(store.getGlobalTotalSessions(), 0u);
  EXPECT_EQ(store.getGlobalTotalPagesTurned(), 0u);
  EXPECT_EQ(secondsReadOn(store, kNoon), 0u);
  EXPECT_TRUE(store.getGlobalDays().empty());
}
