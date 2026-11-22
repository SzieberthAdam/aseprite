// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/cmd/set_cel_quad.h"

#include "app/doc.h"
#include "app/doc_event.h"
#include "doc/cel.h"
#include "doc/cel_data.h"

namespace app { namespace cmd {

using namespace doc;

SetCelQuad::SetCelQuad(Cel* cel, const Quad& quad) : WithCel(cel), m_old(getState(cel))
{
  m_new.hasQuad = true;
  m_new.quad = quad;
  m_new.bounds = quad.bounds();
}

SetCelQuad::SetCelQuad(Cel* cel, const gfx::RectF& bounds) : WithCel(cel), m_old(getState(cel))
{
  m_new.hasQuad = false;
  m_new.bounds = bounds;
}

// static
SetCelQuad::State SetCelQuad::getState(Cel* cel)
{
  State s;
  s.hasQuad = cel->data()->hasQuad();
  s.quad = cel->data()->quad();
  s.bounds = cel->boundsF();
  return s;
}

// static
void SetCelQuad::setState(Cel* cel, const State& s)
{
  if (s.hasQuad) {
    cel->data()->setQuad(s.quad);
  }
  else {
    cel->data()->removeQuad();
    cel->setBoundsF(s.bounds);
  }
  cel->incrementVersion();
}

void SetCelQuad::onExecute()
{
  setState(cel(), m_new);
}

void SetCelQuad::onUndo()
{
  setState(cel(), m_old);
}

void SetCelQuad::onFireNotifications()
{
  Cel* cel = this->cel();
  Doc* doc = static_cast<Doc*>(cel->document());
  DocEvent ev(doc);
  ev.sprite(cel->sprite());
  ev.cel(cel);
  doc->notify_observers<DocEvent&>(&DocObserver::onCelPositionChanged, ev);
}

}} // namespace app::cmd
