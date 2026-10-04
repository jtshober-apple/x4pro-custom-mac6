#pragma once

#include <string>

struct OpenLibraryResult {
  bool found = false;
  std::string title;
  std::string author;
  // Direct HTTPS URL for the cover JPEG. Empty if no cover was found.
  // Always HTTPS. May point to OpenLibrary or Google Books.
  std::string coverUrl;
};

// Multi-source metadata and cover fetcher.
//
// search() tries OpenLibrary first, falls back to Google Books.  Both sources
// use streaming JSON so the full response body is never held in RAM.  Each
// HTTP request is retried up to kMaxAttempts times with brief back-off.
//
// downloadCoverJpeg() downloads the URL stored in OpenLibraryResult::coverUrl.
// For OpenLibrary covers it automatically cascades through sizes (L → M → S)
// if the preferred size returns an error, so a cover is obtained whenever one
// exists on the server.
class OpenLibraryClient {
 public:
  // Search for a book by title. Returns true when at least a title was found.
  // Tries OpenLibrary, then Google Books as a fallback.
  static bool search(const std::string& title, OpenLibraryResult& out);

  // Download the cover JPEG at coverUrl to destPath.
  // For openlibrary.org URLs the size suffix is varied automatically if the
  // first attempt fails (L → M → S).
  // Returns true on success.
  static bool downloadCoverJpeg(const std::string& coverUrl, const std::string& destPath);
};
