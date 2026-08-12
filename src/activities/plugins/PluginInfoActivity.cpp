#include "PluginInfoActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/CatalogScreens.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_README_SIZE = 24 * 1024;

// A very light markdown flattener for e-ink: drops heading/list/emphasis
// markers and code fences, leaving readable plain text. No renderer needed.
std::string stripMarkdown(const std::string& line) {
  std::string s = line;
  size_t start = 0;
  while (start < s.size() && (s[start] == '#' || s[start] == '>' || s[start] == ' ')) start++;
  s = s.substr(start);
  if (s.rfind("- ", 0) == 0 || s.rfind("* ", 0) == 0) s = "\xE2\x80\xA2 " + s.substr(2);  // bullet
  // strip inline emphasis/backticks
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '*' || c == '_' || c == '`') continue;
    out += c;
  }
  return out;
}
}  // namespace

void PluginInfoActivity::onEnter() {
  Activity::onEnter();
  topLine = 0;
  subscreenOpen = false;
  wrappedWidth = -1;
  loadParagraphs();
  resetUi();
  app.setScreen(&PluginInfoActivity::rootScreen, this);
  requestUpdate();
}

// Read raw text into `paragraphs` without any measurement — safe to run in
// onEnter (fonts/orientation may not be settled until the render task runs).
void PluginInfoActivity::loadParagraphs() {
  paragraphs.clear();
  if (!plugin.description.empty()) {
    paragraphs.push_back(plugin.description);
    paragraphs.emplace_back("");
  }
  if (plugin.readmePath.empty()) {
    if (!plugin.hasCatalog) paragraphs.emplace_back(tr(STR_PLUGIN_WEB_ONLY));
    return;
  }
  HalFile file;
  if (!Storage.openFileForRead("PINFO", plugin.readmePath, file)) return;
  const size_t size = file.fileSize();
  if (size == 0 || size > MAX_README_SIZE) return;
  std::string raw;
  raw.resize(size);
  if (file.read(raw.data(), size) != static_cast<int>(size)) return;

  paragraphs.reserve(paragraphs.size() + raw.size() / 40);  // rough average line length
  size_t pos = 0;
  while (pos <= raw.size()) {
    size_t nl = raw.find('\n', pos);
    std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("```", 0) != 0) paragraphs.push_back(line);  // drop code-fence markers
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

// Word-wrap `paragraphs` into `lines` at the current content width, measured
// with the FreeInkUI body font the screen draws with. Runs on the render task,
// where the width and font metrics are final.
void PluginInfoActivity::ensureWrapped() {
  const int fontId = uiScaleSpec().bodyFontId;
  const int sidePadding = UITheme::getInstance().getMetrics().contentSidePadding;
  const int wrapWidth = renderer.getScreenWidth() - sidePadding * 2;
  if (wrappedWidth == wrapWidth) return;  // already wrapped for this width
  wrappedWidth = wrapWidth;
  lines.clear();
  lines.reserve(paragraphs.size() * 2);  // rough average wrapped lines per paragraph
  for (const std::string& raw : paragraphs) {
    std::string text = stripMarkdown(raw);
    if (text.empty()) {
      lines.emplace_back("");
      continue;
    }
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
      size_t sp = text.find(' ', i);
      std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
      // Hard-break a single word wider than the line (e.g. a long URL).
      while (renderer.getTextWidth(fontId, word.c_str()) > wrapWidth && word.size() > 1) {
        size_t cut = word.size();
        while (cut > 1 && renderer.getTextWidth(fontId, word.substr(0, cut).c_str()) > wrapWidth) cut--;
        if (!cur.empty()) {
          lines.push_back(cur);
          cur.clear();
        }
        lines.push_back(word.substr(0, cut));
        word = word.substr(cut);
      }
      const std::string trial = cur.empty() ? word : cur + " " + word;
      if (renderer.getTextWidth(fontId, trial.c_str()) > wrapWidth && !cur.empty()) {
        lines.push_back(cur);
        cur = word;
      } else {
        cur = trial;
      }
      if (sp == std::string::npos) break;
      i = sp + 1;
    }
    lines.push_back(cur);
  }
}

void PluginInfoActivity::rootScreen(UiScreen& screen, void* user) {
  static_cast<PluginInfoActivity*>(user)->buildScreen(screen);
}

void PluginInfoActivity::buildScreen(UiScreen& screen) {
  ensureWrapped();  // wrap now that width + fonts are final
  catalogScreenHeader(screen, renderer, plugin.title.c_str());

  fui::TextStyle body = screen.theme().bodyText;
  const int16_t lh = screen.target().lineHeight(body.font);
  const int16_t sidePadding = static_cast<int16_t>(UITheme::getInstance().getMetrics().contentSidePadding);
  fui::Rect bodyRect = screen.body();
  visibleRows = std::max<int>(1, bodyRect.height / lh);
  for (int i = 0; i < visibleRows && topLine + i < static_cast<int>(lines.size()); i++) {
    const fui::Rect lineRect = screen.takeTop(lh).inset(fui::Insets{0, sidePadding, 0, sidePadding});
    screen.target().text(lineRect, lines[topLine + i].c_str(), body);
  }
}

void PluginInfoActivity::openCatalog() {
  subscreenOpen = true;
  startActivityForResult(
      std::make_unique<PluginCatalogActivity>(renderer, mappedInput, plugin.manifestPath, plugin.title),
      [this](const ActivityResult&) { subscreenOpen = false; });
}

void PluginInfoActivity::loop() {
  if (subscreenOpen) return;

  if (plugin.hasCatalog && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openCatalog();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int rows = visibleRows;
  const int maxTop = std::max(0, static_cast<int>(lines.size()) - rows);
  const auto scroll = [&](int delta) {
    const int next = std::min(std::max(0, topLine + delta), maxTop);
    if (next != topLine) {
      topLine = next;
      requestUpdate();
    }
  };
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    scroll(rows);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    scroll(-rows);
    return;
  }
  buttonNavigator.onNextRelease([&] { scroll(rows); });
  buttonNavigator.onPreviousRelease([&] { scroll(-rows); });
}

void PluginInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderUi();

  const char* confirmLabel = plugin.hasCatalog ? tr(STR_OPEN) : "";
  const bool canScroll = static_cast<int>(lines.size()) > visibleRows;
  const char* up = canScroll ? tr(STR_DIR_UP) : "";
  const char* down = canScroll ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
