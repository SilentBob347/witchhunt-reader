// Parked image decodes (RenderConfig::checkpointPath).
//
// The reader's image lane hands its region back on every page turn, so a decode it cannot finish
// is stopped by CooperativeAbort. It used to be thrown away and started over -- in runs 18-21 (X3)
// the lane discarded 73 s of preempted decodes against 54 s it completed. Now the decode parks: the
// rows already done stay in the caches' .part files, a checkpoint holds the decoder's and the
// pipeline's state, and the next decode of the same image resumes.
//
// The guarantee tested here: however often a decode is stopped, and wherever, the caches it
// finally publishes are byte for byte those of an uninterrupted decode -- both variants, for the
// full progressive decoder and for TJpgDec.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "CooperativeAbort.h"
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PixelCache.h"
#include "GfxRenderer.h"
#include "HalStorage.h"

namespace fs = std::filesystem;

namespace {

// Fires on the Nth poll of the abort hook (1-based); 0 never fires.
int gPollsLeft = 0;
int gPolls = 0;
bool countedAbort() {
  ++gPolls;
  if (gPollsLeft <= 0) return false;
  return --gPollsLeft == 0;
}

struct ImageParkFixture : testing::TestWithParam<const char*> {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    work = fs::temp_directory_path() / ("img_park_" + std::to_string(std::hash<std::string>{}(name)));
    fs::remove_all(work);
    fs::create_directories(work);
    CooperativeAbort::setLongTaskAbortPredicate(countedAbort);
    gPollsLeft = 0;
    gPolls = 0;
    CooperativeAbort::clearAborted();
  }
  void TearDown() override {
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
    fs::remove_all(work);
  }

  std::string image() const { return std::string(JPEG_FIXTURE_DIR "/") + GetParam(); }
  std::string at(const std::string& name) const { return (work / name).string(); }

  // The reader's configuration: the 1-bit variant drawn, the 4-level one as its companion, a
  // downscale that area-averages (so the carry across rows of blocks is in play).
  RenderConfig config(const std::string& tag, const int width = 150, const int height = 104,
                      const bool checkpoint = true) const {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = width;
    c.maxHeight = height;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = at(tag + "_bw.pxc");
    c.companionCachePath = at(tag + "_grey.pxc");
    if (checkpoint) c.checkpointPath = at(tag + ".ckpt");
    return c;
  }

  // One decode call; true when it stopped for input (the lane's "preempted").
  bool decodeOnce(const RenderConfig& c, const int abortOnPoll) {
    gPollsLeft = abortOnPoll;
    CooperativeAbort::clearAborted();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(image(), renderer, c));
    gPollsLeft = 0;
    return CooperativeAbort::consumeAborted();
  }

  static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

TEST_P(ImageParkFixture, EveryParkPointResumesToTheUninterruptedCaches) {
  const RenderConfig ref = config("ref", 150, 104, /*checkpoint=*/false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const auto refBw = read(ref.cachePath);
  const auto refGrey = read(ref.companionCachePath);
  ASSERT_GT(refBw.size(), PixelCache::PXC_HEADER_BYTES);
  ASSERT_EQ(refBw.size(), refGrey.size());
  ASSERT_NE(refBw, refGrey);

  // How many times an uninterrupted checkpointable decode polls the hook: every one is a park point.
  const RenderConfig probe = config("probe");
  gPolls = 0;
  ASSERT_FALSE(decodeOnce(probe, 0));
  const int polls = gPolls;
  ASSERT_GT(polls, 3) << "the fixture must be stoppable at several points";
  EXPECT_EQ(read(probe.cachePath), refBw) << "checkpointing must not change an uninterrupted decode";

  for (int stop = 1; stop <= polls; ++stop) {
    SCOPED_TRACE("stopped at poll " + std::to_string(stop));
    const RenderConfig c = config("p" + std::to_string(stop));
    ASSERT_TRUE(decodeOnce(c, stop));
    EXPECT_FALSE(fs::exists(c.cachePath)) << "a parked decode publishes nothing";
    EXPECT_TRUE(fs::exists(c.checkpointPath));
    EXPECT_TRUE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    ASSERT_FALSE(decodeOnce(c, 0)) << "the resumed decode runs to the end";
    EXPECT_FALSE(fs::exists(c.checkpointPath)) << "a finished decode leaves no checkpoint";
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    EXPECT_EQ(read(c.cachePath), refBw);
    EXPECT_EQ(read(c.companionCachePath), refGrey);
  }
}

// A decode stopped again and again -- a reader flipping pages -- still ends with the same caches.
TEST_P(ImageParkFixture, RepeatedParksStillEndInTheSameCaches) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const RenderConfig c = config("many");
  int parks = 0;
  while (decodeOnce(c, 2)) {  // every call makes one poll's worth of progress, then parks
    ++parks;
    ASSERT_LT(parks, 1000);
  }
  EXPECT_GT(parks, 1);
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
}

// A checkpoint only resumes the decode it was taken of: another box starts over, cleanly.
TEST_P(ImageParkFixture, AChangedConfigurationStartsOver) {
  const RenderConfig small = config("ref_small", 120, 83, false);
  ASSERT_FALSE(decodeOnce(small, 0));

  RenderConfig c = config("changed");
  ASSERT_TRUE(decodeOnce(c, 3));
  ASSERT_TRUE(fs::exists(c.checkpointPath));
  c.maxWidth = 120;
  c.maxHeight = 83;
  ASSERT_FALSE(decodeOnce(c, 0));
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_EQ(read(c.cachePath), read(small.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(small.companionCachePath));
}

// A partial cache that no longer matches its checkpoint (a crash between the two writes, a
// card error) fails that one decode and clears the way: the next one starts over.
TEST_P(ImageParkFixture, ADamagedPartialFileStartsOverNextTime) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));

  const RenderConfig c = config("damaged");
  ASSERT_TRUE(decodeOnce(c, 3));
  const std::string part = PixelCache::partPathFor(c.cachePath);
  fs::resize_file(part, fs::file_size(part) + 1);

  JpegToFramebufferConverter converter;
  EXPECT_FALSE(converter.decodeToFramebuffer(image(), renderer, c)) << "this decode fails";
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_FALSE(fs::exists(part));
  EXPECT_FALSE(fs::exists(c.cachePath));

  ASSERT_FALSE(decodeOnce(c, 0));
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
}

// Without a checkpoint path a stopped decode is thrown away, as before, and leaves nothing.
TEST_P(ImageParkFixture, WithoutACheckpointPathAStopIsDiscarded) {
  const RenderConfig c = config("nockpt", 150, 104, false);
  const bool stopped = decodeOnce(c, 2);
  if (!stopped) GTEST_SKIP() << "this decoder only polls when it can park";
  EXPECT_FALSE(fs::exists(c.cachePath));
  EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
}

INSTANTIATE_TEST_SUITE_P(ProgressiveAndBaseline, ImageParkFixture,
                         testing::Values("prog_full_420.jpg", "prog_full_420_base.jpg", "prog_full_gray.jpg"));

}  // namespace
