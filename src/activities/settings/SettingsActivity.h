#pragma once
#include <vector>

#include "SettingInfo.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class SettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedCategoryIndex = 0;  // Currently selected category
  int selectedSettingIndex = 0;
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  // Controls sub-lists, reached from the SUBMENU entries in controlsSettings.
  std::vector<SettingInfo> controlsPowerSettings;
  std::vector<SettingInfo> controlsFrontButtonSettings;
  std::vector<SettingInfo> controlsSideButtonSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  // None while the category's top-level list is showing.
  SettingAction activeSubmenu = SettingAction::None;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  // Points currentSettings at the list for the active category and submenu, and refreshes
  // settingsCount. The single place that decides which list is on screen.
  void applyCurrentSettingsList();
  void openSubmenu(SettingAction action);
  void closeSubmenu();
  // Title for the active submenu, or STR_NONE_OPT at the top level.
  StrId activeSubmenuTitleId() const;
  void buildControlsLists();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Settings", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
