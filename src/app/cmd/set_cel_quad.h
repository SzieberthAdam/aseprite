// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_CMD_SET_CEL_QUAD_H_INCLUDED
#define APP_CMD_SET_CEL_QUAD_H_INCLUDED
#pragma once

#include "app/cmd.h"
#include "app/cmd/with_cel.h"
#include "doc/quad.h"
#include "gfx/rect.h"

namespace app { namespace cmd {
using namespace doc;

// Changes the perspective quad of a reference layer cel, or removes it
// (back to a plain axis-aligned reference cel with the given bounds).
class SetCelQuad : public Cmd,
                   public WithCel {
public:
  SetCelQuad(Cel* cel, const Quad& quad);
  // Removes the quad and sets the given bounds.
  SetCelQuad(Cel* cel, const gfx::RectF& bounds);

protected:
  void onExecute() override;
  void onUndo() override;
  void onFireNotifications() override;
  size_t onMemSize() const override { return sizeof(*this); }

private:
  struct State {
    bool hasQuad = false;
    Quad quad;
    gfx::RectF bounds;
  };
  static State getState(Cel* cel);
  static void setState(Cel* cel, const State& state);

  State m_old;
  State m_new;
};

}} // namespace app::cmd

#endif
