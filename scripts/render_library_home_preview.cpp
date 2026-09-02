#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "components/LibraryHomeScreen.h"
#include "gallery_font.h"

namespace fui = freeink::ui;

namespace {

struct Preview {
  using App = fui::FreeInkApp<24, 6>;

  int16_t width;
  int16_t height;
  int16_t widthBytes;
  std::vector<uint8_t> framebuffer;
  fui::DisplayTarget target;
  App app;
  library_home::ScreenStorage storage;
  library_home::Model model;
  fui::TileGridItem utilities[4];
  const char* titles[10] = {"The Dispossessed",
                            "Piranesi",
                            "Beloved",
                            "Kindred",
                            "Wolf Hall",
                            "Dune",
                            "Orlando",
                            "Parable of the Sower",
                            "The Left Hand of Darkness",
                            "Invisible Cities"};

  Preview(const int16_t w, const int16_t h)
      : width(w),
        height(h),
        widthBytes(static_cast<int16_t>((w + 7) / 8)),
        framebuffer(static_cast<size_t>(widthBytes) * h, 0xFF),
        target(framebuffer.data(), w, h, widthBytes, fui::Orientation::LandscapeCounterClockwise),
        app(target, device(w, h)) {
    target.setFont(fui::kNotoSansSmallFont);
    app.setTheme(fui::themeTokensForLineHeight(target.lineHeight(0)));
    app.setClearColor(fui::Color::White);
    model.labels = {"Menu", "No books yet"};
    model.bookProvider = &Preview::bookAt;
    model.bookProviderUserData = this;
    model.bookCount = 10;
    model.selectedBook = 0;
    model.topChrome = 64;
    model.utilityItems = utilities;
    model.utilityCount = 4;

    const char* utilityLabels[4] = {"Add Books", "Browse Files", "Online Library", "Settings"};
    for (int16_t i = 0; i < 4; ++i) {
      utilities[i].label = utilityLabels[i];
      utilities[i].value = i;
    }
    app.setScreen(&Preview::screen, this, fui::RefreshHint::Fast);
  }

  static fui::DeviceContext device(const int16_t width, const int16_t height) {
    fui::DeviceContext result;
    result.width = width;
    result.height = height;
    result.hasTouch = true;
    result.hasButtons = true;
    result.minTouchSize = 44;
    return result;
  }

  static fui::CoverGridItem bookAt(const uint16_t index, void* user) {
    auto* self = static_cast<Preview*>(user);
    return fui::coverGridItem(self->titles[index], static_cast<int16_t>(index));
  }

  static void screen(App::ScreenType& screen, void* user) {
    auto* self = static_cast<Preview*>(user);
    fui::HeaderProps header;
    header.title = "Library";
    header.rightLabel = "82%";
    header.titleText = screen.theme().titleText;
    header.subtitleText = screen.theme().smallText;
    header.styles = screen.theme().popup;
    header.borderEdges = fui::EdgeBottom;
    fui::header(screen.frame(), fui::Rect{0, 0, self->width, 64}, header);
    library_home::build(screen, self->model, self->storage);
  }

  bool inkAt(const int16_t x, const int16_t y) const {
    const uint8_t byte = framebuffer[static_cast<size_t>(y) * widthBytes + (x >> 3)];
    return (byte & static_cast<uint8_t>(0x80 >> (x & 7))) == 0;
  }
};

void writeSvg(const Preview& preview, const std::string& path) {
  std::ofstream output(path);
  if (!output) {
    std::fprintf(stderr, "Unable to open %s\n", path.c_str());
    std::exit(1);
  }

  output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << preview.width << ' ' << preview.height
         << "\" width=\"" << preview.width << "\" height=\"" << preview.height
         << "\" shape-rendering=\"crispEdges\">\n";
  output << "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>\n<g fill=\"#111\">\n";
  for (int16_t y = 0; y < preview.height; ++y) {
    int16_t x = 0;
    while (x < preview.width) {
      while (x < preview.width && !preview.inkAt(x, y)) ++x;
      const int16_t start = x;
      while (x < preview.width && preview.inkAt(x, y)) ++x;
      if (x > start) {
        output << "<rect x=\"" << start << "\" y=\"" << y << "\" width=\"" << x - start << "\" height=\"1\"/>\n";
      }
    }
  }
  output << "</g>\n</svg>\n";
}

void writePgm(const Preview& preview, const std::string& path) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    std::fprintf(stderr, "Unable to open %s\n", path.c_str());
    std::exit(1);
  }
  output << "P5\n" << preview.width << ' ' << preview.height << "\n255\n";
  for (int16_t y = 0; y < preview.height; ++y) {
    for (int16_t x = 0; x < preview.width; ++x) {
      const uint8_t value = preview.inkAt(x, y) ? 0 : 255;
      output.write(reinterpret_cast<const char*>(&value), 1);
    }
  }
}

void writeLe16(std::ofstream& output, const uint16_t value) {
  const uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeLe32(std::ofstream& output, const uint32_t value) {
  const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeBmp(const Preview& preview, const std::string& path) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    std::fprintf(stderr, "Unable to open %s\n", path.c_str());
    std::exit(1);
  }
  const uint32_t rowBytes = static_cast<uint32_t>((preview.width * 3 + 3) & ~3);
  const uint32_t pixelBytes = rowBytes * preview.height;
  output.write("BM", 2);
  writeLe32(output, 54 + pixelBytes);
  writeLe32(output, 0);
  writeLe32(output, 54);
  writeLe32(output, 40);
  writeLe32(output, preview.width);
  writeLe32(output, preview.height);
  writeLe16(output, 1);
  writeLe16(output, 24);
  writeLe32(output, 0);
  writeLe32(output, pixelBytes);
  writeLe32(output, 2835);
  writeLe32(output, 2835);
  writeLe32(output, 0);
  writeLe32(output, 0);
  const uint8_t padding[3] = {0, 0, 0};
  for (int y = preview.height - 1; y >= 0; --y) {
    for (int16_t x = 0; x < preview.width; ++x) {
      const uint8_t value = preview.inkAt(x, static_cast<int16_t>(y)) ? 0 : 255;
      const uint8_t pixel[3] = {value, value, value};
      output.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    }
    output.write(reinterpret_cast<const char*>(padding), rowBytes - static_cast<uint32_t>(preview.width * 3));
  }
}

void renderPreview(const std::string& path, const int16_t width, const int16_t height, const bool menuOpen,
                   const uint16_t bookCount) {
  Preview preview(width, height);
  preview.model.menuOpen = menuOpen;
  preview.model.bookCount = bookCount;
  preview.app.render();
  writeSvg(preview, path);
  writePgm(preview, path.substr(0, path.size() - 4) + ".pgm");
  writeBmp(preview, path.substr(0, path.size() - 4) + ".bmp");
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
    return 2;
  }
  const std::string output = argv[1];
  renderPreview(output + "/library-home.svg", 480, 800, false, 10);
  renderPreview(output + "/library-home-menu.svg", 480, 800, true, 10);
  renderPreview(output + "/library-home-empty.svg", 480, 800, false, 0);
  renderPreview(output + "/library-home-landscape.svg", 800, 480, false, 10);
  return 0;
}
