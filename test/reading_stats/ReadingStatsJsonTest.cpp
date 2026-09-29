// ReadingStatsJson: the reading history as JSON -- the legacy file scanned for the import, one entry
// decoded at an offset, and the writers the web payloads are built from, pinned to hand-derived text.
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "ReadingStatsJson.h"

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

using ScanResult = ReadingStatsJson::ScanResult;

ReadingStatsJson::Summary scanOf(const std::string& file, const ReadingStatsJson::ScanRequest& request,
                                 ScanResult expected = ScanResult::Ok) {
  HalFile in = HalFile::fromString(file);
  ReadingStatsJson::Summary summary;
  EXPECT_EQ(ReadingStatsJson::scan(in, summary, request), expected);
  return summary;
}

const std::string kBookD =
    R"({"docId":"d","title":"D","author":"","totalSeconds":120,"pagesTurned":3,"sessions":1,"firstReadEpoch":0,)"
    R"("lastReadEpoch":0,"progress":5,"finishedCount":0,"lastFinishedEpoch":0,"finished":false,)"
    R"("days":[[20464,120]]})";

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

// A book entry without its closing brace, for splicing the dashboard's added field in.
std::string open(const std::string& book) { return book.substr(0, book.size() - 1); }

}  // namespace

TEST(ReadingStatsJsonSummary, AddsUpTheFile) {
  const auto summary = scanOf(kFile, {});

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
}

TEST(ReadingStatsJsonSummary, RejectsSomethingThatIsNotJson) { scanOf("not a stats file", {}, ScanResult::Malformed); }

TEST(ReadingStatsJsonScan, DecodesTheWholeTarget) {
  const std::string head = R"({"totalSeconds":900,"books":[)";
  const std::string dated =
      // A JSON unicode escape, six characters on disk; the parser passes it through undecoded.
      R"({"docId":"d","title":"Say \"hi\" )" + std::string("\\u00e9") +
      R"(","author":"X","totalSeconds":900,"pagesTurned":9,"sessions":3,)"
      R"("firstReadEpoch":1767225600,"lastReadEpoch":1768046400,"progress":100,"finishedCount":2,)"
      R"("lastFinishedEpoch":1768046400,"finished":true,"days":[[20463,900]]})";
  HalFile in = HalFile::fromString(head + dated + "]}");
  BookReadingStats b;

  ASSERT_EQ(ReadingStatsJson::readBookAt(in, head.size(), b), ScanResult::Ok);
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
}

TEST(ReadingStatsJsonScan, LegacyFinishedFlagCountsAsOneFinish) {
  const std::string legacy = R"({"docId":"l","totalSeconds":60,"progress":100,"finished":true,"days":[]})";
  const std::string head = R"({"totalSeconds":60,"books":[)";
  const std::string file = head + legacy + "]}";
  HalFile in = HalFile::fromString(file);
  BookReadingStats book;

  ASSERT_EQ(ReadingStatsJson::readBookAt(in, head.size(), book), ScanResult::Ok);

  EXPECT_EQ(book.finishedCount, 1);
  EXPECT_EQ(scanOf(file, {}).finishedBookCount, 1u);
}

TEST(ReadingStatsJsonScan, IndexOrdersBooksByTime) {
  ReadingStatsJson::ScanRequest request;
  request.wantIndex = true;

  const auto summary = scanOf(kFile, request);

  std::vector<std::pair<uint32_t, uint32_t>> index;
  for (const auto& e : summary.byTime) index.emplace_back(e.totalSeconds, e.offset);
  EXPECT_EQ(index, (std::vector<std::pair<uint32_t, uint32_t>>{{1000, static_cast<uint32_t>(kFile.find(kBookA))},
                                                               {300, static_cast<uint32_t>(kFile.find(kBookB))},
                                                               {50, static_cast<uint32_t>(kFile.find(kBookC))}}));
}

TEST(ReadingStatsJsonScan, TruncatedFileIsMalformed) {
  scanOf(kFile.substr(0, kFile.size() / 2), {}, ScanResult::Malformed);
}

TEST(ReadingStatsJsonBookAt, DecodesTheEntryAtAnOffset) {
  HalFile in = HalFile::fromString(kFile);
  BookReadingStats book;

  ASSERT_EQ(ReadingStatsJson::readBookAt(in, kFile.find(kBookB), book), ScanResult::Ok);

  EXPECT_EQ(book.docId, "b");
  EXPECT_EQ(book.title, "B");
  EXPECT_EQ(book.totalSeconds, 300u);
  EXPECT_EQ(book.finishedCount, 1);
  EXPECT_EQ(pairsOf(book.days), (std::vector<std::pair<uint16_t, uint32_t>>{{20463, 300}}));
}

TEST(ReadingStatsJsonBookAt, CutOffEntryIsMalformed) {
  const std::string cut = kFile.substr(0, kFile.find(kBookA) + kBookA.size() / 2);
  HalFile in = HalFile::fromString(cut);
  BookReadingStats book;

  EXPECT_EQ(ReadingStatsJson::readBookAt(in, kFile.find(kBookA), book), ScanResult::Malformed);
}

TEST(ReadingStatsJsonWriteBook, EscapesTheTitleAndKeepsTheFieldOrder) {
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

  ReadingStatsJson::writeBook(out, b);

  EXPECT_EQ(out.text,
            R"({"docId":"e","title":"A \"q\" \\ x\ny\u0001","author":"Ö","totalSeconds":5,"pagesTurned":2,)"
            R"("sessions":1,"firstReadEpoch":1767225600,"lastReadEpoch":1768046400,"progress":7,"finishedCount":0,)"
            R"("lastFinishedEpoch":0,"finished":false,"days":[[20463,5]]})");
}

TEST(ReadingStatsJsonWriteBook, TakesTheLargestValuesEveryFieldCanHold) {
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

  ReadingStatsJson::writeBook(out, b);

  EXPECT_EQ(out.text,
            R"({"docId":"m","title":"M","author":"","totalSeconds":4294967295,"pagesTurned":4294967295,)"
            R"("sessions":4294967295,"firstReadEpoch":1767225600,"lastReadEpoch":1768046400,"progress":100,)"
            R"("finishedCount":65535,"lastFinishedEpoch":1768046400,"finished":true,"days":[[20463,4294967295]]})");
}

TEST(ReadingStatsJsonWriteBook, AddsTheTimeToFinishBeforeTheClosingBrace) {
  StringPrint plain;
  StringPrint withEta;

  ReadingStatsJson::writeBook(plain, bookD());
  ReadingStatsJson::writeBook(withEta, bookD(), 1980);

  EXPECT_EQ(plain.text, kBookD);
  EXPECT_EQ(withEta.text, open(kBookD) + R"(,"etaSeconds":1980})");
}

TEST(ReadingStatsJsonHeads, FileHeadAndBooksMakeTheFile) {
  StringPrint out;

  ReadingStatsJson::writeFileHead(out, totalsOf(1350, 4, 23, {{20463, 950}, {20464, 400}}));
  out.text += kBookA;
  ReadingStatsJson::writeBookSeparator(out);
  out.text += kBookB;
  ReadingStatsJson::writeBookSeparator(out);
  out.text += kBookC;
  ReadingStatsJson::writeTail(out);

  EXPECT_EQ(out.text, kFile);
}

TEST(ReadingStatsJsonHeads, DashboardHeadCarriesCountsAndStreaks) {
  StringPrint out;

  ReadingStatsJson::writeDashboardHead(out, totalsOf(1350, 4, 23, {{20463, 950}, {20464, 400}}), 3, 1, 20464);

  EXPECT_EQ(out.text,
            R"({"totalSeconds":1350,"totalSessions":4,"totalPagesTurned":23,"bookCount":3,"finishedBookCount":1,)"
            R"("todayDayIndex":20464,"currentStreak":2,"longestStreak":2,)"
            R"("globalDays":[[20463,950],[20464,400]],"books":[)");
}

TEST(ReadingStatsJsonHeads, DashboardHeadLeavesStreaksOutWithoutAClock) {
  StringPrint out;

  ReadingStatsJson::writeDashboardHead(out, totalsOf(300, 1, 5, {{20463, 300}}), 1, 1, 0);

  EXPECT_EQ(out.text,
            R"({"totalSeconds":300,"totalSessions":1,"totalPagesTurned":5,"bookCount":1,"finishedBookCount":1,)"
            R"("todayDayIndex":0,"globalDays":[[20463,300]],"books":[)");
}
