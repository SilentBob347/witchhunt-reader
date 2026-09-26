#pragma once

#include <HalStorage.h>

class BuildArena;  // lib/Memory -- optional scratch for the work pool / progressive workspace
class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool eightBit = false,
                                          BuildArena* scratch = nullptr);

 public:
  // grayscale8Bit: emit an 8-bit BMP instead of quantizing to the display's four
  // levels here, deferring dithering to draw time. Only worth it for a consumer
  // that can use the extra tonal range (the sleep screen's adaptive tone filter);
  // it costs 4x the SD footprint of a 2-bit cover.
  // `scratch` (every entry point): a lent region for the decoder's working memory -- the
  // TJpgDec pool or the progressive workspace -- so a cover decode needs only the row pipeline
  // from the heap. Null decodes from the heap behind the historical free-heap gates.
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true, bool grayscale8Bit = false,
                                  BuildArena* scratch = nullptr);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          BuildArena* scratch = nullptr);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              BuildArena* scratch = nullptr);

  // One 1-bit, crop-fitted BMP per target from a SINGLE decode (the Lyra carousel's two cover
  // thumbnails): the DCT pre-scale is the one the largest target needs, and every target gets its
  // own resampler, ditherer and BMP stream fed from the same source rows, so each is still dithered
  // once from gray -- never rescaled after dithering. The largest target's BMP is byte-identical to
  // what jpegFileTo1BitBmpStreamWithSize writes for it alone. False when any output failed.
  static constexpr int kMaxTargets = 2;
  struct BmpTarget {
    Print* out;
    int maxWidth;
    int maxHeight;
  };
  static bool jpegFileTo1BitBmpStreamsWithSizes(FsFile& jpegFile, const BmpTarget* targets, int count,
                                                BuildArena* scratch = nullptr);
};
