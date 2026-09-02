#pragma once

#include <FreeInkApp.h>

#include <algorithm>
#include <cstdint>

namespace library_home {

namespace fui = freeink::ui;

constexpr fui::ActionId ACTION_BOOK = 1;
constexpr fui::ActionId ACTION_MENU = 2;
constexpr fui::ActionId ACTION_UTILITY = 3;
constexpr fui::ActionId ACTION_DISMISS = 4;

struct Labels {
  const char* menu = nullptr;
  const char* empty = nullptr;
};

struct Model {
  Labels labels{};
  fui::CoverGridItemProvider bookProvider = nullptr;
  void* bookProviderUserData = nullptr;
  fui::CoverGridCoverPainter coverPainter = nullptr;
  void* coverPainterUserData = nullptr;
  const fui::TileGridItem* utilityItems = nullptr;
  uint16_t bookCount = 0;
  uint16_t topIndex = 0;
  uint16_t utilityCount = 0;
  int16_t selectedBook = -1;
  int16_t topChrome = 0;
  int16_t bottomChrome = 0;
  bool menuSelected = false;
  bool menuOpen = false;
};

struct Layout {
  fui::Rect grid{};
  fui::Rect menuButton{};
  fui::Rect utilityGrid{};
  uint16_t visibleBooks = 0;
  uint8_t columns = 3;
  fui::Size coverSize{};
  int16_t rowHeight = 0;
};

struct ScreenStorage {
  fui::CoverGridProps covers{};
  fui::ButtonProps menuButton{};
  fui::SheetProps sheet{};
  fui::HeaderProps sheetHeader{};
  fui::TileGridProps utilities{};
  Layout layout{};
};

inline Layout computeLayout(fui::Rect body, const fui::ThemeTokens& theme, const int16_t smallLineHeight) {
  Layout result;
  const bool portrait = body.height >= body.width;
  result.columns = portrait ? 3 : 5;

  const int16_t menuHeight = std::max<int16_t>(theme.minTouchSize, theme.rowHeight);
  result.menuButton = fui::Rect{body.x, static_cast<int16_t>(body.bottom() - menuHeight), body.width, menuHeight};
  result.grid = fui::Rect{body.x, body.y, body.width,
                          static_cast<int16_t>(std::max<int>(0, result.menuButton.y - theme.spaceMd - body.y))};

  const int16_t rowGap = theme.spaceMd;
  const int16_t rows = portrait ? 2 : 1;
  result.rowHeight =
      static_cast<int16_t>(std::max<int>(1, (result.grid.height - static_cast<int16_t>((rows - 1) * rowGap)) / rows));
  const int16_t cellWidth = static_cast<int16_t>(
      (result.grid.width - static_cast<int16_t>((result.columns - 1) * theme.spaceSm)) / result.columns);
  const int16_t labelHeight = static_cast<int16_t>(std::max<int16_t>(1, smallLineHeight) * 2);
  const int16_t maxCoverWidth = static_cast<int16_t>(std::max<int>(1, cellWidth - 2 * theme.spaceSm));
  const int16_t maxCoverHeight =
      static_cast<int16_t>(std::max<int>(1, result.rowHeight - labelHeight - theme.spaceSm - theme.spaceMd));
  int16_t coverWidth = std::min<int16_t>(maxCoverWidth, static_cast<int16_t>(maxCoverHeight * 2 / 3));
  int16_t coverHeight = static_cast<int16_t>(coverWidth * 3 / 2);
  if (coverHeight > maxCoverHeight) {
    coverHeight = maxCoverHeight;
    coverWidth = static_cast<int16_t>(coverHeight * 2 / 3);
  }
  result.coverSize = fui::Size{coverWidth, coverHeight};
  result.visibleBooks = fui::coverGridVisibleCells(result.grid, result.columns, result.rowHeight, rowGap);
  return result;
}

template <size_t MaxInteractions>
void build(fui::Screen<MaxInteractions>& screen, const Model& model, ScreenStorage& storage) {
  const auto& theme = screen.theme();
  screen.setContentMarginFromScreen(fui::Insets{model.topChrome, 0, model.bottomChrome, 0});
  screen.insetContent(fui::Insets{theme.spaceMd, theme.spaceMd, theme.spaceMd, theme.spaceMd});

  storage.layout = computeLayout(screen.body(), theme, screen.target().lineHeight(theme.smallText.font));

  storage.menuButton.label = model.labels.menu;
  storage.menuButton.action = model.menuOpen ? fui::NO_ACTION : ACTION_MENU;
  storage.menuButton.inputMask = fui::InputTouch;
  storage.menuButton.state = model.menuSelected ? fui::StateSelected : fui::StateNormal;
  storage.menuButton.text = theme.bodyText;
  storage.menuButton.styles = theme.button;
  storage.menuButton.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  storage.menuButton.styles.normal.borderWidth = 1;
  storage.menuButton.styles.selected.border = fui::Paint::solid(fui::Color::Black);
  storage.menuButton.styles.selected.borderWidth = 1;
  storage.menuButton.borderEdges = fui::EdgeTop;
  screen.button(storage.menuButton, storage.layout.menuButton);

  if (model.bookCount > 0 && model.bookProvider) {
    storage.covers.itemProvider = model.bookProvider;
    storage.covers.itemProviderUserData = model.bookProviderUserData;
    storage.covers.count = model.bookCount;
    storage.covers.topIndex = model.topIndex;
    storage.covers.selectedIndex = model.selectedBook;
    storage.covers.action = model.menuOpen ? fui::NO_ACTION : ACTION_BOOK;
    storage.covers.inputMask = fui::InputTouch;
    storage.covers.titleText = theme.smallText;
    storage.covers.titleText.maxLines = 2;
    storage.covers.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
    storage.covers.columns = storage.layout.columns;
    storage.covers.coverSize = storage.layout.coverSize;
    storage.covers.rowHeight = storage.layout.rowHeight;
    storage.covers.gap = theme.spaceSm;
    storage.covers.rowGap = theme.spaceMd;
    storage.covers.cellInset = fui::Insets{theme.spaceSm, 0, 0, 0};
    storage.covers.labelHeight = static_cast<int16_t>(screen.target().lineHeight(theme.smallText.font) * 2);
    storage.covers.labelGap = theme.spaceSm;
    storage.covers.coverPainter = model.coverPainter;
    storage.covers.coverPainterUserData = model.coverPainterUserData;
    fui::coverGrid(screen.frame(), storage.layout.grid, storage.covers);
  } else if (model.labels.empty) {
    fui::TextStyle emptyText = theme.bodyText;
    emptyText.align = fui::TextAlign::Center;
    emptyText.maxLines = 2;
    screen.target().text(storage.layout.grid, model.labels.empty, emptyText);
  }

  if (!model.menuOpen) return;

  storage.sheet.anchor = fui::SheetEdge::Bottom;
  storage.sheet.dismissAction = ACTION_DISMISS;
  storage.sheet.grabberMargin = theme.spaceMd;
  storage.sheet.grabberInset = theme.spaceLg;
  const int16_t tileHeight = static_cast<int16_t>(theme.minTouchSize + 28);
  const int16_t tileGap = theme.spaceMd;
  const int16_t gridHeight = fui::tileGridHeight(model.utilityCount, 2, tileHeight, tileGap);
  const int16_t grabberBand =
      static_cast<int16_t>(storage.sheet.grabberMargin + storage.sheet.grabberHeight + storage.sheet.grabberInset);
  const int16_t sheetHeight = static_cast<int16_t>(std::min<int>(
      screen.frame().safeRect().height, grabberBand + theme.headerHeight + theme.spaceMd + gridHeight + theme.spaceLg));
  screen.sheet(storage.sheet, sheetHeight);
  screen.insetContent(fui::Insets{0, theme.spaceLg, theme.spaceLg, theme.spaceLg});
  storage.sheetHeader.title = model.labels.menu;
  storage.sheetHeader.borderEdges = fui::EdgesNone;
  screen.header(storage.sheetHeader);

  storage.utilities.items = model.utilityItems;
  storage.utilities.count = model.utilityCount;
  storage.utilities.action = ACTION_UTILITY;
  storage.utilities.columns = 2;
  storage.utilities.gap = tileGap;
  storage.utilities.tileHeight = tileHeight;
  storage.utilities.inputMask = fui::InputTouch;
  storage.utilities.text = theme.smallText;
  storage.utilities.radius = theme.controlRadius;
  storage.layout.utilityGrid = screen.takeTop(gridHeight);
  fui::tileGrid(screen.frame(), storage.layout.utilityGrid, storage.utilities);
}

}  // namespace library_home
