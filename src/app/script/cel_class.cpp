// Aseprite
// Copyright (C) 2018-2026  Igara Studio S.A.
// Copyright (C) 2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/cmd/remove_cel.h"
#include "app/cmd/replace_image.h"
#include "app/cmd/set_cel_quad.h"
#include "app/cmd/set_cel_opacity.h"
#include "app/cmd/set_cel_position.h"
#include "app/cmd/set_cel_zindex.h"
#include "app/doc_api.h"
#include "app/tx.h"
#include "app/script/docobj.h"
#include "app/script/engine.h"
#include "app/script/luacpp.h"
#include "app/script/userdata.h"
#include "doc/cel.h"
#include "doc/layer.h"
#include "doc/sprite.h"

namespace app { namespace script {

using namespace doc;

namespace {

int Cel_eq(lua_State* L)
{
  const auto a = may_get_docobj<Cel>(L, 1);
  const auto b = may_get_docobj<Cel>(L, 2);
  lua_pushboolean(L, (!a && !b) || (a && b && a->id() == b->id()));
  return 1;
}

int Cel_get_sprite(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  push_docobj(L, cel->sprite());
  return 1;
}

int Cel_get_layer(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  push_docobj(L, (Layer*)cel->layer());
  return 1;
}

int Cel_get_frame(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  if (auto sprite = cel->sprite())
    push_sprite_frame(L, sprite, cel->frame());
  else
    lua_pushnil(L);
  return 1;
}

int Cel_get_frameNumber(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  lua_pushinteger(L, cel->frame() + 1);
  return 1;
}

int Cel_get_image(lua_State* L)
{
  auto cel = get_docobj<Cel>(L, 1);
  push_cel_image(L, cel);
  return 1;
}

int Cel_get_position(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  push_new<gfx::Point>(L, cel->position());
  return 1;
}

int Cel_get_bounds(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  push_new<gfx::Rect>(L, cel->bounds());
  return 1;
}

// Precise bounds of reference layer cels (a table {x,y,width,height}
// with fractional numbers).
int Cel_get_boundsF(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  const gfx::RectF b = cel->boundsF();
  lua_newtable(L);
  lua_pushnumber(L, b.x);
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, b.y);
  lua_setfield(L, -2, "y");
  lua_pushnumber(L, b.w);
  lua_setfield(L, -2, "width");
  lua_pushnumber(L, b.h);
  lua_setfield(L, -2, "height");
  return 1;
}

// Perspective quad of a reference layer cel: an array of 4 tables
// {x=..., y=...} (top-left, top-right, bottom-right, bottom-left image
// corners in sprite coordinates), or nil if the cel doesn't have one.
int Cel_get_quad(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  if (!cel->data()->hasQuad()) {
    lua_pushnil(L);
    return 1;
  }
  const doc::Quad q = cel->data()->quad();
  lua_newtable(L);
  for (int i = 0; i < 4; ++i) {
    lua_newtable(L);
    lua_pushnumber(L, q[i].x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, q[i].y);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

int Cel_set_quad(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  if (!cel->layer() || !cel->layer()->isReference())
    return luaL_error(L, "only reference layer cels can have a perspective quad");

  Tx tx(cel->sprite());
  if (lua_isnil(L, 2)) {
    tx(new cmd::SetCelQuad(cel, cel->boundsF()));
  }
  else {
    luaL_checktype(L, 2, LUA_TTABLE);
    doc::Quad q;
    for (int i = 0; i < 4; ++i) {
      if (lua_geti(L, 2, i + 1) == LUA_TNIL)
        return luaL_error(L, "the quad must have 4 points");
      lua_getfield(L, -1, "x");
      q[i].x = lua_tonumber(L, -1);
      lua_pop(L, 1);
      lua_getfield(L, -1, "y");
      q[i].y = lua_tonumber(L, -1);
      lua_pop(L, 2);
    }
    if (!q.isConvex())
      return luaL_error(L, "the quad must be convex");
    tx(new cmd::SetCelQuad(cel, q));
  }
  tx.commit();
  return 0;
}

int Cel_get_opacity(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  lua_pushinteger(L, cel->opacity());
  return 1;
}

int Cel_get_zIndex(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  lua_pushinteger(L, cel->zIndex());
  return 1;
}

int Cel_set_frame(lua_State* L)
{
  const auto cel = get_docobj<Cel>(L, 1);
  doc::frame_t frame = get_frame_number_from_arg(L, 2);

  if (cel->frame() == frame)
    return 0;

  Doc* doc = static_cast<Doc*>(cel->document());
  Tx tx(doc);
  DocApi api = doc->getApi(tx);
  api.moveCel(cel->layer(), cel->frame(), cel->layer(), frame);
  tx.commit();
  return 0;
}

int Cel_set_image(lua_State* L)
{
  auto cel = get_docobj<Cel>(L, 1);
  Tx tx(cel->sprite());
  if (may_get_obj<Image>(L, 2)) {
    const auto* srcImage = get_image_from_arg(L, 2);
    const ImageRef newImage(Image::createCopy(srcImage));
    tx(new cmd::ReplaceImage(cel->sprite(), cel->imageRef(), newImage));
  }
  else if (lua_isnil(L, 2))
    tx(new cmd::RemoveCel(cel));
  tx.commit();
  return 0;
}

int Cel_set_position(lua_State* L)
{
  auto cel = get_docobj<Cel>(L, 1);
  const gfx::Point pos = convert_args_into_point(L, 2);
  Tx tx(cel->sprite());
  tx(new cmd::SetCelPosition(cel, pos));
  tx.commit();
  return 0;
}

int Cel_set_opacity(lua_State* L)
{
  auto cel = get_docobj<Cel>(L, 1);
  Tx tx(cel->sprite());
  tx(new cmd::SetCelOpacity(cel, lua_tointeger(L, 2)));
  tx.commit();
  return 0;
}

int Cel_set_zIndex(lua_State* L)
{
  auto cel = get_docobj<Cel>(L, 1);
  Tx tx(cel->sprite());
  tx(new cmd::SetCelZIndex(cel, lua_tointeger(L, 2)));
  tx.commit();
  return 0;
}

const luaL_Reg Cel_methods[] = {
  { "__eq",  Cel_eq  },
  { nullptr, nullptr }
};

const Property Cel_properties[] = {
  { "sprite",      Cel_get_sprite,               nullptr                      },
  { "layer",       Cel_get_layer,                nullptr                      },
  { "frame",       Cel_get_frame,                Cel_set_frame                },
  { "frameNumber", Cel_get_frameNumber,          Cel_set_frame                },
  { "image",       Cel_get_image,                Cel_set_image                },
  { "bounds",      Cel_get_bounds,               nullptr                      },
  { "boundsF",     Cel_get_boundsF,              nullptr                      },
  { "quad",        Cel_get_quad,                 Cel_set_quad                 },
  { "position",    Cel_get_position,             Cel_set_position             },
  { "opacity",     Cel_get_opacity,              Cel_set_opacity              },
  { "zIndex",      Cel_get_zIndex,               Cel_set_zIndex               },
  { "color",       UserData_get_color<Cel>,      UserData_set_color<Cel>      },
  { "data",        UserData_get_text<Cel>,       UserData_set_text<Cel>       },
  { "properties",  UserData_get_properties<Cel>, UserData_set_properties<Cel> },
  { nullptr,       nullptr,                      nullptr                      }
};

} // anonymous namespace

DEF_MTNAME(Cel);

void register_cel_class(lua_State* L)
{
  using Cel = doc::Cel;
  REG_CLASS(L, Cel);
  REG_CLASS_PROPERTIES(L, Cel);
}

void push_sprite_cel(lua_State* L, Cel* cel)
{
  push_docobj(L, cel);
}

}} // namespace app::script
