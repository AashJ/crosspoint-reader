#include "BookPages.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
// Coarse seed used only before any section has an exact or live count; replaced
// by the calibrated average as soon as one is available.
constexpr double DEFAULT_BYTES_PER_PAGE = 2000.0;
// A section's serialized pageCount is a uint16_t, so no estimate needs to exceed this.
constexpr int MAX_SECTION_PAGES = std::numeric_limits<uint16_t>::max();

int clampToInt(const uint64_t v) {
  return v > static_cast<uint64_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
                                                                    : static_cast<int>(v);
}
}  // namespace

BookPagePosition computeBookPagePosition(const BookPageEntry* entries, const int sectionCount, const int spineIndex,
                                         const int pageInSection, const int liveSectionPages) {
  BookPagePosition pos;
  if (!entries || sectionCount <= 0) {
    return pos;
  }

  // Calibrate bytes-per-page from every section with an exact count (empty
  // known-0 chapters excluded), plus the live estimate for spineIndex when it
  // has no exact count yet.
  uint64_t knownBytes = 0;
  uint64_t knownPages = 0;
  for (int i = 0; i < sectionCount; ++i) {
    if (entries[i].pages > 0) {
      knownBytes += entries[i].bytes;
      knownPages += static_cast<uint32_t>(entries[i].pages);
    }
  }
  const bool useLive = spineIndex >= 0 && spineIndex < sectionCount && entries[spineIndex].pages < 0 &&
                       liveSectionPages > 0 && entries[spineIndex].bytes > 0;
  if (useLive) {
    knownBytes += entries[spineIndex].bytes;
    knownPages += static_cast<uint32_t>(liveSectionPages);
  }
  const double bytesPerPage = (knownBytes > 0 && knownPages > 0)
                                  ? static_cast<double>(knownBytes) / static_cast<double>(knownPages)
                                  : DEFAULT_BYTES_PER_PAGE;

  uint64_t total = 0;
  uint64_t before = 0;
  bool exact = true;
  for (int i = 0; i < sectionCount; ++i) {
    uint32_t pages;
    if (entries[i].pages >= 0) {
      pages = static_cast<uint32_t>(entries[i].pages);  // exact (0 = genuinely empty chapter)
    } else if (i == spineIndex && liveSectionPages > 0) {
      pages = static_cast<uint32_t>(std::min(liveSectionPages, MAX_SECTION_PAGES));
      exact = false;
    } else {
      const double raw = std::floor(static_cast<double>(entries[i].bytes) / bytesPerPage + 0.5);
      pages = static_cast<uint32_t>(std::clamp(raw, 1.0, static_cast<double>(MAX_SECTION_PAGES)));
      exact = false;
    }
    total += pages;
    if (i < spineIndex) {
      before += pages;
    }
  }

  pos.totalPages = clampToInt(total);
  pos.currentPage = clampToInt(before + static_cast<uint64_t>(std::max(0, pageInSection)) + 1);
  pos.isEstimate = !exact;
  return pos;
}
