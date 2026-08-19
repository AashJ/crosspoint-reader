## UI and Orientation Guidelines

### Orientation-Aware Logic

* No Hardcoding: Never assume 800 or 480. Use renderer.getScreenWidth() and renderer.getScreenHeight().
* Viewable Area: Use renderer.getOrientedViewableTRBL() to stay within physical bezel margins.

### Logical Button Mapping

**Source**: [src/MappedInputManager.cpp:20-55](../../src/MappedInputManager.cpp)

Constraint: Physical button positions are fixed on hardware, but their logical functions change based on user settings and screen orientation.

**Button Categories**:

1. **Physical Fixed** (Up/Down side buttons):
   
   - `Button::Up` → Always `HalGPIO::BTN_UP`
   
   - `Button::Down` → Always `HalGPIO::BTN_DOWN`

2. **User Remappable** (Front buttons):
   
   - `Button::Back` → Maps to `SETTINGS.frontButtonBack` (hardware index)
   
   - `Button::Confirm` → Maps to `SETTINGS.frontButtonConfirm`
   
   - `Button::Left` → Maps to `SETTINGS.frontButtonLeft`
   
   - `Button::Right` → Maps to `SETTINGS.frontButtonRight`

3. **Reader-Specific** (Page navigation with optional swap):
   
   - `Button::PageBack` → Uses side button (swappable via `SETTINGS.sideButtonLayout`)
   
   - `Button::PageForward` → Uses side button (swappable)

**Implementation**:

- Activities use **logical buttons** (e.g., `Button::Confirm`)
- `MappedInputManager` translates to **physical hardware buttons**
- User can remap front buttons in settings
- Orientation changes handled separately by renderer coordinate transforms

**Rule**: Always use `MappedInputManager::Button::*` enums, never raw `HalGPIO::BTN_*` indices (except in ButtonRemapActivity).

### UITheme (The GUI Macro)

* Rule: All UI rendering must go through the GUI macro (UITheme). 
* Do not hardcode fonts, colors, or positioning. This ensures orientation-aware layout consistency.

---

## Common Patterns

### Singleton Access

**Available Singletons**:

```cpp
#define SETTINGS CrossPointSettings::getInstance()  // User settings
#define APP_STATE CrossPointState::getInstance()    // Runtime state
#define GUI UITheme::getInstance()                   // Current theme
#define Storage HalStorage::getInstance()            // SD card I/O
#define I18N I18n::getInstance()                     // Internationalization
```

### Activity Lifecycle and Memory Management

**Source**: [src/main.cpp:132-143](../../src/main.cpp)

**CRITICAL**: Activities are **heap-allocated** and **deleted on exit**.

```cpp
// main.cpp navigation pattern
void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;  // Activity deleted here!
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;  // Heap-allocated activity
  currentActivity->onEnter();
}
```

**Memory Implications**:

- Activity navigation = `delete` old activity + `new` create next activity
- Any memory allocated in `onEnter()` MUST be freed in `onExit()`
- FreeRTOS tasks MUST be deleted in `onExit()` before activity destruction
- Member `FsFile` handles MUST be closed in `onExit()` (local `FsFile` variables auto-close via destructor)

**Activity Pattern**:

```cpp
void onEnter()  { Activity::onEnter(); /* alloc: buffer, tasks */ render(); }
void loop()     { mappedInput.update(); /* handle input */ }
void onExit()   { /* free: vTaskDelete, free buffer, close member FsFiles */ Activity::onExit(); }
```

**Critical**: Free resources in reverse order. Delete tasks BEFORE activity destruction.

### FreeRTOS Task Guidelines

**Source**: [src/activities/util/KeyboardEntryActivity.cpp:45-50](../../src/activities/util/KeyboardEntryActivity.cpp)

**Pattern**: See Activity Lifecycle above. `xTaskCreate(&taskTrampoline, "Name", stackSize, this, 1, &handle)`

**Stack Sizing** (in BYTES, not words):

- **2048**: Simple rendering (most activities)
- **4096**: Network, EPUB parsing
- Monitor: `uxTaskGetStackHighWaterMark()` if crashes

**Rules**: Always `vTaskDelete()` in `onExit()` before destruction. Use mutex if shared state.

### Global Font Loading

**Source**: [src/main.cpp:40-115](../../src/main.cpp)

**All fonts are loaded as global static objects** at firmware startup:

- Noto Serif: 12, 14, 16, 18pt (4 styles each: regular, bold, italic, bold-italic)
- Noto Sans: 12, 14, 16, 18pt (4 styles each)
- Ubuntu UI fonts: 10, 12pt (2 styles)

**Total**: ~80+ global `EpdFont` and `EpdFontFamily` objects

**Compilation Flag**:

```cpp
#ifndef OMIT_FONTS
  // Most fonts loaded here
#endif
```

**Implications**:

- Fonts stored in **Flash** (marked as `static const` in `lib/EpdFont/builtinFonts/`)
- Font rendering data cached in **DRAM** when first used
- `OMIT_FONTS` can reduce binary size for minimal builds
- Font IDs defined in [src/fontIds.h](../../src/fontIds.h)

**Usage**:

```cpp
#include "fontIds.h"

renderer.insertFont(FONT_UI_MEDIUM, ui12FontFamily);
renderer.drawText(FONT_UI_MEDIUM, x, y, "Hello", true);
```

---
