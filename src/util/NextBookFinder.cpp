#include "NextBookFinder.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <string_view>

#include "CrossPointSettings.h"
#include "LibraryBuilder.h"
#include "LibraryFormat.h"
#include "LibraryIndexFile.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;

bool isSupportedBookFile(const std::string_view name) {
  // Formats ReaderActivity can open (bmp is a viewer, not a book, so it is excluded)
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}
}  // namespace

std::vector<std::string> NextBookFinder::findNextBooks(const std::string& currentBookPath, const size_t maxCount) {
  std::vector<std::string> result;
  if (maxCount == 0 || currentBookPath.empty()) {
    return result;
  }

  const std::string folder = FsHelpers::extractFolderPath(currentBookPath);
  const auto lastSlash = currentBookPath.find_last_of('/');
  const std::string currentName =
      lastSlash == std::string::npos ? currentBookPath : currentBookPath.substr(lastSlash + 1);

  auto dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("NBF", "Cannot open folder: %s", folder.c_str());
    return result;
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("NBF", "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    dir.close();
    return result;
  }

  // Heap use is bounded: at most maxCount+1 short filename strings live at once (the
  // file browser holds a whole folder in the same std::string form). A failed
  // allocation here would abort like any STL growth in this codebase; the reserve
  // below makes vector growth a single up-front allocation.
  result.reserve(maxCount + 1);
  const auto less = [](const std::string& a, const std::string& b) { return FsHelpers::naturalLess(a, b); };

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      continue;
    }
    file.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    if (!SETTINGS.showHiddenFiles && nameBuffer[0] == '.') {
      continue;
    }
    if (!isSupportedBookFile(nameBuffer.get())) {
      continue;
    }
    std::string name{nameBuffer.get()};
    // Keep only files ordering strictly after the current one; equal names (the book
    // itself, or a case-variant of it) compare "not less" both ways and drop out here.
    if (!FsHelpers::naturalLess(currentName, name)) {
      continue;
    }
    // Bounded insertion sort: keep the maxCount lowest-ordering candidates
    if (result.size() >= maxCount && !less(name, result.back())) {
      continue;
    }
    const auto pos = std::lower_bound(result.begin(), result.end(), name, less);
    result.insert(pos, std::move(name));
    if (result.size() > maxCount) {
      result.pop_back();
    }
  }
  dir.close();

  return result;
}

std::string NextBookFinder::findNextInSeries(const std::string& currentBookPath, std::string& outLabel) {
  outLabel.clear();
  if (currentBookPath.empty()) return {};

  // The index reader holds a record and its own read buffer; keep it off the
  // stack like every other caller does.
  struct IndexReader {
    library::LibraryIndexFile index;
    library::ClixRecord record{};
  };
  auto reader = makeUniqueNoThrow<IndexReader>();
  if (!reader) {
    LOG_ERR("NBF", "OOM: library index reader");
    return {};
  }
  auto& index = reader->index;
  // Read-only: a missing or stale index is a reason to fall back to the folder
  // scan, never to rebuild the whole library from the end-of-book screen.
  if (!index.open(library::libraryIndexPath())) return {};

  const library::BookIdentity identity{library::clixPathHash(currentBookPath.data(), currentBookPath.size()), 0};
  uint16_t row = 0xFFFF;
  if (!index.recentRowsFor(&identity, 1, &row) || row == 0xFFFF) return {};

  const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentAsc, row);
  if (ordinal == 0xFFFF || ordinal + 1 >= index.bookCount()) return {};

  if (!index.readRecord(ordinal, reader->record)) return {};
  std::string series;
  index.readSeries(reader->record, series);
  if (series.empty()) return {};  // standalone book

  // Records are written in sort-key order and that key leads with the series, so
  // the neighbour is the next volume whenever it belongs to the same series.
  if (!index.readRecord(static_cast<uint16_t>(ordinal + 1), reader->record)) return {};
  std::string nextSeries;
  index.readSeries(reader->record, nextSeries);
  if (nextSeries != series) return {};  // that was the last volume

  std::string path;
  if (!index.readPath(reader->record, path) || path.empty()) return {};
  if (path == currentBookPath) return {};

  if (!index.readTitle(reader->record, outLabel) || outLabel.empty()) {
    std::string name;
    if (index.readName(reader->record, name)) {
      const auto dot = name.rfind('.');
      outLabel = dot == std::string::npos ? name : name.substr(0, dot);
    }
  }
  LOG_DBG("NBF", "Next in '%s': %s", series.c_str(), path.c_str());
  return path;
}
