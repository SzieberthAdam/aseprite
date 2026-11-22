// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/editor/transforming_ref_cel_state.h"

#include "app/cmd/set_cel_quad.h"
#include "app/commands/command.h"
#include "app/console.h"
#include "app/context.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/tx.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_customization_delegate.h"
#include "app/ui/keyboard_shortcuts.h"
#include "app/ui/status_bar.h"
#include "app/ui_context.h"
#include "doc/cel.h"
#include "doc/cel_data.h"
#include "doc/image.h"
#include "fmt/format.h"
#include "ui/message.h"

#include <cmath>

namespace app {

using namespace ui;

namespace {

constexpr double kPi = 3.14159265358979323846;

gfx::PointF rotate_point(const gfx::PointF& p, const gfx::PointF& c, double angle)
{
  const double ca = std::cos(angle);
  const double sa = std::sin(angle);
  const double x = p.x - c.x;
  const double y = p.y - c.y;
  return gfx::PointF(c.x + x * ca - y * sa, c.y + x * sa + y * ca);
}

} // anonymous namespace

TransformingRefCelState::TransformingRefCelState(Editor* editor,
                                                 const MouseMessage* msg,
                                                 doc::Cel* cel,
                                                 const RefCelHit& hit)
  : m_reader(UIContext::instance(), 500)
  , m_editor(editor)
  , m_cel(cel)
  , m_hit(hit)
{
  ContextWriter writer(m_reader);

  m_startHasQuad = m_cel->data()->hasQuad();
  m_startQuad = m_cel->data()->quad();
  m_startBounds = m_cel->boundsF();
  m_quad = m_startQuad;
  m_cursorStart = ref_cel_screen_to_sprite(editor, msg->position());
  m_lastScreenPos = msg->position();

  // Cancel the drag if the user executes a command (undo, etc.)
  m_ctxConn = UIContext::instance()->BeforeCommandExecution.connect(
    &TransformingRefCelState::onBeforeCommandExecution,
    this);

  editor->captureMouse();
}

void TransformingRefCelState::onBeforePopState(Editor* editor)
{
  m_ctxConn.disconnect();
  StandbyState::onBeforePopState(editor);
}

bool TransformingRefCelState::onMouseUp(Editor* editor, MouseMessage* msg)
{
  updateQuad(editor, msg->position(), msg->ctrlPressed());

  const doc::Quad newQuad = m_quad;
  restoreStart();

  if (m_modified && newQuad != m_startQuad) {
    try {
      ContextWriter writer(m_reader, 1000);
      Tx tx(writer, "Transform Reference Layer", ModifyDocument);

      // Store plain axis-aligned rectangles as precise bounds only (so
      // the file stays compatible with Aseprite versions without
      // perspective quads).
      if (newQuad.isAxisAlignedRect())
        tx(new cmd::SetCelQuad(m_cel, newQuad.bounds()));
      else
        tx(new cmd::SetCelQuad(m_cel, newQuad));

      tx.commit();
    }
    catch (const std::exception& ex) {
      Console::showException(ex);
    }
    editor->document()->notifyGeneralUpdate();
  }

  editor->backToPreviousState();
  editor->releaseMouse();
  editor->invalidate();
  return true;
}

bool TransformingRefCelState::onMouseMove(Editor* editor, MouseMessage* msg)
{
  m_lastScreenPos = msg->position();
  updateQuad(editor, msg->position(), msg->ctrlPressed());
  return true;
}

bool TransformingRefCelState::onSetCursor(Editor* editor, const gfx::Point& mouseScreenPos)
{
  if (set_ref_cel_cursor(editor, m_cel, m_hit))
    return true;
  editor->showMouseCursor(kArrowCursor);
  return true;
}

bool TransformingRefCelState::onKeyDown(Editor* editor, KeyMessage* msg)
{
  // Update the transformation when a modifier key is pressed (e.g.
  // Shift to snap the angle) without moving the mouse.
  updateQuad(editor, m_lastScreenPos, msg->ctrlPressed());
  return true;
}

bool TransformingRefCelState::onKeyUp(Editor* editor, KeyMessage* msg)
{
  updateQuad(editor, m_lastScreenPos, msg->ctrlPressed());
  return true;
}

bool TransformingRefCelState::onUpdateStatusBar(Editor* editor)
{
  const gfx::RectF b = m_quad.bounds();
  std::string text;
  switch (m_hit.type) {
    case RefCelHit::Rotate:
      text = fmt::format(":angle: {:.1f}", m_angle * 180.0 / kPi);
      break;
    case RefCelHit::Corner: {
      const gfx::PointF& p = m_quad[m_hit.index];
      text = fmt::format(":pos: {:.2f} {:.2f}  :size: {:.2f} {:.2f}", p.x, p.y, b.w, b.h);
      break;
    }
    default: text = fmt::format(":size: {:.2f} {:.2f}", b.w, b.h); break;
  }
  StatusBar::instance()->setStatusText(0, text);
  return true;
}

void TransformingRefCelState::updateQuad(Editor* editor, const gfx::Point& screenPos, bool ctrl)
{
  const gfx::PointF cursor = ref_cel_screen_to_sprite(editor, screenPos);
  const gfx::PointF delta = cursor - m_cursorStart;
  const doc::Quad& s = m_startQuad;
  doc::Quad q = s;

  KeyAction scaleKeys = KeyAction::None;
  KeyAction rotateKeys = KeyAction::None;
  if (auto* customization = editor->getCustomizationDelegate()) {
    scaleKeys = customization->getPressedKeyAction(KeyContext::ScalingSelection);
    rotateKeys = customization->getPressedKeyAction(KeyContext::RotatingSelection);
  }

  switch (m_hit.type) {
    case RefCelHit::Corner: {
      const int i = m_hit.index;
      if (int(scaleKeys & KeyAction::MaintainAspectRatio)) {
        // Proportional scale from the opposite corner
        const gfx::PointF& o = s[(i + 2) % 4];
        const double vx = s[i].x - o.x;
        const double vy = s[i].y - o.y;
        const double len2 = vx * vx + vy * vy;
        if (len2 > 1e-12) {
          const double f = ((cursor.x - o.x) * vx + (cursor.y - o.y) * vy) / len2;
          if (f > 1e-3) {
            for (int j = 0; j < 4; ++j)
              q[j] = gfx::PointF(o.x + (s[j].x - o.x) * f, o.y + (s[j].y - o.y) * f);
          }
        }
      }
      else {
        // Free corner (perspective/distort)
        q[i] = s[i] + delta;
      }
      break;
    }

    case RefCelHit::Edge: {
      const int i = m_hit.index;
      const int j = (i + 1) % 4;
      gfx::PointF d = delta;
      if (!ctrl) {
        // Stretch: move the edge perpendicular to itself
        const double ex = s[j].x - s[i].x;
        const double ey = s[j].y - s[i].y;
        const double len = std::hypot(ex, ey);
        if (len > 1e-9) {
          const double nx = -ey / len;
          const double ny = ex / len;
          const double t = delta.x * nx + delta.y * ny;
          d = gfx::PointF(nx * t, ny * t);
        }
      }
      q[i] = s[i] + d;
      q[j] = s[j] + d;
      break;
    }

    case RefCelHit::Rotate: {
      const gfx::PointF c = s.center();
      double angle = std::atan2(cursor.y - c.y, cursor.x - c.x) -
                     std::atan2(m_cursorStart.y - c.y, m_cursorStart.x - c.x);
      if (int(rotateKeys & KeyAction::AngleSnap)) {
        const double step = kPi / 12.0; // 15 degrees
        angle = std::round(angle / step) * step;
      }
      for (int k = 0; k < 4; ++k)
        q[k] = rotate_point(s[k], c, angle);
      m_angle = angle;
      break;
    }

    default: return;
  }

  // Ignore invalid (concave/degenerate) shapes, keep the last valid one.
  if (!q.isConvex())
    return;

  if (q != m_quad) {
    m_quad = q;
    m_modified = true;
    m_cel->data()->setQuad(q);
    m_cel->incrementVersion();
    editor->invalidate();
    editor->updateStatusBar();
  }
}

void TransformingRefCelState::restoreStart()
{
  if (m_startHasQuad) {
    m_cel->data()->setQuad(m_startQuad);
  }
  else {
    m_cel->data()->removeQuad();
    m_cel->setBoundsF(m_startBounds);
  }
  m_cel->incrementVersion();
}

void TransformingRefCelState::onBeforeCommandExecution(CommandExecutionEvent& ev)
{
  if (ev.command()->id() == CommandId::Undo() || ev.command()->id() == CommandId::Redo() ||
      ev.command()->id() == CommandId::Cancel()) {
    restoreStart();
    m_editor->backToPreviousState();
    m_editor->releaseMouse();
    m_editor->invalidate();
  }
  ev.cancel();
}

} // namespace app
