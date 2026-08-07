#pragma once

#include <cstdint>

class GfxRenderer;

// One action space for every configurable shortcut.
//
// The power button and the reader long-press bindings are persisted as two separate enums with
// different numbering (see CrossPointSettings::SHORT_PWRBTN and LONG_PRESS_ACTION). Both are
// translated into this type at the point of use, so every dispatcher switches over one enum
// instead of repeating a near-identical switch per binding.
enum class ShortcutAction : uint8_t {
  None,
  Sleep,
  PageTurn,
  RefreshScreen,
  Footnotes,
  ToggleBookmark,
  SyncProgress,
  LookUpWord,
  Screenshot,
  FileBrowser,
  FileTransfer,
  ToggleTiltPageTurn,
};

ShortcutAction shortcutFromPowerButtonSetting(uint8_t value);
ShortcutAction shortcutFromLongPressSetting(uint8_t value);

// True when the action needs no open book, and so can run from anywhere. The rest are
// reader-only: they are ignored outside a reader rather than doing something surprising.
bool isShortcutAvailableOutsideReader(ShortcutAction action);

// Runs the actions that need no open book. Returns false for reader-only actions, PageTurn and
// None, all of which are the caller's to handle.
bool runGlobalShortcut(ShortcutAction action, GfxRenderer& renderer);

// Defined in main.cpp, where the sleep sequence lives.
void enterDeepSleep(bool fromTimeout);
