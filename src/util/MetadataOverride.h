#pragma once

#include <string>

// Utilities for reading and writing a sidecar metadata override file that
// lives in the epub's cache directory. Writing the override then deleting
// book.bin forces Epub.cpp to rebuild the cache with the new title/author
// baked in permanently.
class MetadataOverride {
 public:
  // Returns the /.crosspoint/epub_HASH path for the given epub file path.
  // Must match the formula in Epub.h exactly.
  static std::string getCachePath(const std::string& epubPath);

  // Write title + author into meta_override.txt and delete book.bin so the
  // next Epub::load() rebuilds the cache with the new values.
  // Returns true on success.
  static bool save(const std::string& epubPath, const std::string& title, const std::string& author);

  // Read title + author from meta_override.txt.
  // Returns true if the file existed and had at least a title line.
  static bool load(const std::string& epubPath, std::string& title, std::string& author);

  // Returns the path where the converted cover BMP should be stored.
  static std::string getCoverBmpPath(const std::string& epubPath);
};
