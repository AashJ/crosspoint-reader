#pragma once
#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * The single Settings entry point for SD plugins: lists every plugin that
 * ships a device.json and opens its catalog subscreen. Keeps the settings
 * menu stable no matter how many plugins the card carries.
 */
class PluginListActivity final : public Activity {
 public:
  explicit PluginListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PluginList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  std::vector<PluginCatalogRef> catalogs;
  int selectorIndex = 0;
  bool subscreenOpen = false;

  void openSelected();
};
