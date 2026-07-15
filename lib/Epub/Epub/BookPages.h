#pragma once

#include <cstdint>

// Whole-book page accounting for book-global "page X of Y".
//
// The finalized section cache files (sections/*.bin) are the single source of
// truth for exact per-section counts; the caller harvests them into an array of
// BookPageEntry (nothing is persisted separately). Sections without an exact
// count are estimated from their byte size, calibrated against the sections
// whose counts are known — so the total gets more accurate as more of the book
// is paginated, and becomes exact once every section is.

struct BookPageEntry {
  uint32_t bytes = 0;  // uncompressed XHTML size, for estimating unknown sections
  int32_t pages = -1;  // exact count from a finalized section cache; -1 = unknown (0 = empty chapter)
};

struct BookPagePosition {
  int currentPage = 0;
  int totalPages = 0;
  bool isEstimate = true;  // true until every section has an exact count
};

// Book-global position: currentPage = pages before spineIndex + pageInSection + 1.
// liveSectionPages is the in-progress build's estimate for spineIndex (see
// Section::estimatedTotalPages); it is used for that section when it has no exact
// count yet and folded into the bytes-per-page calibration. Pure function: no I/O.
BookPagePosition computeBookPagePosition(const BookPageEntry* entries, int sectionCount, int spineIndex,
                                         int pageInSection, int liveSectionPages);
