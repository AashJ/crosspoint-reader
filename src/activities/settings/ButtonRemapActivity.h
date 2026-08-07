#pragma once

#include <array>
#include <functional>
#include <string>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class ButtonRemapActivity final : public Activity {
 public:
  // Which of the two front-button mappings this run edits: the system-wide one, or the
  // reader-only override that shadows it while a book is open.
  enum class Target : uint8_t { System, Reader };

  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Target target = Target::System)
      : Activity("ButtonRemap", renderer, mappedInput), target(target) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Rendering task state.

  Target target;
  // The four role fields this run writes, in Back/Confirm/Left/Right order.
  std::array<uint8_t CrossPointSettings::*, 4> targetFields() const;
  // Index of the logical role currently awaiting input.
  uint8_t currentStep = 0;
  // Temporary mapping from logical role -> hardware button index.
  uint8_t tempMapping[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  // Error banner timing (used when reassigning duplicate buttons).
  unsigned long errorUntil = 0;
  std::string errorMessage;

  // Commit temporary mapping to settings.
  void applyTempMapping();
  // Returns false if a hardware button is already assigned to a different role.
  bool validateUnassigned(uint8_t pressedButton);
  // Labels for UI display.
  const char* getRoleName(uint8_t roleIndex) const;
  const char* getHardwareName(uint8_t buttonIndex) const;
};
