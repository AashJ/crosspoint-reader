#include <FreeInkApp.h>
#include <FreeInkUICore.h>

#include <cstdio>

#include "components/LibraryHomeScreen.h"

namespace fui = freeink::ui;

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

#define CHECK_EQ(actual, expected)                                                               \
  do {                                                                                           \
    ++checksRun;                                                                                 \
    const auto actualValue = (actual);                                                           \
    const auto expectedValue = (expected);                                                       \
    if (!(actualValue == expectedValue)) {                                                       \
      ++checksFailed;                                                                            \
      std::printf("FAIL %s:%d  %s == %s (%ld != %ld)\n", __FILE__, __LINE__, #actual, #expected, \
                  static_cast<long>(actualValue), static_cast<long>(expectedValue));             \
    }                                                                                            \
  } while (0)

class FakeTarget final : public fui::DrawTarget {
 public:
  fui::Size measureText(fui::FontId, const char* textValue, fui::TextStyle) const override {
    int16_t width = 0;
    while (textValue && *textValue++) width += 6;
    return fui::Size{width, 12};
  }
  int16_t lineHeight(fui::FontId) const override { return 12; }
  void fill(fui::Rect, fui::Paint, uint8_t, uint8_t) override {}
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t, uint8_t) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect, const char*, fui::TextStyle) override {}
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override {}
};

struct Harness {
  using App = fui::FreeInkApp<24, 6>;

  FakeTarget target;
  App app;
  library_home::ScreenStorage storage;
  library_home::Model model;
  fui::TileGridItem utilities[4];
  const char* titles[8] = {"Dune",    "Beloved",   "Piranesi", "The Dispossessed",
                           "Kindred", "Wolf Hall", "Orlando",  "The Left Hand of Darkness"};

  Harness() : app(target, device()) {
    model.labels = {"Menu", "No books yet"};
    model.bookProvider = &Harness::bookAt;
    model.bookProviderUserData = this;
    model.bookCount = 8;
    model.selectedBook = 0;
    model.topChrome = 64;
    model.bottomChrome = 40;
    for (int i = 0; i < 4; ++i) {
      utilities[i].label = i == 0 ? "Add Books" : i == 1 ? "Browse Files" : i == 2 ? "Online Library" : "Settings";
      utilities[i].value = static_cast<int16_t>(i);
    }
    model.utilityItems = utilities;
    model.utilityCount = 4;
    app.setScreen(&Harness::screen, this, fui::RefreshHint::Fast);
  }

  static fui::DeviceContext device() {
    fui::DeviceContext value;
    value.width = 480;
    value.height = 800;
    value.hasTouch = true;
    value.hasButtons = true;
    return value;
  }

  static fui::CoverGridItem bookAt(const uint16_t index, void* user) {
    auto* self = static_cast<Harness*>(user);
    return fui::coverGridItem(self->titles[index], static_cast<int16_t>(index));
  }

  static void screen(App::ScreenType& screenValue, void* user) {
    auto* self = static_cast<Harness*>(user);
    library_home::build(screenValue, self->model, self->storage);
  }

  fui::ActionEvent tap(const fui::Rect rect) {
    fui::InputSnapshot input;
    input.touchReleased = true;
    input.touchX = static_cast<int16_t>(rect.x + rect.width / 2);
    input.touchY = static_cast<int16_t>(rect.y + rect.height / 2);
    return app.route(input);
  }
};

void testPortraitLayout() {
  fui::ThemeTokens theme = fui::themeTokensForLineHeight(12);
  const auto layout = library_home::computeLayout(fui::Rect{12, 64, 456, 684}, theme, 12);
  CHECK_EQ(layout.columns, 3);
  CHECK_EQ(layout.visibleBooks, 6);
  CHECK(layout.coverSize.width > 0);
  CHECK(layout.coverSize.height > layout.coverSize.width);
}

void testLandscapeLayout() {
  fui::ThemeTokens theme = fui::themeTokensForLineHeight(12);
  const auto layout = library_home::computeLayout(fui::Rect{12, 64, 776, 364}, theme, 12);
  CHECK_EQ(layout.columns, 5);
  CHECK_EQ(layout.visibleBooks, 5);
}

void testBookAndMenuRouting() {
  Harness harness;
  harness.app.render();
  const fui::Rect grid = harness.storage.layout.grid;
  const int16_t cellWidth = static_cast<int16_t>((grid.width - 2 * harness.storage.covers.gap) / 3);
  CHECK_EQ(harness.tap(fui::Rect{grid.x, grid.y, cellWidth, harness.storage.layout.rowHeight}).action,
           library_home::ACTION_BOOK);
  harness.app.render();
  CHECK_EQ(harness.tap(harness.storage.layout.menuButton).action, library_home::ACTION_MENU);
}

void testSheetRouting() {
  Harness harness;
  harness.model.menuOpen = true;
  harness.app.render();
  const fui::Rect grid = harness.storage.layout.utilityGrid;
  const int16_t tileWidth = static_cast<int16_t>((grid.width - harness.storage.utilities.gap) / 2);
  const auto utility = harness.tap(fui::Rect{grid.x, grid.y, tileWidth, harness.storage.utilities.tileHeight});
  CHECK_EQ(utility.action, library_home::ACTION_UTILITY);
  CHECK_EQ(utility.value, 0);
  harness.app.render();
  CHECK_EQ(harness.tap(fui::Rect{0, 0, 480, static_cast<int16_t>(grid.y - 1)}).action, library_home::ACTION_DISMISS);
}

void testGestures() {
  CHECK(fui::edgeSwipe(fui::ScreenEdge::Bottom, 240, 790, 240, 610, 480, 800));
  CHECK(!fui::edgeSwipe(fui::ScreenEdge::Bottom, 240, 500, 240, 320, 480, 800));
  CHECK_EQ(fui::swipeDirection(420, 400, 100, 400), fui::SwipeDir::Left);
  CHECK_EQ(fui::swipeDirection(100, 400, 420, 400), fui::SwipeDir::Right);
}

}  // namespace

int main() {
  testPortraitLayout();
  testLandscapeLayout();
  testBookAndMenuRouting();
  testSheetRouting();
  testGestures();
  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
