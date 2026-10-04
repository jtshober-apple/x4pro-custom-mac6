#include "Activity.h"

#include <HalStorage.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cctype>
#include <string>

#include "ActivityManager.h"
#include "activities/home/MetadataLookupActivity.h"
#include "network/HttpDownloader.h"
#include "util/MetadataOverride.h"

// ---------------------------------------------------------------------------
// Helpers (local to this TU)
// ---------------------------------------------------------------------------

// Returns the lower-case extension (without dot) of an absolute file path.
static std::string pathExtension(const std::string& path) {
  const auto dot = path.rfind('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return ext;
}

// Derive a human-readable title hint from the last path component, stripping
// the extension and replacing separators with spaces.
static std::string titleHintFromPath(const std::string& path) {
  const auto slash = path.rfind('/');
  std::string name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
  const auto dot = name.rfind('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  for (char& c : name) {
    if (c == '_' || c == '-') c = ' ';
  }
  return name;
}

// ---------------------------------------------------------------------------

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

// Open a book, optionally enriching its metadata first.
//
// For epub and xtc files that have not yet had a metadata lookup we chain
// MetadataLookupActivity before handing off to the reader.  The lookup is
// skipped silently when:
//   • WiFi is not connected (no fetch possible)
//   • Heap is too low for TLS (avoids mid-stream OOM)
//   • The file already has a meta_override.txt sidecar
//
// This makes the metadata system "everywhere": rename, open from file
// browser, open from recent books, open from home screen — all paths call
// onSelectBook() and all benefit from automatic enrichment on first open.
void Activity::onSelectBook(const std::string& path) {
  const std::string ext = pathExtension(path);
  const bool isEpubLike = (ext == "epub" || ext == "xtc");

  if (isEpubLike && !MetadataOverride::hasOverride(path) &&
      WiFi.status() == WL_CONNECTED &&
      heap_caps_get_free_size(MALLOC_CAP_8BIT) >= HttpDownloader::MIN_TLS_FREE_HEAP) {
    LOG_INF("ACT", "No metadata for '%s' — fetching before open", path.c_str());
    const std::string hint = titleHintFromPath(path);
    startActivityForResult(
        makeUniqueNoThrow<MetadataLookupActivity>(renderer, mappedInput, path, hint),
        [this, path](const ActivityResult&) {
          // Always open the reader after the lookup, whether it succeeded or not.
          activityManager.goToReader(path);
        });
    return;
  }

  activityManager.goToReader(path);
}

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
