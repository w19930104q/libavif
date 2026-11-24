// Copyright 2023 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>
#include <limits>

#include "avif/avif.h"
#include "aviftest_helpers.h"
#include "gtest/gtest.h"

namespace avif {
namespace {

TEST(AvifImageTest, CreateEmpty) {
  ImagePtr empty(avifImageCreateEmpty());
  EXPECT_NE(empty, nullptr);
}

bool IsValidAvifImageCreate(uint32_t width, uint32_t height, uint32_t depth,
                            avifPixelFormat format) {
  ImagePtr image(avifImageCreate(width, height, depth, format));
  return image != nullptr;
}

TEST(AvifImageTest, Create) {
  EXPECT_TRUE(IsValidAvifImageCreate(0, 0, 0, AVIF_PIXEL_FORMAT_NONE));
  EXPECT_TRUE(
      IsValidAvifImageCreate(1, 1, /*depth=*/1, AVIF_PIXEL_FORMAT_NONE));
  EXPECT_TRUE(
      IsValidAvifImageCreate(64, 64, /*depth=*/8, AVIF_PIXEL_FORMAT_NONE));
  EXPECT_TRUE(IsValidAvifImageCreate(std::numeric_limits<uint32_t>::max(),
                                     std::numeric_limits<uint32_t>::max(),
                                     /*depth=*/16, AVIF_PIXEL_FORMAT_NONE));
}

TEST(AvifImageTest, Invalid) {
  EXPECT_FALSE(IsValidAvifImageCreate(0, 0, 0, AVIF_PIXEL_FORMAT_COUNT));
  EXPECT_FALSE(
      IsValidAvifImageCreate(0, 0, /*depth=*/17, AVIF_PIXEL_FORMAT_YUV400));
}

TEST(AvifImageTest, WriteImage) {
  ImagePtr image =
      testutil::CreateImage(/*width=*/12, /*height=*/34, /*depth=*/10,
                            AVIF_PIXEL_FORMAT_YUV444, AVIF_PLANES_ALL);
  ASSERT_NE(image, nullptr);
  testutil::FillImageGradient(image.get());
  ASSERT_TRUE(testutil::WriteImage(
      image.get(), (testing::TempDir() + "/avifimagetest.png").c_str()));
}

// A failed copy must leave the destination image untouched: the source
// structure has to be validated and the new planes fully prepared before any
// destination plane is replaced.
TEST(AvifImageTest, CopyIncompleteSourceKeepsDestination) {
  // Destination: an existing 1x1 RGBA image produced through an RGB
  // conversion, with an alpha plane.
  ImagePtr dst =
      testutil::CreateImage(/*width=*/1, /*height=*/1, /*depth=*/8,
                            AVIF_PIXEL_FORMAT_YUV444, AVIF_PLANES_ALL);
  ASSERT_NE(dst, nullptr);
  {
    testutil::AvifRgbImage rgb(dst.get(), /*rgbDepth=*/8,
                               AVIF_RGB_FORMAT_RGBA);
    rgb.pixels[0] = 10;  // R
    rgb.pixels[1] = 20;  // G
    rgb.pixels[2] = 30;  // B
    rgb.pixels[3] = 40;  // A
    ASSERT_EQ(avifImageRGBToYUV(dst.get(), &rgb), AVIF_RESULT_OK);
  }

  // Snapshot the destination state before the failed copy.
  const uint32_t dstWidth = dst->width;
  const uint32_t dstHeight = dst->height;
  const uint8_t* const dstAlphaBefore = dst->alphaPlane;
  const uint8_t dstAlphaValueBefore = *dst->alphaPlane;
  const uint8_t dstYValueBefore = *dst->yuvPlanes[AVIF_CHAN_Y];
  const uint8_t dstUValueBefore = *dst->yuvPlanes[AVIF_CHAN_U];
  const uint8_t dstVValueBefore = *dst->yuvPlanes[AVIF_CHAN_V];

  // Source: claims 4:2:0 but only the Y plane is allocated; U and V planes
  // are missing, which makes its structure incomplete.
  ImagePtr src(avifImageCreate(/*width=*/1, /*height=*/1, /*depth=*/8,
                               AVIF_PIXEL_FORMAT_YUV420));
  ASSERT_NE(src, nullptr);
  ASSERT_EQ(avifImageAllocatePlanes(src.get(), AVIF_PLANES_YUV),
            AVIF_RESULT_OK);
  // Simulate a corrupted/partial image: keep only the Y plane.
  avifFree(src->yuvPlanes[AVIF_CHAN_U]);
  avifFree(src->yuvPlanes[AVIF_CHAN_V]);
  src->yuvPlanes[AVIF_CHAN_U] = nullptr;
  src->yuvPlanes[AVIF_CHAN_V] = nullptr;
  src->yuvRowBytes[AVIF_CHAN_U] = 0;
  src->yuvRowBytes[AVIF_CHAN_V] = 0;
  src->imageOwnsYUVPlanes = AVIF_TRUE;  // only the remaining Y plane is owned
  *src->yuvPlanes[AVIF_CHAN_Y] = 255;

  // One full-plane copy of the malformed source must fail with an argument
  // error.
  ASSERT_EQ(avifImageCopy(dst.get(), src.get(), AVIF_PLANES_ALL),
            AVIF_RESULT_INVALID_ARGUMENT);

  // The destination image is preserved exactly as it was before the copy:
  // dimensions, alpha plane storage/value and visible pixel values.
  EXPECT_EQ(dst->width, dstWidth);
  EXPECT_EQ(dst->height, dstHeight);
  ASSERT_NE(dst->alphaPlane, nullptr);
  EXPECT_EQ(dst->alphaPlane, dstAlphaBefore);
  EXPECT_EQ(*dst->alphaPlane, dstAlphaValueBefore);
  ASSERT_NE(dst->yuvPlanes[AVIF_CHAN_Y], nullptr);
  ASSERT_NE(dst->yuvPlanes[AVIF_CHAN_U], nullptr);
  ASSERT_NE(dst->yuvPlanes[AVIF_CHAN_V], nullptr);
  EXPECT_EQ(*dst->yuvPlanes[AVIF_CHAN_Y], dstYValueBefore);
  EXPECT_EQ(*dst->yuvPlanes[AVIF_CHAN_U], dstUValueBefore);
  EXPECT_EQ(*dst->yuvPlanes[AVIF_CHAN_V], dstVValueBefore);

  // The destination must still be fully usable (e.g. another valid copy).
  ImagePtr validSrc =
      testutil::CreateImage(/*width=*/1, /*height=*/1, /*depth=*/8,
                            AVIF_PIXEL_FORMAT_YUV444, AVIF_PLANES_ALL);
  ASSERT_NE(validSrc, nullptr);
  testutil::FillImageGradient(validSrc.get());
  ASSERT_EQ(avifImageCopy(dst.get(), validSrc.get(), AVIF_PLANES_ALL),
            AVIF_RESULT_OK);
  EXPECT_TRUE(testutil::AreImagesEqual(*dst, *validSrc));
}

// A successful copy gives the destination its own independent plane storage:
// mutating the source afterwards must not affect the destination (deep copy).
TEST(AvifImageTest, CopySucceedsWithIndependentPlanes) {
  ImagePtr src =
      testutil::CreateImage(/*width=*/4, /*height=*/3, /*depth=*/8,
                            AVIF_PIXEL_FORMAT_YUV420, AVIF_PLANES_ALL);
  ASSERT_NE(src, nullptr);
  testutil::FillImageGradient(src.get());
  ImagePtr dst(avifImageCreateEmpty());
  ASSERT_NE(dst, nullptr);

  ASSERT_EQ(avifImageCopy(dst.get(), src.get(), AVIF_PLANES_ALL),
            AVIF_RESULT_OK);

  // The planes must be distinct allocations.
  for (int c = AVIF_CHAN_Y; c <= AVIF_CHAN_V; ++c) {
    ASSERT_NE(dst->yuvPlanes[c], nullptr);
    EXPECT_NE(dst->yuvPlanes[c], src->yuvPlanes[c]);
  }
  ASSERT_NE(dst->alphaPlane, nullptr);
  EXPECT_NE(dst->alphaPlane, src->alphaPlane);
  EXPECT_TRUE(testutil::AreImagesEqual(*dst, *src));

  // Mutating the source does not reach the destination.
  const uint8_t dstYBefore = *dst->yuvPlanes[AVIF_CHAN_Y];
  const uint8_t dstAlphaBefore = *dst->alphaPlane;
  *src->yuvPlanes[AVIF_CHAN_Y] = static_cast<uint8_t>(dstYBefore + 1);
  *src->alphaPlane = static_cast<uint8_t>(dstAlphaBefore + 1);
  EXPECT_EQ(*dst->yuvPlanes[AVIF_CHAN_Y], dstYBefore);
  EXPECT_EQ(*dst->alphaPlane, dstAlphaBefore);
}

}  // namespace
}  // namespace avif
