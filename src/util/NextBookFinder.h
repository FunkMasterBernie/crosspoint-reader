#pragma once

#include <string>
#include <vector>

namespace NextBookFinder {

// Collects up to maxCount book files that order after currentBookPath's filename
// (natural sort, same ordering as the file browser) within the same folder.
// Returns bare filenames in sorted order; the current file itself is excluded.
// Single directory pass keeping only the maxCount best matches, so memory stays
// bounded regardless of folder size.
std::vector<std::string> findNextBooks(const std::string& currentBookPath, size_t maxCount);

/**
 * The next volume of the series `currentBookPath` belongs to, as a full path.
 *
 * Records in the Library index are stored in sort-key order, and that key is
 * series-aware, so the next volume is simply the next record -- provided it
 * carries the same series name. Returns an empty string for a standalone book,
 * for the last volume of a series, or when the index cannot be read; the caller
 * then falls back to folder siblings.
 *
 * Unlike findNextBooks(), this can cross folders: the next volume is routinely
 * filed under a different author.
 *
 * `outLabel` receives the book's own title when it has one, otherwise its
 * filename without the extension.
 */
std::string findNextInSeries(const std::string& currentBookPath, std::string& outLabel);

}  // namespace NextBookFinder
