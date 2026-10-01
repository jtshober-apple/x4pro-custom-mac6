#pragma once

#include <string>

struct OpenLibraryResult {
  bool found = false;
  std::string title;
  std::string author;
  int coverId = 0;  // 0 means no cover
};

// Thin wrapper around OpenLibrary's public search API.
// All methods are synchronous and block the caller.
class OpenLibraryClient {
 public:
  // Search for a book by title. Fills `out` on success.
  // Returns true if at least one result was found.
  static bool search(const std::string& title, OpenLibraryResult& out);

  // Download the cover JPEG for the given coverId to destPath.
  // Returns true on success.
  static bool downloadCoverJpeg(int coverId, const std::string& destPath);
};
