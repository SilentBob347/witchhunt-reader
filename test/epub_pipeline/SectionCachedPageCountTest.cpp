// Section::cachedPageCount reads another spine's page count from its cache header alone, so the
// reader can count a chapter split over several spine items (#325) without loading them. It reads
// the header at fixed offsets, so these pin it to what a real build writes: the count of a
// finished build, and nothing for a missing, other-settings or still-running one.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "section_cached_page_count_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams defaultParams() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  return p;
}

std::shared_ptr<Epub> loadTables(const std::string& tag) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_tables.epub", freshCacheDir(tag));
  EXPECT_TRUE(epub->load(true));
  return epub;
}

}  // namespace

TEST(SectionCachedPageCount, ReadsTheCountOfAFinishedBuild) {
  GfxRenderer renderer;
  auto epub = loadTables("finished");
  const Section::BuildParams p = defaultParams();

  EXPECT_FALSE(Section::cachedPageCount(epub->getCachePath(), 0, p).has_value()) << "nothing built yet";

  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_GT(section.pageCount, 0);

  const auto count = Section::cachedPageCount(epub->getCachePath(), 0, p);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(section.pageCount, *count);
  EXPECT_FALSE(Section::cachedPageCount(epub->getCachePath(), 1, p).has_value()) << "a sibling never built";
}

TEST(SectionCachedPageCount, IgnoresACacheBuiltForOtherSettings) {
  GfxRenderer renderer;
  auto epub = loadTables("other_settings");
  const Section::BuildParams p = defaultParams();

  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));

  Section::BuildParams narrower = p;
  narrower.viewportWidth = 400;
  EXPECT_FALSE(Section::cachedPageCount(epub->getCachePath(), 0, narrower).has_value());
}

TEST(SectionCachedPageCount, AcceptsTheNoCssFallbackVariantLikeLoadSectionFileDoes) {
  GfxRenderer renderer;
  auto epub = loadTables("no_css_fallback");
  const Section::BuildParams p = defaultParams();  // embeddedStyle = false

  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));

  Section::BuildParams withCss = p;
  withCss.embeddedStyle = true;
  Section loader(epub, 0, renderer);
  ASSERT_TRUE(loader.loadSectionFile(withCss)) << "precondition: the reader would open this cache";

  const auto count = Section::cachedPageCount(epub->getCachePath(), 0, withCss);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(section.pageCount, *count);
}

// Background-B builds a sibling in slices while the reader keeps turning pages; until the build
// patches its header, the half-written file must not be taken for a count.
TEST(SectionCachedPageCount, CountsASlicedBuildOnlyOnceItHasFinished) {
  GfxRenderer renderer;
  auto epub = loadTables("in_flight");
  const Section::BuildParams p = defaultParams();

  // Without a ticking clock the host's millis() is frozen and the sliced build finishes in one call.
  const host_clock::Ticking tick(1);
  Section section(epub, 0, renderer);
  int liveSlices = 0;
  Section::BuildStep step = Section::BuildStep::More;
  for (int i = 0; i < 20000; ++i) {
    step = section.stepSectionBuild(p, /*budgetMs=*/1);
    if (step == Section::BuildStep::Done || step == Section::BuildStep::Failed) break;
    ++liveSlices;
    EXPECT_FALSE(Section::cachedPageCount(epub->getCachePath(), 0, p).has_value())
        << "counted a build still in flight after slice " << liveSlices;
  }
  ASSERT_EQ(Section::BuildStep::Done, step);
  ASSERT_GT(liveSlices, 0) << "the build finished in one slice; the in-flight state was never reached";

  const auto count = Section::cachedPageCount(epub->getCachePath(), 0, p);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(section.pageCount, *count);
}
