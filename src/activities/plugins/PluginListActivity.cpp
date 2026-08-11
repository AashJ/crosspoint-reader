#include "PluginListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "PluginInfoActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PluginListActivity::onEnter() {
  Activity::onEnter();
  plugins = discoverPlugins();
  selectorIndex = 0;
  subscreenOpen = false;
  confirmArmed = false;  // wait for a press that starts inside this screen
  requestUpdate();
}

void PluginListActivity::goBack() {
  // Home launch replaced the home screen (root); Settings launch pushed us.
  if (rootMode) {
    onGoHome();
  } else {
    finish();
  }
}

void PluginListActivity::openSelected() {
  if (isOpdsRow(selectorIndex)) {
    activityManager.goToBrowser();  // replaces this screen with the OPDS browser
    return;
  }
  const int pi = pluginIndex(selectorIndex);
  if (pi < 0 || pi >= static_cast<int>(plugins.size())) return;
  subscreenOpen = true;
  startActivityForResult(std::make_unique<PluginInfoActivity>(renderer, mappedInput, plugins[pi]),
                         [this](const ActivityResult&) { subscreenOpen = false; });
}

void PluginListActivity::loop() {
  if (subscreenOpen) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmArmed = true;
  if (confirmArmed && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmArmed = false;
    openSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  if (itemCount() > 0) {
    const ListLayout l = GUI.getListLayout(renderer, /*hasSubtitle=*/true);
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, l.list.y, l.rowStep, l.pageItems);
    if (touch != MappedInputManager::RowTouch::None) {
      const int touched = selectorIndex / l.pageItems * l.pageItems + row;
      if (touched >= 0 && touched < itemCount()) {
        if (touch == MappedInputManager::RowTouch::Down) {
          if (selectorIndex != touched) {
            selectorIndex = touched;
            requestUpdate();
          }
        } else {
          selectorIndex = touched;
          openSelected();
        }
        return;
      }
    }

    buttonNavigator.onNextRelease([this] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, itemCount());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, itemCount());
      requestUpdate();
    });
  }
}

void PluginListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const auto& m = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, m.topPadding, pageWidth, m.headerHeight}, tr(STR_PLUGINS));

  const char* confirmLabel = itemCount() == 0 ? "" : tr(STR_OPEN);
  const char* up = itemCount() > 1 ? tr(STR_DIR_UP) : "";
  const char* down = itemCount() > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (itemCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_NO_PLUGINS_INSTALLED));
  } else {
    const ListLayout l = GUI.getListLayout(renderer, /*hasSubtitle=*/true);
    GUI.drawList(
        renderer, l.list, itemCount(), selectorIndex,
        [this](int i) { return isOpdsRow(i) ? std::string(tr(STR_OPDS_BROWSER)) : plugins[pluginIndex(i)].title; },
        [this](int i) {
          return isOpdsRow(i) ? std::string(tr(STR_OPDS_SERVERS)) : plugins[pluginIndex(i)].description;
        });
  }
  renderer.displayBuffer();
}
