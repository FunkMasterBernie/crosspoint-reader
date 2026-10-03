#pragma once

#include <map>
#include <string>

#include "HalStorage.h"

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  std::string series;
  float seriesIndex = 0.0f;
  bool success = true;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(std::string& title, std::string& author, std::string* series = nullptr,
                    float* seriesIndex = nullptr) {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    if (series) *series = metadata.series;
    if (seriesIndex) *seriesIndex = metadata.seriesIndex;
    return true;
  }
};
