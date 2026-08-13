#include "PluginListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "PluginInfoActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void PluginListActivity::onEnter() {
  UiListActivity::onEnter();
  plugins = discoverPlugins();
  subscreenOpen = false;
  confirmArmed = false;  // wait for a press that starts inside this screen

  rowItems.clear();
  rowItems.reserve(plugins.size() + (showOpds ? 1 : 0));
  for (int i = 0; i < static_cast<int>(plugins.size()) + (showOpds ? 1 : 0); i++) {
    fui::ListItem item;
    if (isOpdsRow(i)) {
      item.label = tr(STR_OPDS_BROWSER);
      item.subtitle = tr(STR_OPDS_SERVERS);
    } else {
      const PluginRef& plugin = plugins[pluginIndex(i)];
      item.label = plugin.title.c_str();
      if (!plugin.description.empty()) item.subtitle = plugin.description.c_str();
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void PluginListActivity::activateIndex(const int index) {
  app.clearTapFlash();  // both paths leave this screen
  if (isOpdsRow(index)) {
    activityManager.goToBrowser();  // replaces this screen with the OPDS browser
    return;
  }
  const int pi = pluginIndex(index);
  if (pi < 0 || pi >= static_cast<int>(plugins.size())) return;
  nav.selected = index;
  subscreenOpen = true;
  const PluginRef& plugin = plugins[pi];
  // A catalog plugin opens its on-device browser directly (Confirm or a tap).
  // Browser-only plugins have no catalog, so fall back to the info screen whose
  // README explains how to run them from the web interface.
  if (plugin.hasCatalog) {
    startActivityForResult(
        std::make_unique<PluginCatalogActivity>(renderer, mappedInput, plugin.manifestPath, plugin.title),
        [this](const ActivityResult&) { subscreenOpen = false; });
  } else {
    startActivityForResult(std::make_unique<PluginInfoActivity>(renderer, mappedInput, plugin),
                           [this](const ActivityResult&) { subscreenOpen = false; });
  }
}

bool PluginListActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmArmed = true;
  if (confirmArmed && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmArmed = false;
    if (nav.selected >= 0 && nav.selected < listCount()) activateIndex(nav.selected);
    return true;
  }
  return false;
}

void PluginListActivity::onBackButton() {
  // Home launch replaced the home screen (root); Settings launch pushed us.
  if (rootMode) {
    onGoHome();
  } else {
    finish();
  }
}

void PluginListActivity::buildScreen(UiScreen& screen) {
  // Same fui header the plugin catalogs use, so the header stays identical when
  // opening a plugin from this list (see drawChrome below).
  catalogScreenHeader(screen, renderer, headerTitle());

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_PLUGINS_INSTALLED), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Let a long plugin description wrap onto a second line under the title; the
  // row grows to fit it. maxLines=2 also marks the style caller-owned (an
  // all-default smallText fails textStyleUnset and the list would resubstitute).
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void PluginListActivity::drawFooter() {
  const char* confirmLabel = rowItems.empty() ? "" : tr(STR_OPEN);
  const char* up = listCount() > 1 ? tr(STR_DIR_UP) : "";
  const char* down = listCount() > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
