// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/cmd/set_cel_quad.h"
#include "app/cmd/set_layer_flags.h"
#include "app/commands/command.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/modules/gui.h"
#include "app/tx.h"
#include "doc/cel.h"
#include "doc/cel_data.h"
#include "doc/image.h"
#include "doc/layer.h"
#include "doc/palette.h"
#include "doc/primitives.h"
#include "doc/quad.h"
#include "doc/sprite.h"

#include <algorithm>
#include <cmath>

namespace app {

using namespace doc;

namespace {

// Returns the active reference layer (or nullptr).
Layer* get_active_reference_layer(const Context* context)
{
  Layer* layer = context->activeSite().layer();
  if (layer && layer->isReference())
    return layer;
  return nullptr;
}

// Reads a pixel of a reference cel image as RGBA.
color_t ref_pixel_to_rgba(const Image* image,
                          const Palette* pal,
                          const int maskIndex,
                          const int x,
                          const int y)
{
  const color_t c = get_pixel(image, x, y);
  switch (image->pixelFormat()) {
    case IMAGE_RGB:       return c;
    case IMAGE_GRAYSCALE: {
      const int v = graya_getv(c);
      return rgba(v, v, v, graya_geta(c));
    }
    case IMAGE_INDEXED:
      if (int(c) == maskIndex || !pal || int(c) >= pal->size())
        return rgba(0, 0, 0, 0);
      return pal->getEntry(c);
  }
  return rgba(0, 0, 0, 0);
}

// Resamples the reference cel (with its bounds/perspective quad) to the
// sprite pixel grid. Each sprite pixel is the average of ss*ss samples
// of the reference image; pixels with less than 50% coverage are
// transparent, the others are opaque (crisp pixel-art edges). Colors
// are mapped to the sprite color mode (the closest palette entry in
// indexed mode). Returns nullptr if the cel doesn't cover the canvas.
ImageRef render_reference_cel(const Sprite* sprite, const Cel* cel, gfx::Point& outPos)
{
  const Image* src = cel->image();
  if (!src || src->width() < 1 || src->height() < 1)
    return nullptr;

  const Quad quad = cel->data()->quad();
  if (!quad.isConvex())
    return nullptr;

  const double iw = src->width();
  const double ih = src->height();
  const Homography inv = Homography::imageToQuad(quad, iw, ih).inverted(quad.center());

  // Area to render: the quad bounds clipped to the canvas.
  const gfx::RectF qb = quad.bounds();
  gfx::Rect area(int(std::floor(qb.x)),
                 int(std::floor(qb.y)),
                 int(std::ceil(qb.x2())) - int(std::floor(qb.x)),
                 int(std::ceil(qb.y2())) - int(std::floor(qb.y)));
  area &= sprite->bounds();
  if (area.isEmpty())
    return nullptr;

  // Supersampling factor: about one sample per source pixel (2..8).
  const double ratio = std::max<double>(iw / std::max<double>(qb.w, 1e-6),
                                       ih / std::max<double>(qb.h, 1e-6));
  const int ss = std::clamp(int(std::ceil(ratio)), 2, 8);
  const int n = ss * ss;

  const Palette* srcPal = sprite->palette(cel->frame());
  const Palette* dstPal = sprite->palette(cel->frame());
  const int srcMask = (src->pixelFormat() == IMAGE_INDEXED ? int(src->maskColor()) : -1);
  const PixelFormat format = sprite->pixelFormat();
  const int dstMask = (format == IMAGE_INDEXED ? int(sprite->transparentColor()) : 0);

  ImageRef dst(Image::create(format, area.w, area.h));
  if (!dst)
    return nullptr;
  dst->clear(format == IMAGE_INDEXED ? dstMask : 0);

  for (int y = 0; y < area.h; ++y) {
    for (int x = 0; x < area.w; ++x) {
      double r = 0, g = 0, b = 0, a = 0;
      for (int j = 0; j < ss; ++j) {
        for (int i = 0; i < ss; ++i) {
          const double sx = area.x + x + (i + 0.5) / ss;
          const double sy = area.y + y + (j + 0.5) / ss;
          double u, v;
          if (!inv.apply(sx, sy, u, v) || u < 0.0 || v < 0.0 || u >= iw || v >= ih)
            continue;
          const color_t c = ref_pixel_to_rgba(src, srcPal, srcMask, int(u), int(v));
          const double ca = rgba_geta(c);
          r += rgba_getr(c) * ca;
          g += rgba_getg(c) * ca;
          b += rgba_getb(c) * ca;
          a += ca;
        }
      }

      // Less than 50% coverage: transparent pixel
      if (a / n < 128.0 || a <= 0.0)
        continue;

      const int R = std::clamp(int(r / a + 0.5), 0, 255);
      const int G = std::clamp(int(g / a + 0.5), 0, 255);
      const int B = std::clamp(int(b / a + 0.5), 0, 255);

      color_t out = 0;
      switch (format) {
        case IMAGE_RGB: out = rgba(R, G, B, 255); break;
        case IMAGE_GRAYSCALE:
          out = graya(std::clamp(int(0.299 * R + 0.587 * G + 0.114 * B + 0.5), 0, 255), 255);
          break;
        case IMAGE_INDEXED:
          out = (dstPal ? dstPal->findBestfit(R, G, B, 255, dstMask) : 0);
          break;
        default: continue;
      }
      put_pixel(dst.get(), x, y, out);
    }
  }

  outPos = area.origin();
  return dst;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////
// RenderReferenceLayer: creates a new layer above the active reference
// layer with the reference image resampled to the sprite pixel grid.

class RenderReferenceLayerCommand : public Command {
public:
  RenderReferenceLayerCommand() : Command(CommandId::RenderReferenceLayer()) {}

protected:
  bool onEnabled(Context* context) override
  {
    if (!context->checkFlags(ContextFlags::ActiveDocumentIsWritable | ContextFlags::HasActiveLayer))
      return false;
    Layer* layer = get_active_reference_layer(context);
    return (layer && static_cast<LayerImage*>(layer)->getCelsCount() > 0);
  }

  void onExecute(Context* context) override
  {
    ContextWriter writer(context);
    Doc* doc = writer.document();
    Sprite* sprite = writer.sprite();
    Layer* refLayer = writer.layer();
    if (!doc || !sprite || !refLayer || !refLayer->isReference())
      return;

    LayerImage* newLayer = nullptr;
    {
      Tx tx(writer, friendlyName());
      DocApi api = doc->getApi(tx);

      newLayer = api.newLayer(refLayer->parent(), refLayer->name() + " Pixels");
      api.restackLayerAfter(newLayer, refLayer->parent(), refLayer);

      for (Cel* cel : static_cast<LayerImage*>(refLayer)->cels()) {
        gfx::Point pos;
        ImageRef image = render_reference_cel(sprite, cel, pos);
        if (!image)
          continue;
        Cel* newCel = api.addCel(newLayer, cel->frame(), image);
        if (newCel)
          api.setCelPosition(sprite, newCel, pos.x, pos.y);
      }
      tx.commit();
    }

    if (newLayer)
      context->setActiveLayer(newLayer);
    update_screen_for_document(doc);
  }
};

Command* CommandFactory::createRenderReferenceLayerCommand()
{
  return new RenderReferenceLayerCommand;
}

//////////////////////////////////////////////////////////////////////
// ResetReferenceTransform: removes the rotation/perspective of the
// active reference cel (keeps its center and approximate size, and
// restores the image aspect ratio).

class ResetReferenceTransformCommand : public Command {
public:
  ResetReferenceTransformCommand() : Command(CommandId::ResetReferenceTransform()) {}

protected:
  bool onEnabled(Context* context) override
  {
    if (!context->checkFlags(ContextFlags::ActiveDocumentIsWritable | ContextFlags::HasActiveLayer))
      return false;
    Layer* layer = get_active_reference_layer(context);
    return (layer && layer->cel(context->activeSite().frame()));
  }

  void onExecute(Context* context) override
  {
    ContextWriter writer(context);
    Layer* layer = writer.layer();
    if (!layer || !layer->isReference())
      return;
    Cel* cel = layer->cel(writer.frame());
    if (!cel || !cel->image())
      return;

    const Quad q = cel->data()->quad();
    const gfx::PointF c = q.center();
    const double top = std::hypot(q[1].x - q[0].x, q[1].y - q[0].y);
    const double bottom = std::hypot(q[2].x - q[3].x, q[2].y - q[3].y);
    const double w = std::max((top + bottom) / 2.0, 1.0);
    const double h = w * cel->image()->height() / cel->image()->width();
    const gfx::RectF bounds(c.x - w / 2.0, c.y - h / 2.0, w, h);

    Tx tx(writer, friendlyName());
    tx(new cmd::SetCelQuad(cel, bounds));
    tx.commit();

    update_screen_for_document(writer.document());
  }
};

Command* CommandFactory::createResetReferenceTransformCommand()
{
  return new ResetReferenceTransformCommand;
}

//////////////////////////////////////////////////////////////////////
// ReferencePaletteColors: switches the active reference layer between
// its original colors and the sprite palette colors (for indexed and
// grayscale sprites).

class ReferencePaletteColorsCommand : public Command {
public:
  ReferencePaletteColorsCommand() : Command(CommandId::ReferencePaletteColors()) {}

protected:
  bool onEnabled(Context* context) override
  {
    if (!context->checkFlags(ContextFlags::ActiveDocumentIsWritable | ContextFlags::HasActiveLayer))
      return false;
    return (get_active_reference_layer(context) != nullptr);
  }

  bool onChecked(Context* context) override
  {
    Layer* layer = get_active_reference_layer(context);
    return (layer && layer->hasFlags(LayerFlags::RefPaletteColors));
  }

  void onExecute(Context* context) override
  {
    ContextWriter writer(context);
    Layer* layer = writer.layer();
    if (!layer || !layer->isReference())
      return;

    LayerFlags flags = layer->flags();
    if (layer->hasFlags(LayerFlags::RefPaletteColors))
      flags = LayerFlags(int(flags) & ~int(LayerFlags::RefPaletteColors));
    else
      flags = LayerFlags(int(flags) | int(LayerFlags::RefPaletteColors));

    Tx tx(writer, friendlyName());
    tx(new cmd::SetLayerFlags(layer, flags));
    tx.commit();

    update_screen_for_document(writer.document());
  }
};

Command* CommandFactory::createReferencePaletteColorsCommand()
{
  return new ReferencePaletteColorsCommand;
}

} // namespace app
