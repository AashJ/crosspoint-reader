#include "PluginListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "PluginInfoActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int HEADER_Y = 15;
constexpr int HEADER_X = 16;
constexpr int LIST_TOP = 60;
constexpr int ROW_STEP = 44;  // two lines per row: title + description
constexpr int PAGE_ITEMS = 9;
}  // namespace

void PluginListActivity::onEnter() {
  Activity::onEnter();
  plugins = discoverPlugins();
  selectorIndex = 0;
  subscreenOpen = false;
  // The Confirm press that launched us from the Settings menu releases inside
  // this activity; swallow it so it doesn't instantly open the first plugin.
  consumeConfirm = true;
  requestUpdate();
}

void PluginListActivity::openSelected() {
  if (plugins.empty()) return;
  subscreenOpen = true;
  startActivityForResult(std::make_unique<PluginInfoActivity>(renderer, mappedInput, plugins[selectorIndex]),
                         [this](const ActivityResult&) { subscreenOpen = false; });
}

void PluginListActivity::loop() {
  if (subscreenOpen) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!plugins.empty()) {
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, LIST_TOP, ROW_STEP, PAGE_ITEMS);
    if (touch != MappedInputManager::RowTouch::None) {
      const int touched = selectorIndex / PAGE_ITEMS * PAGE_ITEMS + row;
      if (touched >= 0 && touched < static_cast<int>(plugins.size())) {
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
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, plugins.size());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, plugins.size());
      requestUpdate();
    });
  }
}

void PluginListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawText(UI_12_FONT_ID, HEADER_X, HEADER_Y, tr(STR_PLUGINS), true, EpdFontFamily::BOLD);

  const char* confirmLabel = plugins.empty() ? "" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (plugins.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_PLUGINS_INSTALLED));
  } else {
    const int pageStart = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, LIST_TOP + (selectorIndex % PAGE_ITEMS) * ROW_STEP - 2, pageWidth - 1, ROW_STEP);
    for (size_t i = pageStart; i < plugins.size() && i < static_cast<size_t>(pageStart + PAGE_ITEMS); i++) {
      const int y = LIST_TOP + static_cast<int>(i % PAGE_ITEMS) * ROW_STEP;
      const bool black = i != static_cast<size_t>(selectorIndex);
      auto title = renderer.truncatedText(UI_12_FONT_ID, plugins[i].title.c_str(), pageWidth - 40);
      renderer.drawText(UI_12_FONT_ID, 20, y, title.c_str(), black);
      if (!plugins[i].description.empty()) {
        auto desc = renderer.truncatedText(UI_10_FONT_ID, plugins[i].description.c_str(), pageWidth - 40);
        renderer.drawText(UI_10_FONT_ID, 20, y + 20, desc.c_str(), black);
      }
    }
  }
  renderer.displayBuffer();
}
