// ReadingStatsFile: the stats file streamed for the web server instead of loaded. The dashboard
// payload and a single-book removal are produced straight from the file, copying book entries
// through byte for byte; these tests pin both outputs to hand-derived text.
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "ReadingStatsFile.h"

namespace {

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

// Three books as the device writes them. A's title carries escapes that must survive untouched.
const std::string kBookA =
    R"({"docId":"a","title":"Say \"hi\" é","author":"X","totalSeconds":1000,"pagesTurned":17,"sessions":2,)"
    R"("firstReadEpoch":0,"lastReadEpoch":0,"progress":25,"finishedCount":0,"lastFinishedEpoch":0,"finished":false,)"
    R"("days":[[20463,600],[20464,400]]})";
const std::string kBookB =
    R"({"docId":"b","title":"B","author":"","totalSeconds":300,"pagesTurned":5,"sessions":1,"firstReadEpoch":0,)"
    R"("lastReadEpoch":0,"progress":40,"finishedCount":1,"lastFinishedEpoch":0,"finished":true,"days":[[20463,300]]})";
const std::string kBookC =
    R"({"docId":"c","title":"C","author":"","totalSeconds":50,"pagesTurned":1,"sessions":1,"firstReadEpoch":0,)"
    R"("lastReadEpoch":0,"progress":1,"finishedCount":0,"lastFinishedEpoch":0,"finished":false,"days":[[20463,50]]})";

const std::string kFile = R"({"totalSeconds":1350,"totalSessions":4,"totalPagesTurned":23,"longestStreak":2,)"
                          R"("globalDays":[[20463,950],[20464,400]],"books":[)" +
                          kBookA + "," + kBookB + "," + kBookC + "]}";

std::vector<std::pair<uint16_t, uint32_t>> pairsOf(const std::vector<DayBucket>& days) {
  std::vector<std::pair<uint16_t, uint32_t>> out;
  for (const auto& d : days) out.emplace_back(d.dayIndex, d.seconds);
  return out;
}

std::string removeFrom(const std::string& file, const std::string& docId) {
  HalFile in = HalFile::fromString(file);
  ReadingStatsFile::Summary summary;
  EXPECT_TRUE(ReadingStatsFile::summarize(in, summary, docId));
  EXPECT_TRUE(summary.found);
  StringPrint out;
  if (summary.found) ReadingStatsFile::writeWithoutTarget(in, summary, out);
  return out.text;
}

std::string dashboardOf(const std::string& file, const uint16_t today) {
  HalFile in = HalFile::fromString(file);
  ReadingStatsFile::Summary summary;
  EXPECT_TRUE(ReadingStatsFile::summarize(in, summary));
  StringPrint out;
  ReadingStatsFile::writeDashboard(in, summary, today, out);
  return out.text;
}

// A book entry without its closing brace, for splicing the dashboard's added field in.
std::string open(const std::string& book) { return book.substr(0, book.size() - 1); }

}  // namespace

TEST(ReadingStatsFileSummary, AddsUpTheFile) {
  HalFile in = HalFile::fromString(kFile);
  ReadingStatsFile::Summary summary;

  ASSERT_TRUE(ReadingStatsFile::summarize(in, summary));

  EXPECT_EQ(summary.totalSeconds, 1350u);
  EXPECT_EQ(summary.totalSessions, 4u);
  EXPECT_EQ(summary.totalPagesTurned, 23u);
  EXPECT_EQ(summary.longestStreak, 2u);
  EXPECT_EQ(pairsOf(summary.globalDays), (std::vector<std::pair<uint16_t, uint32_t>>{{20463, 950}, {20464, 400}}));
  EXPECT_EQ(summary.bookCount, 3u);
  EXPECT_EQ(summary.finishedBookCount, 1u);
  // C, at 1%, is not far enough in to count toward the pace.
  EXPECT_EQ(summary.paceSeconds, 1300u);
  EXPECT_EQ(summary.pacePercents, 65u);
  EXPECT_FALSE(summary.found);
}

TEST(ReadingStatsFileSummary, FindsTheBookAskedFor) {
  HalFile in = HalFile::fromString(kFile);
  ReadingStatsFile::Summary summary;

  ASSERT_TRUE(ReadingStatsFile::summarize(in, summary, "a"));

  ASSERT_TRUE(summary.found);
  EXPECT_EQ(summary.target.totalSeconds, 1000u);
  EXPECT_EQ(summary.target.sessions, 2u);
  EXPECT_EQ(summary.target.pagesTurned, 17u);
  EXPECT_EQ(pairsOf(summary.target.days), (std::vector<std::pair<uint16_t, uint32_t>>{{20463, 600}, {20464, 400}}));
}

TEST(ReadingStatsFileSummary, UnknownBookIsNotFound) {
  HalFile in = HalFile::fromString(kFile);
  ReadingStatsFile::Summary summary;

  EXPECT_TRUE(ReadingStatsFile::summarize(in, summary, "nope"));
  EXPECT_FALSE(summary.found);
}

TEST(ReadingStatsFileSummary, RejectsATruncatedFile) {
  HalFile in = HalFile::fromString(kFile.substr(0, kFile.size() / 2));
  ReadingStatsFile::Summary summary;

  EXPECT_FALSE(ReadingStatsFile::summarize(in, summary, "a"));
}

TEST(ReadingStatsFileSummary, RejectsSomethingThatIsNotJson) {
  HalFile in = HalFile::fromString("not a stats file");
  ReadingStatsFile::Summary summary;

  EXPECT_FALSE(ReadingStatsFile::summarize(in, summary));
}

TEST(ReadingStatsFileRemove, MiddleBook) {
  EXPECT_EQ(removeFrom(kFile, "b"), R"({"totalSeconds":1050,"totalSessions":3,"totalPagesTurned":18,"longestStreak":2,)"
                                    R"("globalDays":[[20463,650],[20464,400]],"books":[)" +
                                        kBookA + "," + kBookC + "]}");
}

TEST(ReadingStatsFileRemove, FirstBookAndTheDayOnlyItWasReadOn) {
  EXPECT_EQ(removeFrom(kFile, "a"), R"({"totalSeconds":350,"totalSessions":2,"totalPagesTurned":6,"longestStreak":2,)"
                                    R"("globalDays":[[20463,350]],"books":[)" +
                                        kBookB + "," + kBookC + "]}");
}

TEST(ReadingStatsFileRemove, LastBook) {
  EXPECT_EQ(removeFrom(kFile, "c"), R"({"totalSeconds":1300,"totalSessions":3,"totalPagesTurned":22,"longestStreak":2,)"
                                    R"("globalDays":[[20463,900],[20464,400]],"books":[)" +
                                        kBookA + "," + kBookB + "]}");
}

TEST(ReadingStatsFileRemove, OnlyBook) {
  const std::string file = R"({"totalSeconds":1000,"totalSessions":2,"totalPagesTurned":17,"longestStreak":2,)"
                           R"("globalDays":[[20463,600],[20464,400]],"books":[)" +
                           kBookA + "]}";

  EXPECT_EQ(removeFrom(file, "a"), R"({"totalSeconds":0,"totalSessions":0,"totalPagesTurned":0,"longestStreak":2,)"
                                   R"("globalDays":[],"books":[]})");
}

TEST(ReadingStatsFileRemove, KeepsAnOverlongTitleVerbatim) {
  // Longer than the parser's 512-byte token buffer, which drops such a string rather than
  // truncating it -- harmless only because titles are copied, never decoded.
  const std::string longA = R"({"docId":"a","title":")" + std::string(600, 'x') +
                            R"(","author":"","totalSeconds":1000,"pagesTurned":17,"sessions":2,"progress":25,)"
                            R"("days":[[20463,600],[20464,400]]})";
  const std::string file = R"({"totalSeconds":1350,"totalSessions":4,"totalPagesTurned":23,"longestStreak":2,)"
                           R"("globalDays":[[20463,950],[20464,400]],"books":[)" +
                           longA + "," + kBookB + "," + kBookC + "]}";

  EXPECT_EQ(removeFrom(file, "b"), R"({"totalSeconds":1050,"totalSessions":3,"totalPagesTurned":18,"longestStreak":2,)"
                                   R"("globalDays":[[20463,650],[20464,400]],"books":[)" +
                                       longA + "," + kBookC + "]}");
}

TEST(ReadingStatsFileRemove, CopesWithWhitespaceBetweenBooks) {
  // A file tidied up by hand.
  const std::string file = R"({"totalSeconds":1300,"totalSessions":3,"totalPagesTurned":22,"longestStreak":2,)"
                           R"("globalDays":[[20463,900],[20464,400]],"books":[)"
                           "\n  " +
                           kBookA + ",\n  " + kBookB + "\n]}";

  EXPECT_EQ(removeFrom(file, "a"), R"({"totalSeconds":300,"totalSessions":1,"totalPagesTurned":5,"longestStreak":2,)"
                                   R"("globalDays":[[20463,300]],"books":[)"
                                   "\n  " +
                                       kBookB + "\n]}");
}

TEST(ReadingStatsFileDashboard, AddsTheTimeToFinishToEveryBook) {
  // A: 1000 s over 25% -> 40 s/% x 75% = 3000 s. B: 300 s over 40% -> 7.5 s/% x 60% = 450 s.
  // C, at 1%, has no pace of its own and takes the global one: 1300 s / 65% = 20 s/% x 99% = 1980 s.
  EXPECT_EQ(dashboardOf(kFile, 20464),
            R"({"totalSeconds":1350,"totalSessions":4,"totalPagesTurned":23,"bookCount":3,"finishedBookCount":1,)"
            R"("todayDayIndex":20464,"currentStreak":2,"longestStreak":2,)"
            R"("globalDays":[[20463,950],[20464,400]],"books":[)" +
                open(kBookA) + R"(,"etaSeconds":3000})" + "," + open(kBookB) + R"(,"etaSeconds":450})" + "," +
                open(kBookC) + R"(,"etaSeconds":1980})" + "]}");
}

TEST(ReadingStatsFileDashboard, LeavesStreaksOutWithoutAClock) {
  const std::string file = R"({"totalSeconds":300,"totalSessions":1,"totalPagesTurned":5,"longestStreak":1,)"
                           R"("globalDays":[[20463,300]],"books":[)" +
                           kBookB + "]}";

  EXPECT_EQ(dashboardOf(file, 0),
            R"({"totalSeconds":300,"totalSessions":1,"totalPagesTurned":5,"bookCount":1,"finishedBookCount":1,)"
            R"("todayDayIndex":0,"globalDays":[[20463,300]],"books":[)" +
                open(kBookB) + R"(,"etaSeconds":450})" + "]}");
}

TEST(ReadingStatsFileDashboard, FinishedBookNeedsNoTime) {
  const std::string done =
      R"({"docId":"d","title":"D","author":"","totalSeconds":900,"pagesTurned":9,"sessions":1,"progress":100,)"
      R"("finishedCount":1,"days":[[20463,900]]})";
  const std::string file = R"({"totalSeconds":900,"totalSessions":1,"totalPagesTurned":9,"longestStreak":1,)"
                           R"("globalDays":[[20463,900]],"books":[)" +
                           done + "]}";

  EXPECT_EQ(dashboardOf(file, 0),
            R"({"totalSeconds":900,"totalSessions":1,"totalPagesTurned":9,"bookCount":1,"finishedBookCount":1,)"
            R"("todayDayIndex":0,"globalDays":[[20463,900]],"books":[)" +
                open(done) + R"(,"etaSeconds":0})" + "]}");
}
