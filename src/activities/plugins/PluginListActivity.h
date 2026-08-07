#pragma once
#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * The single Settings entry point for SD plugins: lists every installed plugin
 * (with title + description) and opens each one's info screen. Keeps the
 * settings menu stable no matter how many plugins the card carries.
 */
class PluginListActivity final : public Activity {
 public:
  // showOpds prepends an "OPDS Browser" row (used when launched from the home
  // screen with OPDS servers configured). rootMode: Back returns to the home
  // screen (home launch) rather than the previous activity (Settings launch).
  explicit PluginListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool showOpds = false,
                              bool rootMode = false)
      : Activity("PluginList", renderer, mappedInput), showOpds(showOpds), rootMode(rootMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  std::vector<PluginRef> plugins;
  bool showOpds = false;
  bool rootMode = false;
  int selectorIndex = 0;
  bool subscreenOpen = false;
  // Only open on a Confirm whose press happened here — ignores the launching
  // Confirm's release leaking in from the Settings menu, without swallowing a
  // genuine first press (which a blanket "consume first Confirm" would).
  bool confirmArmed = false;

  int itemCount() const { return static_cast<int>(plugins.size()) + (showOpds ? 1 : 0); }
  bool isOpdsRow(int i) const { return showOpds && i == 0; }
  int pluginIndex(int i) const { return showOpds ? i - 1 : i; }
  void goBack();
  void openSelected();
};
