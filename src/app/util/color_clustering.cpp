// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/util/color_clustering.h"

#include "doc/cel.h"
#include "doc/cel_data.h"
#include "doc/image.h"
#include "doc/palette.h"
#include "doc/primitives.h"
#include "doc/quad.h"
#include "doc/sprite.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace app {

using namespace doc;

namespace {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<std::array<double, 3>, 3>;

Vec3 mul(const Mat3& m, const Vec3& v)
{
  return { m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
           m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
           m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] };
}

Mat3 inverse(const Mat3& m)
{
  const double a = m[0][0], b = m[0][1], c = m[0][2];
  const double d = m[1][0], e = m[1][1], f = m[1][2];
  const double g = m[2][0], h = m[2][1], i = m[2][2];
  const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const double det = a * A + b * B + c * C;
  Mat3 r;
  r[0] = { A / det, -(b * i - c * h) / det, (b * f - c * e) / det };
  r[1] = { B / det, (a * i - c * g) / det, -(a * f - c * d) / det };
  r[2] = { C / det, -(a * h - b * g) / det, (a * e - b * d) / det };
  return r;
}

//////////////////////////////////////////////////////////////////////
// Color spaces (all take/give sRGB in 0..1)

double to_linear(double v)
{
  return (v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
}

double from_linear(double v)
{
  v = std::clamp(v, 0.0, 1.0);
  return (v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
}

Vec3 to_linear(const Vec3& c)
{
  return { to_linear(c[0]), to_linear(c[1]), to_linear(c[2]) };
}

Vec3 from_linear(const Vec3& c)
{
  return { from_linear(c[0]), from_linear(c[1]), from_linear(c[2]) };
}

// The HSV cone: hue is the angle, saturation * value the distance from
// the axis, value the height.
Vec3 to_hsv_cone(const Vec3& c)
{
  const double r = c[0], g = c[1], b = c[2];
  const double v = std::max({ r, g, b });
  const double delta = v - std::min({ r, g, b });
  const double s = (v > 0.0 ? delta / v : 0.0);
  double h = 0.0;
  if (delta > 0.0) {
    if (v == r)
      h = std::fmod((g - b) / delta, 6.0);
    else if (v == g)
      h = (b - r) / delta + 2.0;
    else
      h = (r - g) / delta + 4.0;
    if (h < 0.0)
      h += 6.0;
    h /= 6.0;
  }
  const double angle = h * 2.0 * 3.14159265358979323846;
  const double radius = s * v;
  return { radius * std::cos(angle), radius * std::sin(angle), v };
}

// Linear sRGB -> XYZ derived from the sRGB primaries and the D65 white
// point (so RGB white falls exactly on the Lab reference white).
struct LabConstants {
  Mat3 rgbToXyz;
  Mat3 xyzToRgb;
  Vec3 white;

  LabConstants()
  {
    auto xyz_of = [](double x, double y) -> Vec3 { return { x / y, 1.0, (1.0 - x - y) / y }; };
    const Vec3 r = xyz_of(0.64, 0.33), g = xyz_of(0.30, 0.60), b = xyz_of(0.15, 0.06);
    Mat3 p;
    for (int i = 0; i < 3; ++i)
      p[i] = { r[i], g[i], b[i] };
    const Vec3 s = mul(inverse(p), xyz_of(0.3127, 0.3290));
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        rgbToXyz[i][j] = p[i][j] * s[j];
    xyzToRgb = inverse(rgbToXyz);
    for (int i = 0; i < 3; ++i)
      white[i] = rgbToXyz[i][0] + rgbToXyz[i][1] + rgbToXyz[i][2];
  }
};

const LabConstants& lab_constants()
{
  static const LabConstants k;
  return k;
}

Vec3 to_lab(const Vec3& c)
{
  const LabConstants& k = lab_constants();
  Vec3 xyz = mul(k.rgbToXyz, to_linear(c));
  constexpr double e = 6.0 / 29.0;
  Vec3 f;
  for (int i = 0; i < 3; ++i) {
    const double t = xyz[i] / k.white[i];
    f[i] = (t > e * e * e ? std::cbrt(t) : t / (3.0 * e * e) + 4.0 / 29.0);
  }
  Vec3 lab = { 116.0 * f[1] - 16.0, 500.0 * (f[0] - f[1]), 200.0 * (f[1] - f[2]) };
  if (c[0] == c[1] && c[1] == c[2])
    lab[1] = lab[2] = 0.0;
  return lab;
}

Vec3 from_lab(const Vec3& lab)
{
  const LabConstants& k = lab_constants();
  constexpr double e = 6.0 / 29.0;
  const double fy = (lab[0] + 16.0) / 116.0;
  const Vec3 f = { fy + lab[1] / 500.0, fy, fy - lab[2] / 200.0 };
  Vec3 xyz;
  for (int i = 0; i < 3; ++i)
    xyz[i] = (f[i] > e ? f[i] * f[i] * f[i] : 3.0 * e * e * (f[i] - 4.0 / 29.0)) * k.white[i];
  return from_linear(mul(k.xyzToRgb, xyz));
}

const Mat3 kOkM1 = { { { 0.4122214708, 0.5363325363, 0.0514459929 },
                       { 0.2119034982, 0.6806995451, 0.1073969566 },
                       { 0.0883024619, 0.2817188376, 0.6299787005 } } };
const Mat3 kOkM2 = { { { 0.2104542553, 0.7936177850, -0.0040720468 },
                       { 1.9779984951, -2.4285922050, 0.4505937099 },
                       { 0.0259040371, 0.7827717662, -0.8086757660 } } };

Vec3 to_oklab(const Vec3& c)
{
  Vec3 lms = mul(kOkM1, to_linear(c));
  for (double& v : lms)
    v = std::cbrt(v);
  return mul(kOkM2, lms);
}

Vec3 from_oklab(const Vec3& lab)
{
  static const Mat3 invM1 = inverse(kOkM1);
  static const Mat3 invM2 = inverse(kOkM2);
  Vec3 lms = mul(invM2, lab);
  for (double& v : lms)
    v = v * v * v;
  return from_linear(mul(invM1, lms));
}

Vec3 to_cluster_space(const Vec3& c, ClusterSpace space)
{
  switch (space) {
    case ClusterSpace::Oklab:   return to_oklab(c);
    case ClusterSpace::Lab:     return to_lab(c);
    case ClusterSpace::HsvCone: return to_hsv_cone(c);
    case ClusterSpace::Linear:  return to_linear(c);
    case ClusterSpace::Rgb:
    default:                    return c;
  }
}

Vec3 to_average_space(const Vec3& c, ClusterAverage avg)
{
  switch (avg) {
    case ClusterAverage::Linear: return to_linear(c);
    case ClusterAverage::Oklab:  return to_oklab(c);
    case ClusterAverage::Lab:    return to_lab(c);
    case ClusterAverage::Rgb:
    default:                     return c;
  }
}

Vec3 from_average_space(const Vec3& c, ClusterAverage avg)
{
  switch (avg) {
    case ClusterAverage::Linear: return from_linear(c);
    case ClusterAverage::Oklab:  return from_oklab(c);
    case ClusterAverage::Lab:    return from_lab(c);
    case ClusterAverage::Rgb:
    default:                     return c;
  }
}

Vec3 unpack(uint32_t c)
{
  return { ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0 };
}

color_t as_rgba(const Vec3& c)
{
  auto byte = [](double v) { return int(std::round(std::clamp(v, 0.0, 1.0) * 255.0)); };
  return rgba(byte(c[0]), byte(c[1]), byte(c[2]), 255);
}

double dist2(const Vec3& a, const Vec3& b)
{
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return x * x + y * y + z * z;
}

// Runs fn(begin, end, chunkIndex) on several threads (one chunk each).
template<typename Fn>
void parallel_chunks(const size_t n, const int nchunks, Fn fn)
{
  if (nchunks <= 1) {
    fn(size_t(0), n, 0);
    return;
  }
  std::vector<std::thread> threads;
  threads.reserve(nchunks);
  for (int t = 0; t < nchunks; ++t) {
    const size_t begin = n * t / nchunks;
    const size_t end = n * (t + 1) / nchunks;
    threads.emplace_back([&fn, begin, end, t] { fn(begin, end, t); });
  }
  for (auto& th : threads)
    th.join();
}

// Picks an index with a probability proportional to its weight.
size_t weighted_pick(const std::vector<double>& w, const double total, std::mt19937_64& rng)
{
  std::uniform_real_distribution<double> dist(0.0, total);
  double u = dist(rng);
  for (size_t i = 0; i < w.size(); ++i) {
    u -= w[i];
    if (u < 0.0)
      return i;
  }
  // Rounding errors: the last one with weight
  for (size_t i = w.size(); i-- > 0;)
    if (w[i] > 0.0)
      return i;
  return 0;
}

// Weighted k-means with a k-means++ start. Returns the cluster of
// every point, numbered 0.. without gaps.
std::vector<int> kmeans(const std::vector<Vec3>& points,
                        const std::vector<double>& weights,
                        const int k,
                        const int rounds)
{
  const size_t n = points.size();
  std::mt19937_64 rng(0);

  const int nthreads = (n < 20000 ? 1 :
                                    std::clamp(int(std::thread::hardware_concurrency()), 1, 32));

  double wsum = 0.0;
  for (double w : weights)
    wsum += w;

  std::vector<Vec3> centres;
  centres.push_back(points[weighted_pick(weights, wsum, rng)]);

  std::vector<double> nearest(n);
  std::vector<double> far(n);
  for (size_t i = 0; i < n; ++i)
    nearest[i] = dist2(points[i], centres[0]);

  while (int(centres.size()) < k) {
    double farSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      far[i] = nearest[i] * weights[i];
      farSum += far[i];
    }
    if (farSum <= 0.0) // Fewer different points than k
      break;
    centres.push_back(points[weighted_pick(far, farSum, rng)]);
    const Vec3& c = centres.back();
    parallel_chunks(n, nthreads, [&](size_t b, size_t e, int) {
      for (size_t i = b; i < e; ++i)
        nearest[i] = std::min(nearest[i], dist2(points[i], c));
    });
  }

  const int kk = int(centres.size());
  std::vector<int> cluster(n, -1);

  struct Partial {
    std::vector<Vec3> sum;
    std::vector<double> w;
    bool changed = false;
  };
  std::vector<Partial> partials(nthreads);

  for (int round = 0; round < rounds; ++round) {
    parallel_chunks(n, nthreads, [&](size_t b, size_t e, int t) {
      Partial& p = partials[t];
      p.sum.assign(kk, Vec3{ 0.0, 0.0, 0.0 });
      p.w.assign(kk, 0.0);
      p.changed = false;
      for (size_t i = b; i < e; ++i) {
        int best = 0;
        double bestD = dist2(points[i], centres[0]);
        for (int c = 1; c < kk; ++c) {
          const double d = dist2(points[i], centres[c]);
          if (d < bestD) {
            bestD = d;
            best = c;
          }
        }
        if (cluster[i] != best) {
          cluster[i] = best;
          p.changed = true;
        }
        const double w = weights[i];
        for (int j = 0; j < 3; ++j)
          p.sum[best][j] += points[i][j] * w;
        p.w[best] += w;
      }
    });

    bool changed = false;
    for (const Partial& p : partials)
      changed |= p.changed;
    if (!changed)
      break;

    for (int c = 0; c < kk; ++c) {
      Vec3 s = { 0.0, 0.0, 0.0 };
      double w = 0.0;
      for (const Partial& p : partials) {
        for (int j = 0; j < 3; ++j)
          s[j] += p.sum[c][j];
        w += p.w[c];
      }
      if (w > 0.0)
        centres[c] = { s[0] / w, s[1] / w, s[2] / w };
    }
  }

  // Renumber without gaps (empty clusters are dropped)
  std::vector<int> renum(kk, -1);
  std::vector<bool> used(kk, false);
  for (int c : cluster)
    used[c] = true;
  int next = 0;
  for (int c = 0; c < kk; ++c)
    if (used[c])
      renum[c] = next++;
  for (int& c : cluster)
    c = renum[c];
  return cluster;
}

color_t pixel_to_rgba(const Image* image, const Palette* pal, const color_t c)
{
  switch (image->pixelFormat()) {
    case IMAGE_RGB: return c;
    case IMAGE_GRAYSCALE: {
      const int v = graya_getv(c);
      return rgba(v, v, v, graya_geta(c));
    }
    case IMAGE_INDEXED:
      if (int(c) == image->maskColor() || !pal)
        return 0;
      return pal->getEntry(c);
  }
  return 0;
}

} // anonymous namespace

ColorHistogram collect_reference_cel_colors(const Cel* cel)
{
  ColorHistogram hist;
  if (!cel || !cel->image())
    return hist;

  const Image* image = cel->image();
  const Sprite* sprite = cel->sprite();
  const Palette* pal = sprite->palette(cel->frame());
  const int w = image->width();
  const int h = image->height();
  const double sw = sprite->width();
  const double sh = sprite->height();
  if (w < 1 || h < 1)
    return hist;

  const Homography H = Homography::imageToQuad(cel->data()->quad(), w, h);

  // Counts of every 24-bit color
  std::vector<uint32_t> counts(1 << 24, 0);
  uint64_t total = 0;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double sx, sy;
      if (!H.apply(x + 0.5, y + 0.5, sx, sy))
        continue;
      if (sx < 0.0 || sy < 0.0 || sx >= sw || sy >= sh)
        continue;
      const color_t c = pixel_to_rgba(image, pal, get_pixel(image, x, y));
      if (rgba_geta(c) == 0)
        continue;
      ++counts[c & 0xffffff];
      ++total;
    }
  }

  for (uint32_t c = 0; c < counts.size(); ++c) {
    if (counts[c]) {
      // color_t is 0xAABBGGRR, store as 0xRRGGBB
      hist.colors.push_back(((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff));
      hist.counts.push_back(counts[c]);
    }
  }
  hist.totalPixels = total;
  return hist;
}

std::vector<ColorCluster> cluster_colors(const ColorHistogram& hist, const ClusterOptions& opts)
{
  std::vector<ColorCluster> result;
  if (hist.empty() || opts.k < 1)
    return result;

  const size_t ncolors = hist.colors.size();

  // Merge similar colors in bins (precision < 8 bits per channel). The
  // point of a bin is the average of its colors (by pixels).
  const int shift = 8 - std::clamp(opts.precision, 1, 8);
  std::vector<uint32_t> binOf(ncolors);
  std::vector<Vec3> binSum;
  std::vector<double> binPixels;
  std::vector<int> binColors;
  {
    std::unordered_map<uint32_t, uint32_t> bins;
    bins.reserve(ncolors);
    for (size_t i = 0; i < ncolors; ++i) {
      const uint32_t c = hist.colors[i];
      const uint32_t key = ((((c >> 16) & 255) >> shift) << 16) |
                           ((((c >> 8) & 255) >> shift) << 8) | ((c & 255) >> shift);
      auto it = bins.find(key);
      uint32_t b;
      if (it == bins.end()) {
        b = uint32_t(binSum.size());
        bins[key] = b;
        binSum.push_back({ 0.0, 0.0, 0.0 });
        binPixels.push_back(0.0);
        binColors.push_back(0);
      }
      else
        b = it->second;
      binOf[i] = b;
      const Vec3 v = unpack(c);
      const double n = hist.counts[i];
      for (int j = 0; j < 3; ++j)
        binSum[b][j] += v[j] * n;
      binPixels[b] += n;
      binColors[b] += 1;
    }
  }

  const size_t nbins = binSum.size();
  std::vector<Vec3> points(nbins);
  std::vector<double> weights(nbins);
  for (size_t b = 0; b < nbins; ++b) {
    const Vec3 rgb = { binSum[b][0] / binPixels[b],
                       binSum[b][1] / binPixels[b],
                       binSum[b][2] / binPixels[b] };
    points[b] = to_cluster_space(rgb, opts.space);
    switch (opts.weight) {
      case ClusterWeight::Pixels: weights[b] = binPixels[b]; break;
      case ClusterWeight::Sqrt:   weights[b] = std::sqrt(binPixels[b]); break;
      case ClusterWeight::One:    weights[b] = binColors[b]; break;
    }
  }

  const std::vector<int> binCluster =
    kmeans(points, weights, std::min<int>(opts.k, int(nbins)), opts.maxRounds);
  int nclusters = 0;
  for (int c : binCluster)
    nclusters = std::max(nclusters, c + 1);

  // The color of a cluster is the average of the pixels it covers (in
  // the average space).
  struct Acc {
    int colors = 0;
    uint64_t pixels = 0;
    Vec3 sum = { 0.0, 0.0, 0.0 };
    color_t mean = 0;
  };
  std::vector<Acc> acc(nclusters);
  for (size_t i = 0; i < ncolors; ++i) {
    Acc& a = acc[binCluster[binOf[i]]];
    const double n = hist.counts[i];
    const Vec3 v = to_average_space(unpack(hist.colors[i]), opts.average);
    for (int j = 0; j < 3; ++j)
      a.sum[j] += v[j] * n;
    a.pixels += hist.counts[i];
    a.colors += 1;
  }
  for (Acc& a : acc) {
    const double n = double(a.pixels);
    a.mean = as_rgba(
      from_average_space({ a.sum[0] / n, a.sum[1] / n, a.sum[2] / n }, opts.average));
  }

  std::sort(acc.begin(), acc.end(), [](const Acc& a, const Acc& b) {
    if (a.pixels != b.pixels)
      return a.pixels > b.pixels;
    if (a.colors != b.colors)
      return a.colors > b.colors;
    const auto key = [](color_t c) {
      return std::make_tuple(rgba_getr(c), rgba_getg(c), rgba_getb(c));
    };
    return key(a.mean) < key(b.mean);
  });

  uint64_t total = 0;
  Vec3 mixed = { 0.0, 0.0, 0.0 };
  for (const Acc& a : acc) {
    total += a.pixels;
    for (int j = 0; j < 3; ++j)
      mixed[j] += a.sum[j];
    ColorCluster c;
    c.colors = a.colors;
    c.pixels = a.pixels;
    c.mean = a.mean;
    c.cumulative = as_rgba(from_average_space(
      { mixed[0] / double(total), mixed[1] / double(total), mixed[2] / double(total) },
      opts.average));
    c.cumulativePixels = total;
    result.push_back(c);
  }
  return result;
}

} // namespace app
