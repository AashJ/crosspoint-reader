#pragma once
#include <I18n.h>

#include <vector>

#include "PluginCatalogActivity.h"
#include "activities/UiListActivity.h"

/**
 * The single Settings entry point for SD plugins: lists every installed plugin
 * (with title + description) and opens each one's info screen. Keeps the
 * settings menu stable no matter how many plugins the card carries.
 */
class PluginListActivity final : public UiListActivity {
 public:
  // showOpds prepends an "OPDS Browser" row (used when launched from the home
  // screen with OPDS servers configured). rootMode: Back returns to the home
  // screen (home launch) rather than the previous activity (Settings launch).
  explicit PluginListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool showOpds = false,
                              bool rootMode = false)
      : UiListActivity("PluginList", renderer, mappedInput), showOpds(showOpds), rootMode(rootMode) {}

  void onEnter() override;

 private:
  std::vector<PluginRef> plugins;
  // Row buffer over `plugins` (labels/subtitles point into its strings);
  // rebuilt with it in onEnter().
  std::vector<freeink::ui::ListItem> rowItems;
  bool showOpds = false;
  bool rootMode = false;
  bool subscreenOpen = false;
  // Only open on a Confirm whose press happened here — ignores the launching
  // Confirm's release leaking in from the Settings menu, without swallowing a
  // genuine first press (which a blanket "consume first Confirm" would).
  bool confirmArmed = false;

  bool isOpdsRow(int i) const { return showOpds && i == 0; }
  int pluginIndex(int i) const { return showOpds ? i - 1 : i; }

  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  // The header is drawn inside buildScreen (catalogScreenHeader), so suppress
  // the base GUI.drawHeader to avoid a second, mismatched header band.
  void drawChrome() override {}
  void activateIndex(int index) override;
  const char* headerTitle() const override { return tr(STR_PLUGINS); }
  bool handleCustomInput() override { return subscreenOpen; }
  bool handleButtons() override;
  void onBackButton() override;
  void drawFooter() override;
};
