// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/editor/ref_cel_handles.h"

#include "app/tools/ink.h"
#include "app/ui/editor/editor.h"
#include "app/ui/skin/skin_theme.h"
#include "doc/cel.h"
#include "doc/cel_data.h"
#include "doc/layer.h"
#include "os/surface.h"
#include "ui/graphics.h"

#include <algorithm>
#include <cmath>

namespace app {

using namespace ui;

namespace {

// Distance (in screen pixels) to grab a handle.
constexpr double kHandleRadius = 6.0;
// Max distance (in screen pixels) from a corner to start a rotation
// (outside the quad).
constexpr double kRotateRadius = 28.0;

double dist(const gfx::PointF& a, const gfx::PointF& b)
{
  return std::hypot(a.x - b.x, a.y - b.y);
}

gfx::PointF midpoint(const gfx::PointF& a, const gfx::PointF& b)
{
  return gfx::PointF((a.x + b.x) / 2.0, (a.y + b.y) / 2.0);
}

bool point_in_quad(const gfx::PointF q[4], const gfx::PointF& pt)
{
  int sign = 0;
  for (int i = 0; i < 4; ++i) {
    const gfx::PointF& a = q[i];
    const gfx::PointF& b = q[(i + 1) % 4];
    const double cross = (b.x - a.x) * (pt.y - a.y) - (b.y - a.y) * (pt.x - a.x);
    const int s = (cross > 0 ? 1 : (cross < 0 ? -1 : 0));
    if (s == 0)
      continue;
    if (sign == 0)
      sign = s;
    else if (s != sign)
      return false;
  }
  return true;
}

// Returns 0..7 for the direction (dx, dy) in screen space: E, SE, S,
// SW, W, NW, N, NE.
int direction_index(double dx, double dy)
{
  constexpr double kPi = 3.14159265358979323846;
  double angle = std::atan2(dy, dx); // -pi..pi, y down
  int i = int(std::floor((angle + kPi / 8.0) / (kPi / 4.0)));
  return (i % 8 + 8) % 8;
}

} // anonymous namespace

doc::Cel* get_transformable_ref_cel(Editor* editor)
{
  if (!editor || !editor->isActive() || !editor->document())
    return nullptr;

  doc::Layer* layer = editor->layer();
  if (!layer || !layer->isReference() || !layer->isVisibleHierarchy() ||
      !layer->isEditableHierarchy() || !layer->isMovable())
    return nullptr;

  tools::Ink* ink = editor->getCurrentEditorInk();
  if (!ink || !ink->isCelMovement())
    return nullptr;

  doc::Cel* cel = layer->cel(editor->frame());
  if (!cel || !cel->image())
    return nullptr;

  return cel;
}

void get_ref_cel_screen_quad(Editor* editor, const doc::Cel* cel, gfx::PointF out[4])
{
  const gfx::PointF mainOffset(editor->mainTilePosition());
  const doc::Quad quad = cel->data()->quad();
  for (int i = 0; i < 4; ++i)
    out[i] = editor->editorToScreenF(quad[i] + mainOffset);
}

gfx::PointF ref_cel_screen_to_sprite(Editor* editor, const gfx::Point& screenPos)
{
  return editor->screenToEditorF(screenPos) - gfx::PointF(editor->mainTilePosition());
}

RefCelHit hit_test_ref_cel(Editor* editor, const doc::Cel* cel, const gfx::Point& screenPos)
{
  RefCelHit hit;
  if (!cel)
    return hit;

  gfx::PointF q[4];
  get_ref_cel_screen_quad(editor, cel, q);
  const gfx::PointF pt(screenPos);

  // Corners first
  for (int i = 0; i < 4; ++i) {
    if (dist(q[i], pt) <= kHandleRadius) {
      hit.type = RefCelHit::Corner;
      hit.index = i;
      return hit;
    }
  }

  // Edge handles (midpoints)
  for (int i = 0; i < 4; ++i) {
    if (dist(midpoint(q[i], q[(i + 1) % 4]), pt) <= kHandleRadius) {
      hit.type = RefCelHit::Edge;
      hit.index = i;
      return hit;
    }
  }

  // Rotation zone: outside the quad, near a corner
  if (!point_in_quad(q, pt)) {
    for (int i = 0; i < 4; ++i) {
      if (dist(q[i], pt) <= kRotateRadius) {
        hit.type = RefCelHit::Rotate;
        hit.index = i;
        return hit;
      }
    }
  }
  return hit;
}

void draw_ref_cel_handles(Editor* editor, ui::Graphics* g, const doc::Cel* cel)
{
  if (!cel)
    return;

  gfx::PointF q[4];
  get_ref_cel_screen_quad(editor, cel, q);

  const gfx::PointF origin(editor->bounds().origin());
  gfx::Point p[4];
  for (int i = 0; i < 4; ++i)
    p[i] = gfx::Point(int(std::round(q[i].x - origin.x)), int(std::round(q[i].y - origin.y)));

  // Outline (dark shadow + light line, visible over any photo)
  for (int i = 0; i < 4; ++i) {
    const gfx::Point& a = p[i];
    const gfx::Point& b = p[(i + 1) % 4];
    g->drawLine(gfx::rgba(0, 0, 0, 160), a + gfx::Point(1, 1), b + gfx::Point(1, 1));
    g->drawLine(gfx::rgba(255, 255, 255, 255), a, b);
  }

  auto theme = skin::SkinTheme::get(editor);
  os::Surface* part = theme->parts.transformationHandle()->bitmap(0);
  const int hw = part->width() / 2;
  const int hh = part->height() / 2;

  for (int i = 0; i < 4; ++i) {
    // Corner handle
    g->drawRgbaSurface(part, p[i].x - hw, p[i].y - hh);

    // Edge handle (midpoint)
    const gfx::Point& a = p[i];
    const gfx::Point& b = p[(i + 1) % 4];
    const gfx::Point m((a.x + b.x) / 2, (a.y + b.y) / 2);
    g->drawRgbaSurface(part, m.x - hw, m.y - hh);
  }
}

gfx::Rect get_ref_cel_handles_bounds(Editor* editor, const doc::Cel* cel)
{
  if (!cel)
    return gfx::Rect();

  gfx::PointF q[4];
  get_ref_cel_screen_quad(editor, cel, q);
  double x1 = q[0].x, y1 = q[0].y, x2 = x1, y2 = y1;
  for (int i = 1; i < 4; ++i) {
    x1 = std::min<double>(x1, q[i].x);
    y1 = std::min<double>(y1, q[i].y);
    x2 = std::max<double>(x2, q[i].x);
    y2 = std::max<double>(y2, q[i].y);
  }
  const int m = 16; // handle size + shadow margin
  return gfx::Rect(int(std::floor(x1)) - m,
                   int(std::floor(y1)) - m,
                   int(std::ceil(x2 - x1)) + 2 * m + 1,
                   int(std::ceil(y2 - y1)) + 2 * m + 1);
}

bool set_ref_cel_cursor(Editor* editor, const doc::Cel* cel, const RefCelHit& hit)
{
  if (!cel || hit.type == RefCelHit::None)
    return false;

  gfx::PointF q[4];
  get_ref_cel_screen_quad(editor, cel, q);
  const gfx::PointF center((q[0].x + q[1].x + q[2].x + q[3].x) / 4.0,
                           (q[0].y + q[1].y + q[2].y + q[3].y) / 4.0);

  gfx::PointF handlePos;
  if (hit.type == RefCelHit::Edge)
    handlePos = midpoint(q[hit.index], q[(hit.index + 1) % 4]);
  else
    handlePos = q[hit.index];

  const int dir = direction_index(handlePos.x - center.x, handlePos.y - center.y);

  if (hit.type == RefCelHit::Rotate) {
    auto theme = skin::SkinTheme::get(editor);
    const Cursor* cursors[8] = { theme->cursors.rotateE(),  theme->cursors.rotateSe(),
                                 theme->cursors.rotateS(),  theme->cursors.rotateSw(),
                                 theme->cursors.rotateW(),  theme->cursors.rotateNw(),
                                 theme->cursors.rotateN(),  theme->cursors.rotateNe() };
    editor->showMouseCursor(kCustomCursor, cursors[dir]);
  }
  else {
    const CursorType cursors[8] = { kSizeECursor, kSizeSECursor, kSizeSCursor, kSizeSWCursor,
                                    kSizeWCursor, kSizeNWCursor, kSizeNCursor, kSizeNECursor };
    editor->showMouseCursor(cursors[dir]);
  }
  return true;
}

} // namespace app
