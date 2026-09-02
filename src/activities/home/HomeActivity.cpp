#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {

size_t maximumCoverRegionBytes(const fui::Rect rect) {
  if (rect.width <= 0 || rect.height <= 0) return 0;
  // Region copies widen the physical x span to byte boundaries. Account for
  // the worst starting bit in both unrotated and 90-degree orientations.
  const size_t unrotatedRowBytes = (static_cast<size_t>(rect.width) + 14) / 8;
  const size_t rotatedRowBytes = (static_cast<size_t>(rect.height) + 14) / 8;
  return std::max(unrotatedRowBytes * static_cast<size_t>(rect.height),
                  rotatedRowBytes * static_cast<size_t>(rect.width));
}

bool sameRect(const fui::Rect left, const fui::Rect right) {
  return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

}  // namespace

void HomeActivity::CoverCacheDeleter::operator()(uint8_t* ptr) const { heap_caps_free(ptr); }

void HomeActivity::loadRecentBooks(const int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    if (recentBooks.size() >= static_cast<size_t>(maxBooks)) break;
    if (!RecentBooksStore::isMissing(book)) recentBooks.push_back(book);
  }
}

void HomeActivity::refreshCoverPaths(const int requestedCoverHeight) {
  for (std::string& path : coverPaths) path.clear();
  for (size_t i = 0; i < recentBooks.size() && i < coverPaths.size(); ++i) {
    if (!recentBooks[i].coverBmpPath.empty()) {
      coverPaths[i] = UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, requestedCoverHeight);
    }
  }
  coverHeight = requestedCoverHeight;
}

void HomeActivity::loadRecentCovers(const int requestedCoverHeight) {
  if (requestedCoverHeight <= 0 || recentBooks.empty()) {
    recentsLoaded = true;
    return;
  }

  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  int progress = 0;
  bool needsRedraw = coverHeight != requestedCoverHeight || loadedCoverTopIndex == UINT16_MAX;

  refreshCoverPaths(requestedCoverHeight);
  const uint16_t first = std::min<uint16_t>(topIndex, static_cast<uint16_t>(recentBooks.size()));
  const uint16_t end = library_home::visibleBookEnd(first, static_cast<uint16_t>(recentBooks.size()), visibleBooks);
  const int visibleCount = std::max<int>(1, end - first);
  for (uint16_t i = first; i < end; ++i) {
    RecentBook& book = recentBooks[i];
    if (!book.coverBmpPath.empty()) {
      const std::string& coverPath = coverPaths[i];
      if (!Storage.exists(coverPath.c_str())) {
        bool success = false;
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          if (epub.load(false, true)) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / visibleCount));
            success = epub.generateThumbBmp(requestedCoverHeight);
            needsRedraw = needsRedraw || success;
          }
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / visibleCount));
            success = xtc.generateThumbBmp(requestedCoverHeight);
            needsRedraw = needsRedraw || success;
          }
        }

        if (!success) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
          book.coverBmpPath.clear();
        }
      }
    }
    ++progress;
  }

  refreshCoverPaths(requestedCoverHeight);
  loadedCoverTopIndex = first;
  recentsLoaded = true;
  recentsLoading = false;
  const bool hasCover =
      std::any_of(coverPaths.begin(), coverPaths.end(), [](const std::string& path) { return !path.empty(); });
  if (needsRedraw && hasCover) requestUpdate();
}

void HomeActivity::configureUtilities() {
  utilityCount = 0;
  auto addUtility = [this](const char* label, const Utility utility, const UIIcon icon) {
    auto& item = utilityItems[utilityCount++];
    item.label = label;
    item.icon = listIconFor(icon, 32);
    item.value = utility;
  };

  addUtility(tr(STR_ADD_BOOKS), AddBooks, Transfer);
  addUtility(tr(STR_BROWSE_FILES), BrowseFiles, Folder);
  if (hasOpdsServers) addUtility(tr(STR_OPDS_BROWSER), OnlineLibrary, Library);
  addUtility(tr(STR_SETTINGS_TITLE), Settings, UIIcon::Settings);
}

int HomeActivity::utilityIndexFor(const HomeMenuItem item) const {
  Utility utility;
  switch (item) {
    case HomeMenuItem::FILE_TRANSFER:
      utility = AddBooks;
      break;
    case HomeMenuItem::FILE_BROWSER:
      utility = BrowseFiles;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      utility = OnlineLibrary;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      utility = Settings;
      break;
    default:
      return 0;
  }
  for (uint16_t i = 0; i < utilityCount; ++i) {
    if (utilityItems[i].value == utility) return i;
  }
  return 0;
}

void HomeActivity::onEnter() {
  Activity::onEnter();
  hasOpdsServers = OPDS_STORE.hasServers();
  loadRecentBooks(kMaxLibraryBooks);
  configureUtilities();
  selectorIndex = 0;

  if (initialMenuItem != HomeMenuItem::NONE && initialMenuItem != HomeMenuItem::RECENTS) {
    menuOpen = true;
    selectedUtility = utilityIndexFor(initialMenuItem);
  }

  resetUi();
  app.on(library_home::ACTION_BOOK, &HomeActivity::onBookEvent, this);
  app.on(library_home::ACTION_MENU, &HomeActivity::onMenuEvent, this);
  app.on(library_home::ACTION_UTILITY, &HomeActivity::onUtilityEvent, this);
  app.on(library_home::ACTION_DISMISS, &HomeActivity::onDismissEvent, this);
  app.setScreen(&HomeActivity::homeScreen, this, fui::RefreshHint::Fast);
  requestUpdate();
}

void HomeActivity::onExit() {
  closeRouting();
  clearCoverCache();
  recentBooks.clear();
  for (std::string& path : coverPaths) path.clear();
  Activity::onExit();
}

void HomeActivity::homeScreen(UiScreen& screen, void* user) { static_cast<HomeActivity*>(user)->buildScreen(screen); }

fui::CoverGridItem HomeActivity::provideBook(const uint16_t index, void* user) {
  const auto* self = static_cast<HomeActivity*>(user);
  if (index >= self->recentBooks.size()) return {};
  return fui::coverGridItem(self->recentBooks[index].title.c_str(), static_cast<int16_t>(index));
}

bool HomeActivity::paintCover(fui::DrawTarget&, const fui::Rect rect, const fui::CoverGridItem&, const uint16_t index,
                              void* user) {
  auto* self = static_cast<HomeActivity*>(user);
  if (index >= self->recentBooks.size() || index >= self->coverPaths.size() || self->coverPaths[index].empty()) {
    return false;
  }

  const uint16_t visibleSlot = index >= self->topIndex ? static_cast<uint16_t>(index - self->topIndex) : UINT16_MAX;
  CoverCacheEntry* cacheEntry =
      visibleSlot < self->coverCacheEntries.size() ? &self->coverCacheEntries[visibleSlot] : nullptr;
  if (cacheEntry && cacheEntry->valid && cacheEntry->bookIndex == index &&
      cacheEntry->thumbnailHeight == self->coverHeight && sameRect(cacheEntry->rect, rect) && self->coverCache &&
      self->renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height,
                                        self->coverCache.get() + visibleSlot * self->coverCacheSlotSize,
                                        self->coverCacheSlotSize)) {
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", self->coverPaths[index], file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  self->renderer.drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height);

  if (cacheEntry && self->ensureCoverCache(rect)) {
    uint8_t* slot = self->coverCache.get() + visibleSlot * self->coverCacheSlotSize;
    cacheEntry->valid =
        self->renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, slot, self->coverCacheSlotSize);
    if (cacheEntry->valid) {
      cacheEntry->bookIndex = index;
      cacheEntry->thumbnailHeight = self->coverHeight;
      cacheEntry->rect = rect;
    }
  }
  return true;
}

bool HomeActivity::ensureCoverCache(const fui::Rect coverRect) {
  const size_t requiredSlotSize = maximumCoverRegionBytes(coverRect);
  if (requiredSlotSize == 0 || coverCacheAllocationFailed) return false;
  if (coverCache && coverCacheSlotSize >= requiredSlotSize) return true;

  clearCoverCache();
  const size_t requiredBytes = requiredSlotSize * coverCacheEntries.size();
#if defined(BOARD_HAS_PSRAM)
  constexpr uint32_t cacheCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
  constexpr uint32_t cacheCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
  // The capability allocator keeps X4 Pro cover pixels in PSRAM instead of
  // consuming internal DRAM. unique_ptr owns the allocation immediately and
  // releases it through heap_caps_free on every exit path.
  coverCache.reset(static_cast<uint8_t*>(heap_caps_malloc(requiredBytes, cacheCaps)));
  if (!coverCache) {
    coverCacheAllocationFailed = true;
    LOG_ERR("HOME", "OOM: cover cache (%u bytes)", static_cast<unsigned>(requiredBytes));
    return false;
  }
  coverCacheSlotSize = requiredSlotSize;
#if defined(BOARD_HAS_PSRAM)
  LOG_DBG("HOME", "Cover cache: %u bytes in PSRAM", static_cast<unsigned>(requiredBytes));
#else
  LOG_DBG("HOME", "Cover cache: %u bytes in internal RAM", static_cast<unsigned>(requiredBytes));
#endif
  return true;
}

void HomeActivity::clearCoverCache() {
  coverCache.reset();
  coverCacheSlotSize = 0;
  for (CoverCacheEntry& entry : coverCacheEntries) entry = {};
}

void HomeActivity::buildScreen(UiScreen& screen) {
  for (uint16_t i = 0; i < utilityCount; ++i) {
    utilityItems[i].state = menuOpen && i == selectedUtility ? fui::StateSelected : fui::StateNormal;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  library_home::Model model;
  model.labels.menu = tr(STR_LIBRARY_MENU);
  model.labels.empty = tr(STR_LIBRARY_EMPTY);
  model.bookProvider = &HomeActivity::provideBook;
  model.bookProviderUserData = this;
  model.coverPainter = &HomeActivity::paintCover;
  model.coverPainterUserData = this;
  model.utilityItems = utilityItems;
  model.bookCount = static_cast<uint16_t>(recentBooks.size());
  model.topIndex = topIndex;
  model.utilityCount = utilityCount;
  model.selectedBook = selectorIndex < static_cast<int>(recentBooks.size()) ? selectorIndex : -1;
  model.topChrome = static_cast<int16_t>(metrics.homeTopPadding);
  model.bottomChrome = static_cast<int16_t>(metrics.buttonHintsHeight);
  model.menuSelected = selectorIndex == static_cast<int>(recentBooks.size());
  model.menuOpen = menuOpen;
  library_home::build(screen, model, screenStorage);

  visibleBooks = std::max<uint16_t>(1, screenStorage.layout.visibleBooks);
}

void HomeActivity::onBookEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<HomeActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int>(self->recentBooks.size())) return;
  self->selectorIndex = event.value;
  activityManager.goToReader(self->recentBooks[event.value].path);
}

void HomeActivity::onMenuEvent(const fui::ActionEvent&, void* user) { static_cast<HomeActivity*>(user)->openMenu(); }

void HomeActivity::onUtilityEvent(const fui::ActionEvent& event, void* user) {
  static_cast<HomeActivity*>(user)->activateUtility(event.value);
}

void HomeActivity::onDismissEvent(const fui::ActionEvent&, void* user) {
  static_cast<HomeActivity*>(user)->closeMenu();
}

void HomeActivity::openMenu(const int utilityIndex) {
  menuOpen = true;
  selectedUtility = std::clamp(utilityIndex, 0, std::max(0, static_cast<int>(utilityCount) - 1));
  closeRouting();
  requestUpdate();
}

void HomeActivity::closeMenu() {
  menuOpen = false;
  selectorIndex = static_cast<int>(recentBooks.size());
  closeRouting();
  requestUpdate();
}

bool HomeActivity::handleHomeGesture() {
  if (menuOpen) {
    closeMenu();
  } else {
    openMenu();
  }
  return true;
}

void HomeActivity::syncBookPage() {
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  topIndex = fui::coverGridTopIndexFor(static_cast<uint16_t>(selectorIndex), static_cast<uint16_t>(recentBooks.size()),
                                       screenStorage.layout.columns, visibleBooks);
}

void HomeActivity::selectNextPage(const bool forward) {
  if (recentBooks.empty()) return;
  const int count = static_cast<int>(recentBooks.size());
  if (selectorIndex >= count) selectorIndex = forward ? 0 : count - 1;
  selectorIndex = forward ? ButtonNavigator::nextPageIndex(selectorIndex, count, visibleBooks)
                          : ButtonNavigator::previousPageIndex(selectorIndex, count, visibleBooks);
  syncBookPage();
  requestUpdate();
}

void HomeActivity::activateSelection() {
  if (selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size())) {
    activityManager.goToReader(recentBooks[selectorIndex].path);
  } else {
    openMenu();
  }
}

void HomeActivity::activateUtility(const int utility) {
  switch (utility) {
    case AddBooks:
      activityManager.goToFileTransfer();
      break;
    case BrowseFiles:
      activityManager.goToFileBrowser();
      break;
    case OnlineLibrary:
      if (hasOpdsServers) activityManager.goToBrowser();
      break;
    case Settings:
      activityManager.goToSettings();
      break;
    default:
      break;
  }
}

void HomeActivity::loop() {
  const auto touch = routeTouch(mappedInput);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) return;
  }

  if (menuOpen) {
    buttonNavigator.onNext([this] {
      selectedUtility = ButtonNavigator::nextIndex(selectedUtility, utilityCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      selectedUtility = ButtonNavigator::previousIndex(selectedUtility, utilityCount);
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeMenu();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateUtility(utilityItems[selectedUtility].value);
    }
    return;
  }

  const int focusableCount = static_cast<int>(recentBooks.size()) + 1;
  buttonNavigator.onNext([this, focusableCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, focusableCount);
    syncBookPage();
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, focusableCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, focusableCount);
    syncBookPage();
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    selectNextPage(true);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    selectNextPage(false);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !recentBooks.empty()) {
    activityManager.goToReader(recentBooks[0].path);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelection();
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 tr(STR_LIBRARY));
  if (!menuOpen) {
    const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                              tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderUi();

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  const int renderedCoverHeight = screenStorage.layout.coverSize.height;
  if (!firstRenderDone) {
    firstRenderDone = true;
    loadRecentCovers(renderedCoverHeight);
  } else if ((!recentsLoaded || loadedCoverTopIndex != topIndex) && !recentsLoading) {
    loadRecentCovers(renderedCoverHeight);
  } else if (coverHeight > 0 && renderedCoverHeight != coverHeight) {
    recentsLoaded = false;
    loadRecentCovers(renderedCoverHeight);
  }
}
