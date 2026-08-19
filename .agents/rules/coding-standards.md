## Coding Standards

### Naming Conventions

* Classes: PascalCase (e.g., EpubReaderActivity)
* Methods/Variables: camelCase (e.g., renderPage())
* Constants: UPPER_SNAKE_CASE (e.g., MAX_BUFFER_SIZE)
* Private Members: memberVariable (no prefix)
* File Names: Match Class names (e.g., EpubReaderActivity.cpp)

### Header Guards

* Use #pragma once for all header files.

### Comment Style

* Keep comments short and write them for the merged state, as if the code had always worked this way.
* Remove before/after narration, investigation measurements, and rationale that belongs in the commit message.
* Keep only non-obvious mechanism, field/parameter meaning, or the reason a special case exists.

### Memory Safety and RAII

* Smart Pointers: Prefer std::unique_ptr. 
* RAII: Use destructors for cleanup. Call `vTaskDelete()` explicitly for deterministic task release. Do NOT call `file.close()` on local `FsFile` variables — `DESTRUCTOR_CLOSES_FILE=1` handles it at scope exit (see Critical Build Flags).

### ESP32-C3 Platform Pitfalls

#### `std::string_view` and Null Termination

`string_view` is *not* null-terminated. Passing `.data()` to any C-style API (`drawText`, `snprintf`, `strcmp`, SdFat file paths) is undefined behaviour when the view is a substring or a view of a non-null-terminated buffer.

**Rule**: `string_view` is safe only when passing to C++ APIs that accept `string_view`. For any C API boundary, convert explicitly:

```cpp
// WRONG - undefined behaviour if view is a substring:
renderer.drawText(font, x, y, myView.data(), true);

// CORRECT - guaranteed null-terminated:
renderer.drawText(font, x, y, std::string(myView).c_str(), true);

// CORRECT - for short strings, use a stack buffer:
char buf[64];
snprintf(buf, sizeof(buf), "%.*s", (int)myView.size(), myView.data());
```

#### `IRAM_ATTR` and Flash Cache Safety

All code runs from flash via the instruction cache. During internal-flash operations such as OTA writes or NVS updates, the cache is briefly suspended. Any code that can execute during this window — ISRs in particular — must reside in IRAM or it will crash silently.

```cpp
// ISR handler: must be in IRAM
void IRAM_ATTR gpioISR() { ... }

// Data accessed from IRAM_ATTR code: must be in DRAM, never a flash const
static DRAM_ATTR uint32_t isrEventFlags = 0;
```

**Rules**:

- All ISR handlers: `IRAM_ATTR`
- Data read by `IRAM_ATTR` code: `DRAM_ATTR` (a flash-resident `static const` will fault)
- Normal task code does **not** need `IRAM_ATTR`

#### ISR vs Task Shared State

`xSemaphoreTake()` (mutex) **cannot** be called from ISR context — it will crash. Use the correct primitive for each communication direction:

| Direction                       | Correct primitive                                  |
| ------------------------------- | -------------------------------------------------- |
| ISR → task (data)               | `xQueueSendFromISR()` + `portYIELD_FROM_ISR()`     |
| ISR → task (signal)             | `xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()` |
| Task → task                     | `xSemaphoreTake()` / mutex                         |
| Simple flag (single writer ISR) | `volatile bool` + `portENTER_CRITICAL_ISR()`       |

#### RISC-V Alignment

ESP32-C3 faults on unaligned multi-byte loads. Never cast a `uint8_t*` buffer to a wider pointer type and dereference it directly. Use `memcpy` for any unaligned read:

```cpp
// WRONG — faults if buf is not 4-byte aligned:
uint32_t val = *reinterpret_cast<const uint32_t*>(buf);

// CORRECT:
uint32_t val;
memcpy(&val, buf, sizeof(val));
```

This applies to all cache deserialization code and any raw buffer-to-struct casting. `__attribute__((packed))` structs have the same hazard when accessed via member reference.

#### Template and `std::function` Bloat

Each template instantiation generates a separate binary copy. `std::function<void()>` adds ~2–4 KB per unique signature and heap-allocates its closure. Avoid both in library code and any path called from the render loop:

```cpp
// Avoid — heap-allocating, large binary footprint:
std::function<void()> callback;

// Prefer — zero overhead:
void (*callback)() = nullptr;

// For member function + context (common activity callback pattern):
struct Callback { void* ctx; void (*fn)(void*); };
```

When a template is necessary, limit instantiations: use explicit template instantiation in a `.cpp` file to prevent the compiler from generating duplicates across translation units.

---

### Error Handling Philosophy

**Source**: [src/main.cpp:132-143](../../src/main.cpp), [lib/GfxRenderer/GfxRenderer.cpp:10](../../lib/GfxRenderer/GfxRenderer.cpp)

**Pattern Hierarchy**:

1. **LOG_ERR + return false** (90%): `LOG_ERR("MOD", "Failed: %s", reason); return false;`
2. **LOG_ERR + fallback**: `LOG_ERR("MOD", "Unavailable"); useDefault();`
3. **assert(false)**: Only for fatal "impossible" states (framebuffer missing)
4. **ESP.restart()**: Only for recovery (OTA complete)

**Rules**: NO exceptions, NO abort(), ALWAYS log before error return

### Heap Buffer Allocation

**Prefer `makeUniqueNoThrow` over `malloc`.** Both are nothrow (return `nullptr` on OOM rather than calling `abort()`), but `malloc` requires a manual `free` on every return path — a common source of leaks. `makeUniqueNoThrow<uint8_t[]>(size)` from `lib/Memory/Memory.h` frees automatically when it goes out of scope.

**Preferred pattern**:

```cpp
#include <Memory.h>

auto buffer = makeUniqueNoThrow<uint8_t[]>(bufferSize);
if (!buffer) {
  LOG_ERR("MODULE", "OOM: %d bytes", bufferSize);
  return false;
}

processData(buffer.get(), bufferSize);
// freed automatically — no manual free needed, no leak on early return
```

**`malloc` or `new (std::nothrow)` are still acceptable** when the buffer must be passed to a C API that takes ownership and frees it itself (e.g., certain SDK callbacks). In that case follow the manual pattern:

```cpp
auto* buffer = static_cast<uint8_t*>(malloc(bufferSize));  // or new (std::nothrow) uint8_t[bufferSize]
if (!buffer) {
  LOG_ERR("MODULE", "OOM: %d bytes", bufferSize);
  return false;
}
sdkApiThatTakesOwnership(buffer, bufferSize);  // SDK calls free() / delete[]
```

**Rules**:

- **Prefer `makeUniqueNoThrow`** — automatic cleanup eliminates leak risk on error paths
- **ALWAYS check for nullptr** after any allocation and `LOG_ERR` before returning false
- **Raw allocation only** when a C API takes ownership; document why in a comment

**Examples in codebase**:

- Memory utilities: [Memory.h](../../lib/Memory/Memory.h) (`makeUniqueNoThrow`)
- Cover image buffers: [HomeActivity.cpp:166](../../src/activities/home/HomeActivity.cpp)
- Bitmap rendering: [GfxRenderer.cpp:439-440](../../lib/GfxRenderer/GfxRenderer.cpp)

### Heap Allocation with `new`: Always Use `makeUniqueNoThrow`

**CRITICAL**: With `-fno-exceptions`, bare `new` on OOM calls `abort()` — it does NOT return `nullptr`. Always use `makeUniqueNoThrow` from `lib/Memory/Memory.h`, which wraps `new (std::nothrow)` and returns a `std::unique_ptr` that is null on OOM and automatically frees on scope exit.

**Preferred pattern**:

```cpp
#include <Memory.h>

auto obj = makeUniqueNoThrow<MyClass>(args);
if (!obj) { LOG_ERR("MOD", "OOM: MyClass"); return false; }

auto buf = makeUniqueNoThrow<uint8_t[]>(size);
if (!buf) { LOG_ERR("MOD", "OOM: %d bytes", size); return false; }

// Pass to C APIs via .get(); unique_ptr frees automatically on return
someApi(buf.get(), size);
```

**`new (std::nothrow)` directly is acceptable** when the object must be passed to a C API that takes ownership and calls `delete` itself:

```cpp
auto* obj = new (std::nothrow) MyClass(args);
if (!obj) { LOG_ERR("MOD", "OOM: MyClass"); return false; }
sdkApiThatTakesOwnership(obj);  // SDK calls delete
```

**Rules**:

- **Prefer `makeUniqueNoThrow`** — automatic cleanup eliminates leak risk on error paths
- **NEVER use bare `new`** — always `makeUniqueNoThrow` or `new (std::nothrow)`
- **ALWAYS `LOG_ERR` before returning false** on OOM
- **Use `.get()`** to pass the raw pointer to C-style APIs; ownership stays with the `unique_ptr`
- **`new (std::nothrow)` directly only** when a C API takes ownership; document why in a comment

**Examples in codebase**:

- Memory utilities: [Memory.h](../../lib/Memory/Memory.h) (`makeUniqueNoThrow`)

---
