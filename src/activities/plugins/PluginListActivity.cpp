#include "PluginListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int HEADER_Y = 15;
constexpr int HEADER_X = 16;
constexpr int LIST_TOP = 60;
constexpr int ROW_STEP = 30;
constexpr int PAGE_ITEMS = 23;
}  // namespace

void PluginListActivity::onEnter() {
  Activity::onEnter();
  catalogs = discoverPluginCatalogs();
  selectorIndex = 0;
  subscreenOpen = false;
  requestUpdate();
}

void PluginListActivity::openSelected() {
  if (catalogs.empty()) return;
  const auto& catalog = catalogs[selectorIndex];
  subscreenOpen = true;
  startActivityForResult(
      std::make_unique<PluginCatalogActivity>(renderer, mappedInput, catalog.manifestPath, catalog.title),
      [this](const ActivityResult&) { subscreenOpen = false; });
}

void PluginListActivity::loop() {
  if (subscreenOpen) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!catalogs.empty()) {
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, LIST_TOP, ROW_STEP, PAGE_ITEMS);
    if (touch != MappedInputManager::RowTouch::None) {
      const int touched = selectorIndex / PAGE_ITEMS * PAGE_ITEMS + row;
      if (touched >= 0 && touched < static_cast<int>(catalogs.size())) {
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
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, catalogs.size());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, catalogs.size());
      requestUpdate();
    });
  }
}

void PluginListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawText(UI_12_FONT_ID, HEADER_X, HEADER_Y, tr(STR_PLUGINS), true, EpdFontFamily::BOLD);

  const char* confirmLabel = catalogs.empty() ? "" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (catalogs.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_PLUGINS_INSTALLED));
  } else {
    const int pageStart = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, LIST_TOP + (selectorIndex % PAGE_ITEMS) * ROW_STEP - 2, pageWidth - 1, ROW_STEP);
    for (size_t i = pageStart; i < catalogs.size() && i < static_cast<size_t>(pageStart + PAGE_ITEMS); i++) {
      auto line = renderer.truncatedText(UI_10_FONT_ID, catalogs[i].title.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, LIST_TOP + static_cast<int>(i % PAGE_ITEMS) * ROW_STEP, line.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}
