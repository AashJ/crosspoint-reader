#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING, SUBMENU };

enum class SettingAction {
  None,
  RemapFrontButtons,
  RemapFrontButtonsReader,
  ControlsPowerButton,
  ControlsFrontButtons,
  ControlsSideButtons,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  TextSettings,
};

struct SettingEnumOption {
  StrId label;
  uint8_t rawValue;
};

// Owns the relationship between a displayed option and its persisted value.
// Callers cannot add one without the other, so reordered and gapped enums stay coherent.
class SettingEnumOptions {
 public:
  SettingEnumOptions() = default;

  explicit SettingEnumOptions(const std::initializer_list<StrId> labels) {
    options.reserve(labels.size());
    uint8_t rawValue = 0;
    for (const StrId label : labels) {
      add(label, rawValue++);
    }
  }

  explicit SettingEnumOptions(const std::initializer_list<SettingEnumOption> mappedOptions,
                              const size_t additionalCapacity = 0) {
    options.reserve(mappedOptions.size() + additionalCapacity);
    for (const auto& option : mappedOptions) {
      add(option.label, option.rawValue);
    }
  }

  void reserve(const size_t count) { options.reserve(count); }
  void add(const StrId label, const uint8_t rawValue) { options.push_back({label, rawValue}); }

  [[nodiscard]] size_t size() const { return options.size(); }
  [[nodiscard]] bool empty() const { return options.empty(); }

  [[nodiscard]] StrId labelAt(const size_t displayIndex) const {
    return displayIndex < options.size() ? options[displayIndex].label : StrId::STR_NONE_OPT;
  }

  [[nodiscard]] uint8_t displayIndexForRawValue(const uint8_t rawValue) const {
    for (size_t i = 0; i < options.size(); i++) {
      if (options[i].rawValue == rawValue) return static_cast<uint8_t>(i);
    }
    return 0;
  }

  [[nodiscard]] uint8_t rawValueForDisplayIndex(const uint8_t displayIndex) const {
    if (options.empty()) return 0;
    return displayIndex < options.size() ? options[displayIndex].rawValue : options.front().rawValue;
  }

  [[nodiscard]] bool containsRawValue(const uint8_t rawValue) const {
    for (const auto& option : options) {
      if (option.rawValue == rawValue) return true;
    }
    return false;
  }

 private:
  std::vector<SettingEnumOption> options;
};

struct SettingInfo {
  StrId nameId = StrId::STR_NONE_OPT;
  SettingType type = SettingType::ACTION;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;
  StrId category = StrId::STR_NONE_OPT;
  bool obfuscated = false;
  bool inTextSettings = false;

  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  SettingInfo& withTextSettings() {
    inTextSettings = true;
    return *this;
  }

  void setEnumStringValues(std::vector<std::string> values) { enumStringValues = std::move(values); }

  [[nodiscard]] bool hasEnumStringValues() const { return !enumStringValues.empty(); }
  [[nodiscard]] const std::vector<std::string>& getEnumStringValues() const { return enumStringValues; }

  [[nodiscard]] size_t enumOptionCount() const {
    return enumStringValues.empty() ? enumOptions.size() : enumStringValues.size();
  }

  [[nodiscard]] StrId enumLabelAt(const size_t displayIndex) const { return enumOptions.labelAt(displayIndex); }

  [[nodiscard]] uint8_t enumDisplayIndexForRawValue(const uint8_t rawValue) const {
    return enumOptions.displayIndexForRawValue(rawValue);
  }

  [[nodiscard]] uint8_t enumRawValueForDisplayIndex(const uint8_t displayIndex) const {
    return enumOptions.rawValueForDisplayIndex(displayIndex);
  }

  [[nodiscard]] bool isEnumRawValueAllowed(const uint8_t rawValue) const {
    return enumOptions.containsRawValue(rawValue);
  }

  // OptionPopup outlives the selected list entry, so its callback owns a compact copy.
  [[nodiscard]] SettingEnumOptions enumOptionsSnapshot() const { return enumOptions; }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::initializer_list<StrId> labels,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumOptions = SettingEnumOptions(labels);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo MappedEnum(StrId nameId, uint8_t CrossPointSettings::* ptr,
                                std::initializer_list<SettingEnumOption> options, const char* key = nullptr,
                                StrId category = StrId::STR_NONE_OPT) {
    return MappedEnum(nameId, ptr, SettingEnumOptions(options), key, category);
  }

  static SettingInfo MappedEnum(StrId nameId, uint8_t CrossPointSettings::* ptr, SettingEnumOptions options,
                                const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumOptions = std::move(options);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Submenu(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SUBMENU;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::initializer_list<StrId> labels, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s = Enum(nameId, nullptr, labels, key, category);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

 private:
  SettingEnumOptions enumOptions;
  std::vector<std::string> enumStringValues;
};
