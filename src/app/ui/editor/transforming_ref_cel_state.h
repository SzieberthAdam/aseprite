// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_EDITOR_TRANSFORMING_REF_CEL_STATE_H_INCLUDED
#define APP_UI_EDITOR_TRANSFORMING_REF_CEL_STATE_H_INCLUDED
#pragma once

#include "app/context_access.h"
#include "app/ui/editor/ref_cel_handles.h"
#include "app/ui/editor/standby_state.h"
#include "doc/quad.h"
#include "obs/connection.h"

namespace doc {
class Cel;
}

namespace app {
class CommandExecutionEvent;
class Editor;

// Editor state used while the user drags one of the on-canvas
// handles of a reference layer cel (corner = perspective, edge =
// stretch, outside a corner = rotate).
class TransformingRefCelState : public StandbyState {
public:
  TransformingRefCelState(Editor* editor,
                          const ui::MouseMessage* msg,
                          doc::Cel* cel,
                          const RefCelHit& hit);

  void onBeforePopState(Editor* editor) override;
  bool onMouseUp(Editor* editor, ui::MouseMessage* msg) override;
  bool onMouseMove(Editor* editor, ui::MouseMessage* msg) override;
  bool onSetCursor(Editor* editor, const gfx::Point& mouseScreenPos) override;
  bool onKeyDown(Editor* editor, ui::KeyMessage* msg) override;
  bool onKeyUp(Editor* editor, ui::KeyMessage* msg) override;
  bool onUpdateStatusBar(Editor* editor) override;
  bool requireBrushPreview() override { return false; }

private:
  void updateQuad(Editor* editor, const gfx::Point& screenPos, bool ctrl);
  void restoreStart();
  void onBeforeCommandExecution(CommandExecutionEvent& ev);

  ContextReader m_reader;
  Editor* m_editor;
  doc::Cel* m_cel;
  RefCelHit m_hit;
  bool m_startHasQuad;
  doc::Quad m_startQuad;
  gfx::RectF m_startBounds;
  gfx::PointF m_cursorStart; // In sprite coordinates
  gfx::Point m_lastScreenPos;
  doc::Quad m_quad;
  double m_angle = 0.0; // Rotation angle (radians) for the status bar
  bool m_modified = false;

  obs::scoped_connection m_ctxConn;
};

} // namespace app

#endif
