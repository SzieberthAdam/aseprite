// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UTIL_COLOR_CLUSTERING_H_INCLUDED
#define APP_UTIL_COLOR_CLUSTERING_H_INCLUDED
#pragma once

#include "doc/color.h"

#include <cstdint>
#include <vector>

namespace doc {
class Cel;
}

namespace app {

// Color clustering (a port of imgclust): weighted k-means over the
// different colors of a picture, with a fixed start (k-means++ with a
// seeded generator), so the same input and options give always the
// same palette.

// The space the colors are clustered in.
enum class ClusterSpace { Oklab, Lab, HsvCone, Rgb, Linear };

// How much a color counts in the clustering.
enum class ClusterWeight {
  Pixels, // By the number of its pixels
  Sqrt,   // By the square root of it (accents get a cluster easier)
  One,    // Every different color counts once
};

// The space the colors of a cluster are averaged in.
enum class ClusterAverage { Rgb, Linear, Oklab, Lab };

struct ClusterOptions {
  int k = 8;
  ClusterSpace space = ClusterSpace::Oklab;
  ClusterWeight weight = ClusterWeight::Sqrt;
  ClusterAverage average = ClusterAverage::Rgb;
  // Bits per channel used to merge similar colors before the
  // clustering (8 = every different color is a point, as imgclust).
  // The colors of the clusters are always averaged from the original
  // pixels, so 6 bits gives the same palette quality much faster.
  int precision = 6;
  int maxRounds = 100;
};

// The different (opaque enough) colors of a picture and their pixel
// counts.
struct ColorHistogram {
  std::vector<uint32_t> colors; // 0xRRGGBB
  std::vector<uint32_t> counts;
  uint64_t totalPixels = 0;

  bool empty() const { return colors.empty(); }
};

struct ColorCluster {
  int colors = 0;        // Number of different colors
  uint64_t pixels = 0;   // Number of pixels
  doc::color_t mean;     // Average color of its pixels (rgba)
  doc::color_t cumulative; // Average of this and all the bigger clusters
  uint64_t cumulativePixels = 0;
};

// Collects the colors of the pixels of the reference cel image which
// are visible inside the sprite canvas (after the cel
// transformation). Fully transparent pixels are left out.
ColorHistogram collect_reference_cel_colors(const doc::Cel* cel);

// Clusters the colors. Returns the clusters from the one with the most
// pixels down.
std::vector<ColorCluster> cluster_colors(const ColorHistogram& histogram,
                                         const ClusterOptions& options);

} // namespace app

#endif
