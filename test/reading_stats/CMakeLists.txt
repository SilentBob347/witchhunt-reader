add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ReadingStatsFileTest.cpp
  ${REPO_ROOT}/src/ReadingStats.cpp
  ${REPO_ROOT}/src/ReadingStatsFile.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${REPO_ROOT}/src
  # The no-op HalStorage JsonSettingsIO.h and ReadingStats.cpp pull in. It must precede lib/hal:
  # the real HalStorage.h there wants SdFat.
  ${REPO_ROOT}/test/shims
  # HalClock.h, whose now()/isSynced() the test stubs.
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)

# localDayIndexFromEpoch() calls localtime_r, which MinGW's <time.h> declares only on request.
if(MINGW)
  target_compile_definitions(ReadingStatsTest PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)
endif()

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
