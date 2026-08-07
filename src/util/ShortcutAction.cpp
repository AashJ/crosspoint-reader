#include "ShortcutAction.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "CrossPointSettings.h"
#include "ScreenshotUtil.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

ShortcutAction shortcutFromPowerButtonSetting(const uint8_t value) {
  switch (value) {
    case CrossPointSettings::SLEEP:
      return ShortcutAction::Sleep;
    case CrossPointSettings::PAGE_TURN:
      return ShortcutAction::PageTurn;
    case CrossPointSettings::FORCE_REFRESH:
      return ShortcutAction::RefreshScreen;
    case CrossPointSettings::FOOTNOTES:
      return ShortcutAction::Footnotes;
    case CrossPointSettings::TOGGLE_BOOKMARK:
      return ShortcutAction::ToggleBookmark;
    case CrossPointSettings::SYNC_PROGRESS:
      return ShortcutAction::SyncProgress;
    case CrossPointSettings::SCREENSHOT:
      return ShortcutAction::Screenshot;
    case CrossPointSettings::FILE_BROWSER:
      return ShortcutAction::FileBrowser;
    case CrossPointSettings::LOOKUP_WORD:
      return ShortcutAction::LookUpWord;
    case CrossPointSettings::FILE_TRANSFER:
      return ShortcutAction::FileTransfer;
    case CrossPointSettings::TOGGLE_TILT_PAGE_TURN:
      return ShortcutAction::ToggleTiltPageTurn;
    case CrossPointSettings::IGNORE:
    default:
      return ShortcutAction::None;
  }
}

ShortcutAction shortcutFromLongPressSetting(const uint8_t value) {
  switch (value) {
    case CrossPointSettings::LONG_ACTION_SLEEP:
      return ShortcutAction::Sleep;
    case CrossPointSettings::LONG_ACTION_REFRESH_SCREEN:
      return ShortcutAction::RefreshScreen;
    case CrossPointSettings::LONG_ACTION_TOGGLE_BOOKMARK:
      return ShortcutAction::ToggleBookmark;
    case CrossPointSettings::LONG_ACTION_SYNC_PROGRESS:
      return ShortcutAction::SyncProgress;
    case CrossPointSettings::LONG_ACTION_LOOKUP_WORD:
      return ShortcutAction::LookUpWord;
    case CrossPointSettings::LONG_ACTION_FOOTNOTES:
      return ShortcutAction::Footnotes;
    case CrossPointSettings::LONG_ACTION_SCREENSHOT:
      return ShortcutAction::Screenshot;
    case CrossPointSettings::LONG_ACTION_FILE_BROWSER:
      return ShortcutAction::FileBrowser;
    case CrossPointSettings::LONG_ACTION_FILE_TRANSFER:
      return ShortcutAction::FileTransfer;
    case CrossPointSettings::LONG_ACTION_TOGGLE_TILT_PAGE_TURN:
      return ShortcutAction::ToggleTiltPageTurn;
    case CrossPointSettings::LONG_ACTION_OFF:
    default:
      return ShortcutAction::None;
  }
}

bool isShortcutAvailableOutsideReader(const ShortcutAction action) {
  switch (action) {
    case ShortcutAction::Sleep:
    case ShortcutAction::RefreshScreen:
    case ShortcutAction::Screenshot:
    case ShortcutAction::FileBrowser:
    case ShortcutAction::FileTransfer:
    case ShortcutAction::ToggleTiltPageTurn:
      return true;
    case ShortcutAction::None:
    case ShortcutAction::PageTurn:
    case ShortcutAction::Footnotes:
    case ShortcutAction::ToggleBookmark:
    case ShortcutAction::SyncProgress:
    case ShortcutAction::LookUpWord:
      return false;
  }
  return false;
}

bool runGlobalShortcut(const ShortcutAction action, GfxRenderer& renderer) {
  switch (action) {
    case ShortcutAction::Sleep:
      enterDeepSleep(false);
      return true;
    case ShortcutAction::RefreshScreen:
      // The active activity gets first refusal so it can redraw its own content; only if it
      // declines do we push the existing framebuffer out again.
      if (!activityManager.handleForcedRefresh()) {
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      return true;
    case ShortcutAction::Screenshot:
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    case ShortcutAction::FileBrowser:
      activityManager.goToFileBrowser();
      return true;
    case ShortcutAction::FileTransfer:
      activityManager.goToFileTransfer();
      return true;
    case ShortcutAction::ToggleTiltPageTurn:
      SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == CrossPointSettings::TILT_OFF ? CrossPointSettings::TILT_ON
                                                                                    : CrossPointSettings::TILT_OFF;
      SETTINGS.saveToFile();
      activityManager.requestUpdate();
      return true;
    case ShortcutAction::None:
    case ShortcutAction::PageTurn:
    case ShortcutAction::Footnotes:
    case ShortcutAction::ToggleBookmark:
    case ShortcutAction::SyncProgress:
    case ShortcutAction::LookUpWord:
      return false;
  }
  return false;
}
