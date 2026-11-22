// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_EDITOR_REF_CEL_HANDLES_H_INCLUDED
#define APP_UI_EDITOR_REF_CEL_HANDLES_H_INCLUDED
#pragma once

#include "doc/quad.h"
#include "gfx/point.h"
#include "gfx/rect.h"
#include "ui/cursor_type.h"

namespace doc {
class Cel;
}
namespace ui {
class Cursor;
class Graphics;
} // namespace ui

namespace app {
class Editor;

// On-canvas handles to transform reference layer cels with the Move
// tool: corner handles (perspective/distort), edge handles (stretch),
// and rotation zones outside the corners.
struct RefCelHit {
  enum Type { None, Corner, Edge, Rotate };
  Type type = None;
  int index = -1; // Corner index, or the first corner of the edge
};

// Returns the reference cel of the active layer/frame when it can be
// transformed with the on-canvas handles (Move tool active, the layer
// is visible, editable, and movable), or nullptr.
doc::Cel* get_transformable_ref_cel(Editor* editor);

// Quad corners of the cel in screen coordinates.
void get_ref_cel_screen_quad(Editor* editor, const doc::Cel* cel, gfx::PointF out[4]);

RefCelHit hit_test_ref_cel(Editor* editor, const doc::Cel* cel, const gfx::Point& screenPos);

// Draws the quad outline and handles (g is the editor graphics, in
// editor-local coordinates).
void draw_ref_cel_handles(Editor* editor, ui::Graphics* g, const doc::Cel* cel);

// Screen region covered by the outline/handles (for invalidation).
gfx::Rect get_ref_cel_handles_bounds(Editor* editor, const doc::Cel* cel);

// Sets the mouse cursor for the given hit. Returns false for "None".
bool set_ref_cel_cursor(Editor* editor, const doc::Cel* cel, const RefCelHit& hit);

// Sprite coordinates of the given screen point.
gfx::PointF ref_cel_screen_to_sprite(Editor* editor, const gfx::Point& screenPos);

} // namespace app

#endif
