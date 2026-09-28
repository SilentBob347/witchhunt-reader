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

using ScanResult = ReadingStatsFile::ScanResult;

ReadingStatsFile::Summary scanOf(const std::string& file, const ReadingStatsFile::ScanRequest& request,
                                 ScanResult expected = ScanResult::Ok) {
  HalFile in = HalFile::fromString(file);
  ReadingStatsFile::Summary summary;
  EXPECT_EQ(ReadingStatsFile::scan(in, summary, request), expected);
  return summary;
}

// B after one more session, as the device would write it.
const std::string kBookBAfter =
    R"({"docId":"b","title":"B","author":"","totalSeconds":450,"pagesTurned":8,"sessions":2,"firstReadEpoch":0,)"
    R"("lastReadEpoch":0,"progress":50,"finishedCount":1,"lastFinishedEpoch":0,"finished":true,)"
    R"("days":[[20463,300],[20464,150]]})";
const std::string kBookD =
    R"({"docId":"d","title":"D","author":"","totalSeconds":120,"pagesTurned":3,"sessions":1,"firstReadEpoch":0,)"
    R"("lastReadEpoch":0,"progress":5,"finishedCount":0,"lastFinishedEpoch":0,"finished":false,)"
    R"("days":[[20464,120]]})";

BookReadingStats bookBAfter() {
  BookReadingStats b;
  b.docId = "b";
  b.title = "B";
  b.totalSeconds = 450;
  b.pagesTurned = 8;
  b.sessions = 2;
  b.progress = 50;
  b.finishedCount = 1;
  b.days = {{20463, 300}, {20464, 150}};
  return b;
}

BookReadingStats bookD() {
  BookReadingStats d;
  d.docId = "d";
  d.title = "D";
  d.totalSeconds = 120;
  d.pagesTurned = 3;
  d.sessions = 1;
  d.progress = 5;
  d.days = {{20464, 120}};
  return d;
}

ReadingTotals totalsOf(uint32_t seconds, uint32_t sessions, uint32_t pages, std::vector<DayBucket> days) {
  ReadingTotals t;
  t.totalSeconds = seconds;
  t.totalSessions = sessions;
  t.totalPagesTurned = pages;
  t.longestStreak = 2;
  t.globalDays = std::move(days);
  return t;
}

std::string rewriteOf(const std::string* file, const ReadingStatsFile::Rewrite& rewrite) {
  StringPrint out;
  if (file == nullptr) {
    EXPECT_EQ(ReadingStatsFile::writeRewrite(nullptr, rewrite, out), ReadingStatsFile::ScanResult::Ok);
  } else {
    HalFile in = HalFile::fromString(*file);
    EXPECT_EQ(ReadingStatsFile::writeRewrite(&in, rewrite, out), ReadingStatsFile::ScanResult::Ok);
  }
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

  // Entries are copied one by one and the separators re-emitted, so the hand-added whitespace goes.
  EXPECT_EQ(removeFrom(file, "a"), R"({"totalSeconds":300,"totalSessions":1,"totalPagesTurned":5,"longestStreak":2,)"
                                   R"("globalDays":[[20463,300]],"books":[)" +
                                       kBookB + "]}");
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

TEST(ReadingStatsFileScan, DecodesTheWholeTarget) {
  const std::string head = R"({"totalSeconds":900,"books":[)";
  const std::string dated =
      // A JSON unicode escape, six characters on disk; the parser passes it through undecoded.
      R"({"docId":"d","title":"Say \"hi\" )" + std::string("\\u00e9") +
      R"(","author":"X","totalSeconds":900,"pagesTurned":9,"sessions":3,)"
      R"("firstReadEpoch":1767225600,"lastReadEpoch":1768046400,"progress":100,"finishedCount":2,)"
      R"("lastFinishedEpoch":1768046400,"finished":true,"days":[[20463,900]]})";
  ReadingStatsFile::ScanRequest request;
  request.findDocId = "d";

  const auto summary = scanOf(head + dated + "]}", request);

  ASSERT_TRUE(summary.found);
  const BookReadingStats& b = summary.target;
  // The parser passes \u escapes through undecoded; titles are for display only.
  EXPECT_EQ(b.title, "Say \"hi\" \\u00e9");
  EXPECT_EQ(b.author, "X");
  EXPECT_EQ(b.totalSeconds, 900u);
  EXPECT_EQ(b.pagesTurned, 9u);
  EXPECT_EQ(b.sessions, 3u);
  EXPECT_EQ(b.firstReadEpoch, 1767225600);
  EXPECT_EQ(b.lastReadEpoch, 1768046400);
  EXPECT_EQ(b.progress, 100);
  EXPECT_EQ(b.finishedCount, 2);
  EXPECT_EQ(b.lastFinishedEpoch, 1768046400);
  EXPECT_EQ(pairsOf(b.days), (std::vector<std::pair<uint16_t, uint32_t>>{{20463, 900}}));
  EXPECT_EQ(summary.targetFirst, head.size());
}

TEST(ReadingStatsFileScan, LegacyFinishedFlagCountsAsOneFinish) {
  const std::string legacy = R"({"docId":"l","totalSeconds":60,"progress":100,"finished":true,"days":[]})";
  ReadingStatsFile::ScanRequest request;
  request.findDocId = "l";

  const auto summary = scanOf(R"({"totalSeconds":60,"books":[)" + legacy + "]}", request);

  ASSERT_TRUE(summary.found);
  EXPECT_EQ(summary.target.finishedCount, 1);
  EXPECT_EQ(summary.finishedBookCount, 1u);
}

TEST(ReadingStatsFileScan, IndexOrdersBooksByTime) {
  ReadingStatsFile::ScanRequest request;
  request.wantIndex = true;

  const auto summary = scanOf(kFile, request);

  std::vector<std::pair<uint32_t, uint32_t>> index;
  for (const auto& e : summary.byTime) index.emplace_back(e.totalSeconds, e.offset);
  EXPECT_EQ(index, (std::vector<std::pair<uint32_t, uint32_t>>{{1000, static_cast<uint32_t>(kFile.find(kBookA))},
                                                               {300, static_cast<uint32_t>(kFile.find(kBookB))},
                                                               {50, static_cast<uint32_t>(kFile.find(kBookC))}}));
}

TEST(ReadingStatsFileScan, VictimIsTheLeastRecentlyReadThenTheLeastRead) {
  const std::string x = R"({"docId":"x","totalSeconds":10,"lastReadEpoch":300,"days":[]})";
  const std::string y = R"({"docId":"y","totalSeconds":300,"lastReadEpoch":100,"days":[]})";
  const std::string z = R"({"docId":"z","totalSeconds":50,"lastReadEpoch":100,"days":[]})";
  const std::string file = R"({"totalSeconds":360,"books":[)" + x + "," + y + "," + z + "]}";
  ReadingStatsFile::ScanRequest request;
  request.wantVictim = true;

  const auto summary = scanOf(file, request);

  ASSERT_TRUE(summary.hasVictim);
  EXPECT_EQ(summary.victimDocId, "z");
  EXPECT_EQ(summary.victimFirst, file.find(z));
}

TEST(ReadingStatsFileScan, RecentsReportKnownAndUnknownBooks) {
  ReadingStatsFile::ScanRequest request;
  request.recentDocIds = {"b", "zz"};

  const auto summary = scanOf(kFile, request);

  ASSERT_EQ(summary.recents.size(), 2u);
  EXPECT_EQ(summary.recents[0].docId, "b");
  EXPECT_TRUE(summary.recents[0].known);
  EXPECT_EQ(summary.recents[0].totalSeconds, 300u);
  EXPECT_EQ(summary.recents[0].knownDays, 1);
  EXPECT_EQ(summary.recents[0].progress, 40);
  EXPECT_EQ(summary.recents[1].docId, "zz");
  EXPECT_FALSE(summary.recents[1].known);
}

TEST(ReadingStatsFileScan, TruncatedFileIsMalformed) {
  scanOf(kFile.substr(0, kFile.size() / 2), {}, ScanResult::Malformed);
}

TEST(ReadingStatsFileBookAt, DecodesTheEntryAtAnOffset) {
  HalFile in = HalFile::fromString(kFile);
  BookReadingStats book;

  ASSERT_EQ(ReadingStatsFile::readBookAt(in, kFile.find(kBookB), book), ScanResult::Ok);

  EXPECT_EQ(book.docId, "b");
  EXPECT_EQ(book.title, "B");
  EXPECT_EQ(book.totalSeconds, 300u);
  EXPECT_EQ(book.finishedCount, 1);
  EXPECT_EQ(pairsOf(book.days), (std::vector<std::pair<uint16_t, uint32_t>>{{20463, 300}}));
}

TEST(ReadingStatsFileBookAt, CutOffEntryIsMalformed) {
  const std::string cut = kFile.substr(0, kFile.find(kBookA) + kBookA.size() / 2);
  HalFile in = HalFile::fromString(cut);
  BookReadingStats book;

  EXPECT_EQ(ReadingStatsFile::readBookAt(in, kFile.find(kBookA), book), ScanResult::Malformed);
}

TEST(ReadingStatsFileWriteBook, EscapesTheTitleAndKeepsTheFieldOrder) {
  BookReadingStats b;
  b.docId = "e";
  b.title = "A \"q\" \\ x\ny\x01";
  b.author = "Ö";
  b.totalSeconds = 5;
  b.pagesTurned = 2;
  b.sessions = 1;
  b.firstReadEpoch = 1767225600;
  b.lastReadEpoch = 1768046400;
  b.progress = 7;
  b.days = {{20463, 5}};
  StringPrint out;

  ReadingStatsFile::writeBook(out, b);

  EXPECT_EQ(out.text,
            R"({"docId":"e","title":"A \"q\" \\ x\ny\u0001","author":"Ö","totalSeconds":5,"pagesTurned":2,)"
            R"("sessions":1,"firstReadEpoch":1767225600,"lastReadEpoch":1768046400,"progress":7,"finishedCount":0,)"
            R"("lastFinishedEpoch":0,"finished":false,"days":[[20463,5]]})");
}

TEST(ReadingStatsFileRewrite, ReplacesAnEntryInPlace) {
  const BookReadingStats after = bookBAfter();
  ReadingStatsFile::Rewrite rewrite;
  rewrite.totals = totalsOf(1500, 5, 26, {{20463, 950}, {20464, 550}});
  rewrite.replaceAt = kFile.find(kBookB);
  rewrite.replacement = &after;

  EXPECT_EQ(rewriteOf(&kFile, rewrite),
            R"({"totalSeconds":1500,"totalSessions":5,"totalPagesTurned":26,"longestStreak":2,)"
            R"("globalDays":[[20463,950],[20464,550]],"books":[)" +
                kBookA + "," + kBookBAfter + "," + kBookC + "]}");
}

TEST(ReadingStatsFileRewrite, DropsOneEntryAndAppendsAnother) {
  const BookReadingStats d = bookD();
  ReadingStatsFile::Rewrite rewrite;
  rewrite.totals = totalsOf(1470, 5, 26, {{20463, 950}, {20464, 520}});
  rewrite.dropAt = kFile.find(kBookC);
  rewrite.append = &d;

  EXPECT_EQ(rewriteOf(&kFile, rewrite),
            R"({"totalSeconds":1470,"totalSessions":5,"totalPagesTurned":26,"longestStreak":2,)"
            R"("globalDays":[[20463,950],[20464,520]],"books":[)" +
                kBookA + "," + kBookB + "," + kBookD + "]}");
}

TEST(ReadingStatsFileRewrite, WithoutAnInputWritesJustTheNewBook) {
  const BookReadingStats d = bookD();
  ReadingStatsFile::Rewrite rewrite;
  rewrite.totals = totalsOf(120, 1, 3, {{20464, 120}});
  rewrite.append = &d;

  EXPECT_EQ(rewriteOf(nullptr, rewrite),
            R"({"totalSeconds":120,"totalSessions":1,"totalPagesTurned":3,"longestStreak":2,)"
            R"("globalDays":[[20464,120]],"books":[)" +
                kBookD + "]}");
}

TEST(ReadingStatsFileRewrite, OutputScansBack) {
  const BookReadingStats after = bookBAfter();
  ReadingStatsFile::Rewrite rewrite;
  rewrite.totals = totalsOf(1500, 5, 26, {{20463, 950}, {20464, 550}});
  rewrite.replaceAt = kFile.find(kBookB);
  rewrite.replacement = &after;
  const std::string written = rewriteOf(&kFile, rewrite);
  ReadingStatsFile::ScanRequest request;
  request.findDocId = "b";

  const auto summary = scanOf(written, request);

  EXPECT_EQ(summary.bookCount, 3u);
  ASSERT_TRUE(summary.found);
  EXPECT_EQ(summary.target.totalSeconds, 450u);
  EXPECT_EQ(summary.totalSeconds, 1500u);
}

TEST(ReadingStatsFileWriteBook, TakesTheLargestValuesEveryFieldCanHold) {
  // A finished book on a synced clock with the widest counters: every field at its longest.
  BookReadingStats b;
  b.docId = "m";
  b.title = "M";
  b.totalSeconds = 4294967295u;
  b.pagesTurned = 4294967295u;
  b.sessions = 4294967295u;
  b.firstReadEpoch = 1767225600;
  b.lastReadEpoch = 1768046400;
  b.progress = 100;
  b.finishedCount = 65535;
  b.lastFinishedEpoch = 1768046400;
  b.days = {{20463, 4294967295u}};
  StringPrint out;

  ReadingStatsFile::writeBook(out, b);

  EXPECT_EQ(out.text,
            R"({"docId":"m","title":"M","author":"","totalSeconds":4294967295,"pagesTurned":4294967295,)"
            R"("sessions":4294967295,"firstReadEpoch":1767225600,"lastReadEpoch":1768046400,"progress":100,)"
            R"("finishedCount":65535,"lastFinishedEpoch":1768046400,"finished":true,"days":[[20463,4294967295]]})");
}
