#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/LibraryHomeScreen.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

struct RecentBook;

class HomeActivity final : public Activity, protected UiAppHost {
  static constexpr int kMaxLibraryBooks = 10;
  static constexpr int kMaxUtilities = 4;
#if defined(BOARD_HAS_PSRAM)
  static constexpr size_t kCoverCacheSlots = 6;
#else
  static constexpr size_t kCoverCacheSlots = 3;
#endif

  struct CoverCacheDeleter {
    void operator()(uint8_t* ptr) const;
  };

  struct CoverCacheEntry {
    freeink::ui::Rect rect{};
    uint16_t bookIndex = UINT16_MAX;
    int thumbnailHeight = 0;
    bool valid = false;
  };

  enum Utility : int16_t {
    AddBooks = 0,
    BrowseFiles = 1,
    OnlineLibrary = 2,
    Settings = 3,
  };

  ButtonNavigator buttonNavigator;
  std::vector<RecentBook> recentBooks;
  // Resolved once after thumbnail generation so ordinary selection repaints
  // do not construct temporary path strings in the render hot path.
  std::array<std::string, kMaxLibraryBooks> coverPaths;
  std::unique_ptr<uint8_t[], CoverCacheDeleter> coverCache;
  std::array<CoverCacheEntry, kCoverCacheSlots> coverCacheEntries{};
  library_home::ScreenStorage screenStorage;
  freeink::ui::TileGridItem utilityItems[kMaxUtilities];

  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  int selectorIndex = 0;
  int selectedUtility = 0;
  uint16_t topIndex = 0;
  uint16_t visibleBooks = 6;
  uint16_t utilityCount = 0;
  uint16_t loadedCoverTopIndex = UINT16_MAX;
  size_t coverCacheSlotSize = 0;
  int coverHeight = 0;
  bool menuOpen = false;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverCacheAllocationFailed = false;

  static void homeScreen(UiScreen& screen, void* user);
  static void onBookEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onMenuEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onUtilityEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onDismissEvent(const freeink::ui::ActionEvent& event, void* user);
  static freeink::ui::CoverGridItem provideBook(uint16_t index, void* user);
  static bool paintCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                         const freeink::ui::CoverGridItem& item, uint16_t index, void* user);

  void buildScreen(UiScreen& screen);
  void configureUtilities();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int requestedCoverHeight);
  void refreshCoverPaths(int requestedCoverHeight);
  bool ensureCoverCache(freeink::ui::Rect coverRect);
  void clearCoverCache();
  void selectNextPage(bool forward);
  void syncBookPage();
  void openMenu(int utilityIndex = 0);
  void closeMenu();
  void activateSelection();
  void activateUtility(int utility);
  int utilityIndexFor(HomeMenuItem item) const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        UiAppHost(renderer),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
  bool handleHomeGesture() override;
};
