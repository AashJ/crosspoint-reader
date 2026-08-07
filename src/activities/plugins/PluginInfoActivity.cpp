#include "PluginInfoActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int HEADER_X = 16;
constexpr int HEADER_Y = 15;
constexpr int BODY_TOP = 46;
constexpr int LINE_STEP = 22;
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
  wrapReadme();
  requestUpdate();
}

int PluginInfoActivity::visibleLines() const {
  const int avail = renderer.getScreenHeight() - BODY_TOP - UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int n = avail / LINE_STEP;
  return n < 1 ? 1 : n;
}

void PluginInfoActivity::wrapReadme() {
  lines.clear();
  // Lead with the description, then a blank line, then the README.
  const int wrapWidth = renderer.getScreenWidth() - HEADER_X * 2;
  auto pushWrapped = [&](const std::string& raw) {
    std::string text = stripMarkdown(raw);
    if (text.empty()) {
      lines.emplace_back("");
      return;
    }
    // Greedy word wrap against the measured pixel width.
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
      size_t sp = text.find(' ', i);
      std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
      // Hard-break a single word wider than the line (e.g. a long URL), which
      // greedy word-wrap alone would let run off the edge.
      while (renderer.getTextWidth(UI_10_FONT_ID, word.c_str()) > wrapWidth && word.size() > 1) {
        size_t cut = word.size();
        while (cut > 1 && renderer.getTextWidth(UI_10_FONT_ID, word.substr(0, cut).c_str()) > wrapWidth) cut--;
        if (!cur.empty()) {
          lines.push_back(cur);
          cur.clear();
        }
        lines.push_back(word.substr(0, cut));
        word = word.substr(cut);
      }
      const std::string trial = cur.empty() ? word : cur + " " + word;
      if (renderer.getTextWidth(UI_10_FONT_ID, trial.c_str()) > wrapWidth && !cur.empty()) {
        lines.push_back(cur);
        cur = word;
      } else {
        cur = trial;
      }
      if (sp == std::string::npos) break;
      i = sp + 1;
    }
    lines.push_back(cur);
  };

  if (!plugin.description.empty()) {
    pushWrapped(plugin.description);
    lines.emplace_back("");
  }

  if (plugin.readmePath.empty()) {
    if (!plugin.hasCatalog) lines.emplace_back(tr(STR_PLUGIN_WEB_ONLY));
    return;
  }
  HalFile file;
  if (!Storage.openFileForRead("PINFO", plugin.readmePath, file)) return;
  const size_t size = file.fileSize();
  if (size == 0 || size > MAX_README_SIZE) return;
  std::string raw;
  raw.resize(size);
  if (file.read(raw.data(), size) != static_cast<int>(size)) return;

  size_t pos = 0;
  while (pos <= raw.size()) {
    size_t nl = raw.find('\n', pos);
    std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("```", 0) == 0) {  // drop code-fence markers, keep contents
    } else {
      pushWrapped(line);
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
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

  const int rows = visibleLines();
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
  const auto pageWidth = renderer.getScreenWidth();

  const auto header = renderer.truncatedText(UI_12_FONT_ID, plugin.title.c_str(), pageWidth - HEADER_X * 2);
  renderer.drawText(UI_12_FONT_ID, HEADER_X, HEADER_Y, header.c_str(), true, EpdFontFamily::BOLD);

  const char* confirmLabel = plugin.hasCatalog ? tr(STR_OPEN) : "";
  const int rows = visibleLines();
  const bool more = topLine + rows < static_cast<int>(lines.size());
  const char* scrollLabel = (more || topLine > 0) ? tr(STR_NEXT_PAGE) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, scrollLabel, "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  for (int i = 0; i < rows && topLine + i < static_cast<int>(lines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, HEADER_X, BODY_TOP + i * LINE_STEP, lines[topLine + i].c_str(), true);
  }
  renderer.displayBuffer();
}
