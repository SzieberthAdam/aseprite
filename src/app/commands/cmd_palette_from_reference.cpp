// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/cmd/replace_image.h"
#include "app/cmd/set_palette.h"
#include "app/commands/command.h"
#include "app/context.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/i18n/strings.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/tx.h"
#include "app/util/color_clustering.h"
#include "base/time.h"
#include "doc/cel.h"
#include "doc/cels_range.h"
#include "doc/image.h"
#include "doc/layer.h"
#include "doc/palette.h"
#include "doc/primitives.h"
#include "doc/sprite.h"
#include "fmt/format.h"
#include "ui/alert.h"
#include "ui/scale.h"
#include "ui/box.h"
#include "ui/button.h"
#include "ui/combobox.h"
#include "ui/grid.h"
#include "ui/label.h"
#include "ui/slider.h"
#include "ui/timer.h"
#include "ui/window.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <map>
#include <memory>

namespace app {

using namespace doc;
using namespace ui;

namespace {

// Reference cel used as the source of colors: the active layer if it's
// a reference layer, or the first visible reference layer.
Cel* get_source_reference_cel(const Context* context)
{
  const Site site = context->activeSite();
  const Sprite* sprite = site.sprite();
  if (!sprite)
    return nullptr;

  if (Layer* layer = site.layer()) {
    if (layer->isReference())
      return layer->cel(site.frame());
  }
  for (Layer* layer : sprite->allVisibleReferenceLayers()) {
    if (Cel* cel = layer->cel(site.frame()))
      return cel;
  }
  return nullptr;
}

// The live preview: changes the palette and the indexed images of the
// sprite directly (without undo) and restores them at the end.
class PalettePreview {
public:
  PalettePreview(Doc* doc, Sprite* sprite, frame_t frame)
    : m_doc(doc)
    , m_sprite(sprite)
    , m_palFrame(sprite->palette(frame)->frame())
    , m_origPal(*sprite->palette(frame))
  {
    // Keep a slot for the transparent color in indexed sprites with
    // transparent layers.
    if (sprite->pixelFormat() == IMAGE_INDEXED) {
      for (const Layer* layer : sprite->allLayers()) {
        if (layer->isImage() && !layer->isBackground() && !layer->isReference()) {
          m_maskIndex = sprite->transparentColor();
          break;
        }
      }

      for (Cel* cel : sprite->uniqueCels()) {
        if (cel->layer()->isReference() || !cel->image() ||
            cel->image()->pixelFormat() != IMAGE_INDEXED)
          continue;
        const ImageRef image = cel->imageRef();
        if (m_images.find(image->id()) == m_images.end())
          m_images[image->id()] = { image, ImageRef(Image::createCopy(image.get())) };
      }
    }
  }

  // Creates the new palette from the clusters.
  Palette makePalette(const std::vector<ColorCluster>& clusters, const bool cumulative) const
  {
    std::vector<color_t> colors;
    for (const auto& c : clusters)
      colors.push_back(c.mean);
    if (cumulative) {
      for (const auto& c : clusters)
        colors.push_back(c.cumulative);
    }

    int n = int(colors.size());
    if (m_maskIndex >= 0)
      n = std::max(n + 1, m_maskIndex + 1);

    Palette pal(m_palFrame, std::max(n, 1));
    int i = 0;
    for (int j = 0; j < pal.size(); ++j) {
      if (j == m_maskIndex)
        pal.setEntry(j,
                     (j < m_origPal.size() ? m_origPal.getEntry(j) : rgba(0, 0, 0, 0)));
      else if (i < int(colors.size()))
        pal.setEntry(j, colors[i++]);
      else
        pal.setEntry(j, rgba(0, 0, 0, 255));
    }
    return pal;
  }

  // Old index -> new index (nearest color of the new palette).
  std::array<color_t, 256> makeRemap(const Palette& newPal) const
  {
    std::array<color_t, 256> table;
    for (int i = 0; i < 256; ++i) {
      if (i == m_maskIndex) {
        table[i] = i;
        continue;
      }
      const color_t c = (i < m_origPal.size() ? m_origPal.getEntry(i) : rgba(0, 0, 0, 255));
      table[i] =
        newPal.findBestfit(rgba_getr(c), rgba_getg(c), rgba_getb(c), rgba_geta(c), m_maskIndex);
    }
    return table;
  }

  static void remapImage(Image* dst, const Image* src, const std::array<color_t, 256>& table)
  {
    for (int y = 0; y < src->height(); ++y)
      for (int x = 0; x < src->width(); ++x)
        put_pixel(dst, x, y, table[get_pixel(src, x, y) & 255]);
  }

  void apply(const Palette& newPal, const bool remap)
  {
    m_sprite->setPalette(&newPal, true);

    const auto table = makeRemap(newPal);
    for (auto& [id, pair] : m_images) {
      if (remap)
        remapImage(pair.image.get(), pair.original.get(), table);
      else
        copy_image(pair.image.get(), pair.original.get());
      pair.image->incrementVersion();
    }
    refresh();
  }

  void restore()
  {
    m_sprite->setPalette(&m_origPal, true);
    for (auto& [id, pair] : m_images) {
      copy_image(pair.image.get(), pair.original.get());
      pair.image->incrementVersion();
    }
    refresh();
  }

  void commit(Tx& tx, const Palette& newPal, const bool remap)
  {
    tx(new cmd::SetPalette(m_sprite, m_palFrame, &newPal));
    if (remap) {
      const auto table = makeRemap(newPal);
      for (auto& [id, pair] : m_images) {
        ImageRef newImage(Image::createCopy(pair.original.get()));
        remapImage(newImage.get(), pair.original.get(), table);
        tx(new cmd::ReplaceImage(m_sprite, pair.image, newImage));
      }
    }
  }

private:
  void refresh()
  {
    set_current_palette(m_sprite->palette(m_palFrame), false);
    m_doc->notifyGeneralUpdate();
    update_screen_for_document(m_doc);
  }

  struct ImagePair {
    ImageRef image;
    ImageRef original;
  };

  Doc* m_doc;
  Sprite* m_sprite;
  frame_t m_palFrame;
  Palette m_origPal;
  int m_maskIndex = -1;
  std::map<ObjectId, ImagePair> m_images;
};

ComboBox* make_combobox(const std::vector<const char*>& items, const int selected)
{
  auto* combo = new ComboBox;
  for (const char* item : items)
    combo->addItem(item);
  combo->setSelectedItemIndex(selected);
  combo->setExpansive(true);
  return combo;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////
// PaletteFromReference: clusters the colors of the visible part of a
// reference layer and uses the clusters as the sprite palette (with a
// live preview while the parameters are changed).

class PaletteFromReferenceCommand : public Command {
public:
  PaletteFromReferenceCommand() : Command(CommandId::PaletteFromReference()) {}

protected:
  bool onEnabled(Context* context) override
  {
    return context->checkFlags(ContextFlags::ActiveDocumentIsWritable |
                               ContextFlags::HasActiveSprite) &&
           get_source_reference_cel(context) != nullptr;
  }

  void onLoadParams(const Params& params) override;
  void onExecute(Context* context) override;

private:
  // Parameters (from scripts): when "ui" is false the palette is
  // created without the dialog.
  bool m_ui = true;
};

// Last used options (for this session)
static ClusterOptions g_options;
static bool g_cumulative = false;
static bool g_remap = true;

template<typename T>
static void parse_choice(const std::string& value, const std::vector<const char*>& names, T& out)
{
  for (size_t i = 0; i < names.size(); ++i) {
    if (value == names[i]) {
      out = T(i);
      return;
    }
  }
}

void PaletteFromReferenceCommand::onLoadParams(const Params& params)
{
  m_ui = (params.get("ui") != "false");
  if (params.has_param("k"))
    g_options.k = std::clamp(std::atoi(params.get("k").c_str()), 1, 255);
  if (params.has_param("precision"))
    g_options.precision = std::clamp(std::atoi(params.get("precision").c_str()), 1, 8);
  parse_choice(params.get("space"), { "oklab", "lab", "hsv", "rgb", "linear" }, g_options.space);
  parse_choice(params.get("weight"), { "pixels", "sqrt", "one" }, g_options.weight);
  parse_choice(params.get("average"), { "rgb", "linear", "oklab", "lab" }, g_options.average);
  if (params.has_param("cumulative"))
    g_cumulative = (params.get("cumulative") == "true");
  if (params.has_param("remap"))
    g_remap = (params.get("remap") != "false");
}

static void show_alert(Context* context, const std::string& msg)
{
  if (context->isUIAvailable())
    ui::Alert::show(msg);
}

void PaletteFromReferenceCommand::onExecute(Context* context)
{
  Doc* doc = context->activeDocument();
  Sprite* sprite = context->activeSite().sprite();
  const frame_t frame = context->activeSite().frame();
  Cel* refCel = get_source_reference_cel(context);
  if (!doc || !sprite || !refCel) {
    show_alert(context, Strings::palette_from_reference_no_reference());
    return;
  }

  const ColorHistogram hist = collect_reference_cel_colors(refCel);
  if (hist.empty()) {
    show_alert(context, Strings::palette_from_reference_no_colors());
    return;
  }

  PalettePreview preview(doc, sprite, frame);

  // Without UI (scripts)
  if (!m_ui || !context->isUIAvailable()) {
    const std::vector<ColorCluster> clusters = cluster_colors(hist, g_options);
    const Palette pal = preview.makePalette(clusters, g_cumulative);
    ContextWriter writer(context);
    Tx tx(writer, friendlyName());
    preview.commit(tx, pal, g_remap && sprite->pixelFormat() == IMAGE_INDEXED);
    tx.commit();
    return;
  }

  // Dialog
  Window window(Window::WithTitleBar, Strings::palette_from_reference_title());
  auto* grid = new ui::Grid(2, false);
  auto* kSlider = new Slider(1, 64, g_options.k);
  auto* space = make_combobox({ "oklab", "lab", "hsv", "rgb", "linear" }, int(g_options.space));
  auto* weight = make_combobox({ "pixels", "sqrt", "one" }, int(g_options.weight));
  auto* average = make_combobox({ "rgb", "linear", "oklab", "lab" }, int(g_options.average));
  auto* precision = new Slider(3, 8, g_options.precision);
  auto* cumulative = new CheckBox(Strings::palette_from_reference_cumulative());
  auto* remap = new CheckBox(Strings::palette_from_reference_remap());
  auto* info = new Label("");
  auto* buttons = new Box(HORIZONTAL | HOMOGENEOUS);
  auto* ok = new Button(Strings::general_ok());
  auto* cancel = new Button(Strings::general_cancel());

  kSlider->setExpansive(true);
  precision->setExpansive(true);
  cumulative->setSelected(g_cumulative);
  remap->setSelected(g_remap);
  remap->setVisible(sprite->pixelFormat() == IMAGE_INDEXED);
  ok->setMinSize(gfx::Size(60 * ui::guiscale(), 0));
  ok->Click.connect([&window, ok] { window.closeWindow(ok); });
  cancel->Click.connect([&window, cancel] { window.closeWindow(cancel); });

  auto addRow = [grid](const std::string& text, Widget* widget) {
    grid->addChildInCell(new Label(text), 1, 1, LEFT | MIDDLE);
    grid->addChildInCell(widget, 1, 1, HORIZONTAL | MIDDLE);
  };
  addRow(Strings::palette_from_reference_colors(), kSlider);
  addRow(Strings::palette_from_reference_space(), space);
  addRow(Strings::palette_from_reference_weight(), weight);
  addRow(Strings::palette_from_reference_average(), average);
  addRow(Strings::palette_from_reference_precision(), precision);
  grid->addChildInCell(cumulative, 2, 1, LEFT);
  grid->addChildInCell(remap, 2, 1, LEFT);
  grid->addChildInCell(info, 2, 1, LEFT);
  buttons->addChild(ok);
  buttons->addChild(cancel);
  grid->addChildInCell(buttons, 2, 1, RIGHT);
  window.addChild(grid);

  auto readOptions = [&]() {
    g_options.k = kSlider->getValue();
    g_options.space = ClusterSpace(space->getSelectedItemIndex());
    g_options.weight = ClusterWeight(weight->getSelectedItemIndex());
    g_options.average = ClusterAverage(average->getSelectedItemIndex());
    g_options.precision = precision->getValue();
    g_cumulative = cumulative->isSelected();
    g_remap = remap->isSelected();
  };

  std::unique_ptr<Palette> newPal;
  auto update = [&]() {
    readOptions();
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<ColorCluster> clusters = cluster_colors(hist, g_options);
    const auto t1 = std::chrono::steady_clock::now();
    newPal = std::make_unique<Palette>(preview.makePalette(clusters, g_cumulative));
    preview.apply(*newPal, g_remap);

    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    info->setText(Strings::palette_from_reference_info(hist.colors.size(),
                                                       hist.totalPixels,
                                                       clusters.size(),
                                                       ms));
    window.layout();
  };

  // Recalculate a little after the last change
  Timer timer(250, &window);
  timer.Tick.connect([&] {
    timer.stop();
    update();
  });
  auto schedule = [&timer] { timer.start(); };
  kSlider->Change.connect(schedule);
  space->Change.connect(schedule);
  weight->Change.connect(schedule);
  average->Change.connect(schedule);
  precision->Change.connect(schedule);
  cumulative->Click.connect(schedule);
  remap->Click.connect(schedule);

  update();
  window.remapWindow();
  window.centerWindow();
  window.openWindowInForeground();
  timer.stop();

  const bool accepted = (window.closer() == ok);
  if (accepted)
    readOptions();

  preview.restore();

  if (accepted) {
    // Recalculate if the last change wasn't previewed yet
    const std::vector<ColorCluster> clusters = cluster_colors(hist, g_options);
    const Palette pal = preview.makePalette(clusters, g_cumulative);

    ContextWriter writer(context);
    Tx tx(writer, friendlyName());
    preview.commit(tx, pal, g_remap && sprite->pixelFormat() == IMAGE_INDEXED);
    tx.commit();

    set_current_palette(sprite->palette(frame), false);
    update_screen_for_document(doc);
  }
}

Command* CommandFactory::createPaletteFromReferenceCommand()
{
  return new PaletteFromReferenceCommand;
}

} // namespace app
