// Aseprite Document Library
// Copyright (C) 2026  Igara Studio S.A.
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef DOC_QUAD_H_INCLUDED
#define DOC_QUAD_H_INCLUDED
#pragma once

#include "gfx/point.h"
#include "gfx/rect.h"

#include <algorithm>
#include <cmath>

namespace doc {

// A quadrilateral used to map a reference cel image with a
// perspective (projective) transformation. Corners are in sprite
// coordinates and correspond to the image corners in this order:
// top-left, top-right, bottom-right, bottom-left.
struct Quad {
  gfx::PointF p[4];

  Quad() {}
  explicit Quad(const gfx::RectF& rc)
  {
    p[0] = gfx::PointF(rc.x, rc.y);
    p[1] = gfx::PointF(rc.x2(), rc.y);
    p[2] = gfx::PointF(rc.x2(), rc.y2());
    p[3] = gfx::PointF(rc.x, rc.y2());
  }

  gfx::PointF& operator[](int i) { return p[i]; }
  const gfx::PointF& operator[](int i) const { return p[i]; }

  bool operator==(const Quad& o) const
  {
    for (int i = 0; i < 4; ++i)
      if (p[i] != o.p[i])
        return false;
    return true;
  }
  bool operator!=(const Quad& o) const { return !operator==(o); }

  gfx::PointF center() const
  {
    return gfx::PointF((p[0].x + p[1].x + p[2].x + p[3].x) / 4.0,
                       (p[0].y + p[1].y + p[2].y + p[3].y) / 4.0);
  }

  gfx::RectF bounds() const
  {
    double x1 = p[0].x, y1 = p[0].y, x2 = x1, y2 = y1;
    for (int i = 1; i < 4; ++i) {
      x1 = std::min<double>(x1, p[i].x);
      y1 = std::min<double>(y1, p[i].y);
      x2 = std::max<double>(x2, p[i].x);
      y2 = std::max<double>(y2, p[i].y);
    }
    return gfx::RectF(x1, y1, x2 - x1, y2 - y1);
  }

  // True if the quad is an axis-aligned rectangle with the same
  // orientation as its bounds (i.e. it can be represented with
  // boundsF only).
  bool isAxisAlignedRect(double eps = 1e-6) const
  {
    const gfx::RectF b = bounds();
    const Quad r(b);
    for (int i = 0; i < 4; ++i)
      if (std::fabs(p[i].x - r.p[i].x) > eps || std::fabs(p[i].y - r.p[i].y) > eps)
        return false;
    return true;
  }

  // True if the quad is convex and non-degenerate (all corners turn
  // to the same side), which is required for a valid perspective
  // mapping.
  bool isConvex() const
  {
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
      const gfx::PointF& a = p[i];
      const gfx::PointF& b = p[(i + 1) % 4];
      const gfx::PointF& c = p[(i + 2) % 4];
      const double cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
      if (std::fabs(cross) < 1e-9)
        return false;
      const int s = (cross > 0 ? 1 : -1);
      if (sign == 0)
        sign = s;
      else if (s != sign)
        return false;
    }
    return true;
  }

  // Maps the quad from one rectangle to another (used when the
  // bounds of a transformed cel are moved or scaled).
  Quad mapRect(const gfx::RectF& from, const gfx::RectF& to) const
  {
    const double sx = (from.w != 0.0 ? to.w / from.w : 1.0);
    const double sy = (from.h != 0.0 ? to.h / from.h : 1.0);
    Quad q;
    for (int i = 0; i < 4; ++i) {
      q.p[i].x = to.x + (p[i].x - from.x) * sx;
      q.p[i].y = to.y + (p[i].y - from.y) * sy;
    }
    return q;
  }
};

// 3x3 projective matrix (row-major).
class Homography {
public:
  Homography()
  {
    for (int i = 0; i < 9; ++i)
      m[i] = (i % 4 == 0 ? 1.0 : 0.0);
  }

  // Maps the unit square (0,0)-(1,1) to the given quad (Heckbert).
  static Homography unitSquareToQuad(const Quad& q)
  {
    Homography h;
    const double x0 = q[0].x, y0 = q[0].y;
    const double x1 = q[1].x, y1 = q[1].y;
    const double x2 = q[2].x, y2 = q[2].y;
    const double x3 = q[3].x, y3 = q[3].y;
    const double sx = x0 - x1 + x2 - x3;
    const double sy = y0 - y1 + y2 - y3;
    double g = 0.0, k = 0.0;
    if (std::fabs(sx) > 1e-12 || std::fabs(sy) > 1e-12) {
      const double dx1 = x1 - x2, dx2 = x3 - x2;
      const double dy1 = y1 - y2, dy2 = y3 - y2;
      const double den = dx1 * dy2 - dx2 * dy1;
      if (std::fabs(den) > 1e-12) {
        g = (sx * dy2 - dx2 * sy) / den;
        k = (dx1 * sy - sx * dy1) / den;
      }
    }
    h.m[0] = x1 - x0 + g * x1;
    h.m[1] = x3 - x0 + k * x3;
    h.m[2] = x0;
    h.m[3] = y1 - y0 + g * y1;
    h.m[4] = y3 - y0 + k * y3;
    h.m[5] = y0;
    h.m[6] = g;
    h.m[7] = k;
    h.m[8] = 1.0;
    return h;
  }

  // Maps image pixel coordinates (0,0)-(w,h) to the quad.
  static Homography imageToQuad(const Quad& q, double w, double h)
  {
    Homography s;
    s.m[0] = (w != 0.0 ? 1.0 / w : 0.0);
    s.m[4] = (h != 0.0 ? 1.0 / h : 0.0);
    return unitSquareToQuad(q) * s;
  }

  // Inverse up to a scale factor (adjugate matrix), normalized so the
  // homogeneous coordinate is positive at the given reference point
  // (e.g. the quad center).
  Homography inverted(const gfx::PointF& ref) const
  {
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[3], e = m[4], f = m[5];
    const double g = m[6], h = m[7], i = m[8];
    Homography r;
    r.m[0] = e * i - f * h;
    r.m[1] = c * h - b * i;
    r.m[2] = b * f - c * e;
    r.m[3] = f * g - d * i;
    r.m[4] = a * i - c * g;
    r.m[5] = c * d - a * f;
    r.m[6] = d * h - e * g;
    r.m[7] = b * g - a * h;
    r.m[8] = a * e - b * d;
    if (r.m[6] * ref.x + r.m[7] * ref.y + r.m[8] < 0.0) {
      for (double& v : r.m)
        v = -v;
    }
    return r;
  }

  // Returns false if the point is mapped to/behind the horizon.
  bool apply(double x, double y, double& outX, double& outY) const
  {
    const double w = m[6] * x + m[7] * y + m[8];
    if (w <= 1e-12)
      return false;
    outX = (m[0] * x + m[1] * y + m[2]) / w;
    outY = (m[3] * x + m[4] * y + m[5]) / w;
    return true;
  }

  Homography operator*(const Homography& o) const
  {
    Homography r;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        r.m[row * 3 + col] = m[row * 3 + 0] * o.m[0 + col] + m[row * 3 + 1] * o.m[3 + col] +
                             m[row * 3 + 2] * o.m[6 + col];
    return r;
  }

  double m[9];
};

// Maps a point in sprite coordinates to image pixel coordinates of an
// image with size (w, h) mapped to the given quad. Returns false if
// the point is not inside the quad.
inline bool quad_point_to_image(const Quad& quad,
                                const double w,
                                const double h,
                                const gfx::PointF& pt,
                                gfx::PointF& out)
{
  if (!quad.isConvex())
    return false;
  const Homography inv = Homography::imageToQuad(quad, w, h).inverted(quad.center());
  double u, v;
  if (!inv.apply(pt.x, pt.y, u, v))
    return false;
  if (u < 0.0 || v < 0.0 || u >= w || v >= h)
    return false;
  out = gfx::PointF(u, v);
  return true;
}

} // namespace doc

#endif
