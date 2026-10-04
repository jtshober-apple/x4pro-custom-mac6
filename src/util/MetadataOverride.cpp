#include "MetadataOverride.h"

#include <HalStorage.h>
#include <Logging.h>

#include <functional>
#include <string>

static constexpr const char* TAG = "MTO";

std::string MetadataOverride::getCachePath(const std::string& epubPath) {
  return std::string("/.crosspoint/epub_") + std::to_string(std::hash<std::string>{}(epubPath));
}

std::string MetadataOverride::getCoverBmpPath(const std::string& epubPath) {
  return getCachePath(epubPath) + "/cover.bmp";
}

bool MetadataOverride::save(const std::string& epubPath, const std::string& title,
                            const std::string& author) {
  const std::string cachePath = getCachePath(epubPath);
  Storage.ensureDirectoryExists(cachePath.c_str());

  const std::string overridePath = cachePath + "/meta_override.txt";
  HalFile f;
  if (!Storage.openFileForWrite(TAG, overridePath, f)) {
    LOG_ERR(TAG, "Could not write meta_override.txt for %s", epubPath.c_str());
    return false;
  }
  f.println(title.c_str());
  f.println(author.c_str());
  // HalFile destructor closes the file.

  // Delete book.bin so Epub::load() rebuilds the cache with the new metadata.
  const std::string bookBinPath = cachePath + "/book.bin";
  if (Storage.exists(bookBinPath.c_str())) {
    Storage.remove(bookBinPath.c_str());
    LOG_INF(TAG, "Deleted stale book.bin for cache rebuild");
  }

  LOG_INF(TAG, "Saved override: '%s' by '%s'", title.c_str(), author.c_str());
  return true;
}

bool MetadataOverride::load(const std::string& epubPath, std::string& title, std::string& author) {
  const std::string overridePath = getCachePath(epubPath) + "/meta_override.txt";
  HalFile f;
  if (!Storage.openFileForRead(TAG, overridePath, f)) return false;

  title.clear();
  author.clear();
  bool readingAuthor = false;

  while (f.available()) {
    const char c = static_cast<char>(f.read());
    if (c == '\n') {
      if (!readingAuthor) {
        readingAuthor = true;
      } else {
        break;
      }
    } else if (c == '\r') {
      // ignore CR in CRLF sequences
    } else if (!readingAuthor) {
      title += c;
    } else {
      author += c;
    }
  }

  return !title.empty();
}

bool MetadataOverride::hasOverride(const std::string& epubPath) {
  const std::string overridePath = getCachePath(epubPath) + "/meta_override.txt";
  return Storage.exists(overridePath.c_str());
}
