#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan /dictionaries on every rebuild: cheap (one directory listing) and
  // picks up dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      // Grouping and per-setting visibility are decided in buildControlsLists().
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  if (!BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
    // The reader override is only worth offering once it is switched on.
    if (SETTINGS.readerFrontButtonsEnabled) {
      controlsSettings.insert(
          controlsSettings.begin() + 1,
          SettingInfo::Action(StrId::STR_REMAP_BUTTONS_READER, SettingAction::RemapFrontButtonsReader));
    }
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  // TODO: Touch devices need their own firmware update path/artifacts before OTA is exposed.
  if (!BoardConfig::hasTouch()) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  buildControlsLists();
  applyCurrentSettingsList();
}

// Splits the flat Controls category into the grouped lists the UI navigates: a parent list of
// submenu rows plus one list per group. Entries are moved out of controlsSettings, so anything
// left there stays at the top level.
void SettingsActivity::buildControlsLists() {
  controlsPowerSettings.clear();
  controlsFrontButtonSettings.clear();
  controlsSideButtonSettings.clear();

  const bool hasFrontButtons = !BoardConfig::hasTouch();

  // Quick-return from footnotes only means anything while some shortcut can open them.
  const bool anyFootnoteShortcut = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES ||
                                   SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES ||
                                   SETTINGS.longPressMenuAction == CrossPointSettings::LONG_ACTION_FOOTNOTES ||
                                   SETTINGS.longPressBackAction == CrossPointSettings::LONG_ACTION_FOOTNOTES;

  // Quick-return from footnotes is the one Controls entry that hides itself: it is meaningless
  // unless a shortcut can open footnotes in the first place.
  auto isHidden = [&](const SettingInfo& s) {
    return s.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack && !anyFootnoteShortcut;
  };

  // Returns the group a setting belongs in, or nullptr to leave it at the category's top level.
  auto groupFor = [&](const SettingInfo& s) -> std::vector<SettingInfo>* {
    if (s.valuePtr == &CrossPointSettings::shortPwrBtn || s.valuePtr == &CrossPointSettings::longPwrBtn ||
        s.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack) {
      return &controlsPowerSettings;
    }
    if (s.valuePtr == &CrossPointSettings::sideButtonLayout ||
        s.valuePtr == &CrossPointSettings::sideButtonOrientationAware ||
        s.valuePtr == &CrossPointSettings::sideButtonLongPress) {
      return &controlsSideButtonSettings;
    }
    // Without front buttons there is no Front Buttons group, so anything that survived the
    // device filter in SettingsList (longPressButtonBehavior, which also covers touch
    // page-turn zones) stays at the top level.
    if (!hasFrontButtons) return nullptr;
    if (s.valuePtr == &CrossPointSettings::frontButtonOrientationAware ||
        s.valuePtr == &CrossPointSettings::readerFrontButtonsEnabled ||
        s.valuePtr == &CrossPointSettings::longPressButtonBehavior ||
        s.valuePtr == &CrossPointSettings::longPressMenuAction ||
        s.valuePtr == &CrossPointSettings::longPressBackAction ||
        s.valuePtr == &CrossPointSettings::backShortToFileBrowser || s.action == SettingAction::RemapFrontButtons ||
        s.action == SettingAction::RemapFrontButtonsReader) {
      return &controlsFrontButtonSettings;
    }
    return nullptr;
  };

  std::vector<SettingInfo> topLevel;
  topLevel.reserve(controlsSettings.size());
  for (auto& setting : controlsSettings) {
    if (isHidden(setting)) continue;
    auto* group = groupFor(setting);
    if (group) {
      group->push_back(std::move(setting));
    } else {
      topLevel.push_back(std::move(setting));
    }
  }

  controlsSettings.clear();
  controlsSettings.reserve(topLevel.size() + 3);
  controlsSettings.push_back(SettingInfo::Submenu(StrId::STR_POWER_BUTTON, SettingAction::ControlsPowerButton));
  if (hasFrontButtons) {
    controlsSettings.push_back(SettingInfo::Submenu(StrId::STR_FRONT_BUTTONS, SettingAction::ControlsFrontButtons));
  }
  controlsSettings.push_back(SettingInfo::Submenu(StrId::STR_SIDE_BUTTONS, SettingAction::ControlsSideButtons));
  for (auto& setting : topLevel) {
    controlsSettings.push_back(std::move(setting));
  }
}

void SettingsActivity::applyCurrentSettingsList() {
  switch (activeSubmenu) {
    case SettingAction::ControlsPowerButton:
      currentSettings = &controlsPowerSettings;
      break;
    case SettingAction::ControlsFrontButtons:
      currentSettings = &controlsFrontButtonSettings;
      break;
    case SettingAction::ControlsSideButtons:
      currentSettings = &controlsSideButtonSettings;
      break;
    default:
      switch (selectedCategoryIndex) {
        case 0:
          currentSettings = &displaySettings;
          break;
        case 1:
          currentSettings = &readerSettings;
          break;
        case 2:
          currentSettings = &controlsSettings;
          break;
        case 3:
        default:
          currentSettings = &systemSettings;
          break;
      }
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

StrId SettingsActivity::activeSubmenuTitleId() const {
  switch (activeSubmenu) {
    case SettingAction::ControlsPowerButton:
      return StrId::STR_POWER_BUTTON;
    case SettingAction::ControlsFrontButtons:
      return StrId::STR_FRONT_BUTTONS;
    case SettingAction::ControlsSideButtons:
      return StrId::STR_SIDE_BUTTONS;
    default:
      return StrId::STR_NONE_OPT;
  }
}

void SettingsActivity::openSubmenu(const SettingAction action) {
  activeSubmenu = action;
  applyCurrentSettingsList();
  selectedSettingIndex = settingsCount > 0 ? 1 : 0;
  requestUpdate();
}

void SettingsActivity::closeSubmenu() {
  activeSubmenu = SettingAction::None;
  applyCurrentSettingsList();
  selectedSettingIndex = settingsCount > 0 ? 1 : 0;
  requestUpdate();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  activeSubmenu = SettingAction::None;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  bool hasChangedCategory = false;

  // Switching category always drops back to that category's top level.
  auto applyCategorySelection = [this] {
    activeSubmenu = SettingAction::None;
    applyCurrentSettingsList();
  };

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
    } else if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int tx = 0;
  int ty = 0;
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight =
      renderer.getScreenHeight() - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                                    metrics.buttonHintsHeight + metrics.verticalSpacing * 2);
  auto buildTabs = [&]() {
    std::vector<TabInfo> tabs;
    tabs.reserve(categoryCount);
    for (int i = 0; i < categoryCount; i++) {
      tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
    }
    return tabs;
  };
  auto settingIndexFromPoint = [&](const int x, const int y, int& settingIndex) {
    (void)x;
    if (settingsCount <= 0 || y < listTop || y >= listTop + listHeight) return false;
    const int rowStep = GUI.getListRowStep(false);
    if (rowStep <= 0) return false;
    const int pageItems = GUI.getListPageItems(listHeight, false);
    const int selectedRow = std::max(0, selectedSettingIndex - 1);
    const int pageStart = selectedRow / pageItems * pageItems;
    const int row = (y - listTop) / rowStep;
    const int touched = pageStart + row;
    if (row < 0 || row >= pageItems || touched < 0 || touched >= settingsCount) return false;
    settingIndex = touched + 1;
    return true;
  };

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    int touchedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              touchedCategory)) {
      if (selectedCategoryIndex != touchedCategory || selectedSettingIndex != 0) {
        selectedCategoryIndex = touchedCategory;
        selectedSettingIndex = 0;
        applyCategorySelection();
        requestUpdate();
      }
      return;
    }

    int touchedSetting = -1;
    if (settingIndexFromPoint(tx, ty, touchedSetting)) {
      if (selectedSettingIndex != touchedSetting) {
        selectedSettingIndex = touchedSetting;
        requestUpdate();
      }
      return;
    }
  }

  if (mappedInput.wasScreenTapped(tx, ty)) {
    int tappedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              tappedCategory)) {
      selectedCategoryIndex = tappedCategory;
      selectedSettingIndex = 0;
      applyCategorySelection();
      requestUpdate();
      return;
    }

    int tappedSetting = -1;
    if (settingIndexFromPoint(tx, ty, tappedSetting)) {
      selectedSettingIndex = tappedSetting;
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  // Handle navigation
  const auto& navMetrics = UITheme::getInstance().getMetrics();
  const int settingsListHeight =
      renderer.getScreenHeight() - (navMetrics.topPadding + navMetrics.headerHeight + navMetrics.tabBarHeight +
                                    navMetrics.buttonHintsHeight + navMetrics.verticalSpacing * 2);
  const int settingsPageItems = GUI.getListPageItems(settingsListHeight, false);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedSettingIndex = selectedSettingIndex == 0 ? 1
                                                     : ButtonNavigator::nextPageIndex(
                                                           selectedSettingIndex, settingsCount + 1, settingsPageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedSettingIndex =
        ButtonNavigator::previousPageIndex(selectedSettingIndex, settingsCount + 1, settingsPageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    applyCategorySelection();
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t optionCount = static_cast<uint8_t>(settingEnumOptionCount(setting));
    if (optionCount == 0) return;
    const uint8_t currentIndex = settingEnumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
    if (optionCount > 2) {
      const auto valuePtr = setting.valuePtr;
      const auto rawValues = setting.enumRawValues;
      optionPopup.show(
          setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), currentIndex,
          [this, valuePtr, rawValues, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
            // The popup outlives the list rebuild that invalidates `setting`, so it
            // carries its own copy of the raw values rather than capturing the entry.
            SETTINGS.*valuePtr =
                rawValues.empty() ? static_cast<uint8_t>(idx)
                                  : (static_cast<size_t>(idx) < rawValues.size() ? rawValues[idx] : rawValues.front());
            syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
            SETTINGS.saveToFile();
            rebuildSettingsLists();
          });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) =
        settingEnumRawValueForDisplayIndex(setting, static_cast<uint8_t>((currentIndex + 1) % optionCount));
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::SUBMENU) {
    openSubmenu(setting.action);
    return;
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::RemapFrontButtonsReader:
        startActivityForResult(
            std::make_unique<ButtonRemapActivity>(renderer, mappedInput, ButtonRemapActivity::Target::Reader),
            resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 // TextSettingsActivity saves on each change; no save needed here.
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ControlsPowerButton:
      case SettingAction::ControlsFrontButtons:
      case SettingAction::ControlsSideButtons:
      case SettingAction::None:
        // Submenu rows never reach here — they are handled as SettingType::SUBMENU above.
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  // Inside a group the header names it, so the user can tell where Back will take them.
  const StrId submenuTitleId = activeSubmenuTitleId();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 submenuTitleId == StrId::STR_NONE_OPT ? tr(STR_SETTINGS_TITLE) : I18N.get(submenuTitleId),
                 CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::SUBMENU) {
          valueText = ">";
        } else if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t index = settingEnumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
          if (index < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[index]);
          }
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        }
        return valueText;
      },
      true);

  // Draw help text
  const SettingInfo* selected = (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount)
                                    ? &(*currentSettings)[selectedSettingIndex - 1]
                                    : nullptr;
  const auto confirmLabel = (selectedSettingIndex == 0)
                                ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                                : (selected == nullptr                            ? tr(STR_TOGGLE)
                                   : selected->type == SettingType::SUBMENU       ? tr(STR_SELECT)
                                   : selected->nameId == StrId::STR_TIME_TO_SLEEP ? tr(STR_SELECT)
                                                                                  : tr(STR_TOGGLE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
