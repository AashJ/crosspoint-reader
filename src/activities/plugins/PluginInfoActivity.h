#pragma once
#include <string>
#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

/**
 * Per-plugin detail screen: title, description, and the plugin's README as
 * scrollable usage instructions. Offers an Open action when the plugin ships a
 * device.json (an on-device catalog). Browser-only plugins have no Open action;
 * their README explains how to use them from the web interface.
 */
class PluginInfoActivity final : public Activity, private UiAppHost {
 public:
  explicit PluginInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, PluginRef plugin)
      : Activity("PluginInfo", renderer, mappedInput), UiAppHost(renderer), plugin(std::move(plugin)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  PluginRef plugin;
  ButtonNavigator buttonNavigator;
  std::vector<std::string> paragraphs;  // raw text lines (description + README), unwrapped
  std::vector<std::string> lines;       // paragraphs wrapped to wrappedWidth
  int wrappedWidth = -1;                // width `lines` was wrapped at; -1 = not yet wrapped
  int topLine = 0;
  // Lines the last-built screen body held; measured in buildScreen (the body
  // rect and fonts are final there), read by loop()'s scroll clamp.
  int visibleRows = 1;
  bool subscreenOpen = false;

  static void rootScreen(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  void openCatalog();
  void loadParagraphs();  // read raw text (no measuring) — safe in onEnter
  void ensureWrapped();   // wrap paragraphs at the current width — call on the render task
};
