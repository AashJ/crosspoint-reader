#pragma once
#include <string>
#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Per-plugin detail screen: title, description, and the plugin's README as
 * scrollable usage instructions. Offers an Open action when the plugin ships a
 * device.json (an on-device catalog). Browser-only plugins have no Open action;
 * their README explains how to use them from the web interface.
 */
class PluginInfoActivity final : public Activity {
 public:
  explicit PluginInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, PluginRef plugin)
      : Activity("PluginInfo", renderer, mappedInput), plugin(std::move(plugin)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  PluginRef plugin;
  ButtonNavigator buttonNavigator;
  std::vector<std::string> lines;  // README wrapped to the screen width
  int topLine = 0;
  bool subscreenOpen = false;

  void openCatalog();
  int visibleLines() const;
  void wrapReadme();
};
