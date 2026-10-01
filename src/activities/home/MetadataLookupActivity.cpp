#include "MetadataLookupActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <Memory.h>

#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/OpenLibraryClient.h"
#include "util/MetadataOverride.h"

static constexpr const char* TAG = "MLA";

MetadataLookupActivity::MetadataLookupActivity(GfxRenderer& renderer, MappedInputManager& input,
                                               const std::string& epubPath,
                                               const std::string& titleHint)
    : Activity("MetadataLookup", renderer, input), epubPath_(epubPath), titleHint_(titleHint) {}

void MetadataLookupActivity::onEnter() {
  Activity::onEnter();
  foundTitle_[0] = '\0';
  foundAuthor_[0] = '\0';
  doneAtMs_ = 0;

  if (WiFi.status() == WL_CONNECTED) {
    state_ = LOOKING_UP;
    requestUpdate();
    return;
  }

  shouldTearDownWifiOnExit_ = true;
  launchWifiSelection();
}

void MetadataLookupActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void MetadataLookupActivity::launchWifiSelection() {
  LOG_INF(TAG, "No WiFi — launching WiFi selection before metadata lookup");
  startActivityForResult(
      makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void MetadataLookupActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF(TAG, "WiFi selection cancelled — skipping metadata lookup");
    finish();
    return;
  }

  state_ = LOOKING_UP;
  requestUpdate();
}

void MetadataLookupActivity::loop() {
  if (state_ == LOOKING_UP) {
    // Force the "Looking up…" screen before blocking on network.
    requestUpdateAndWait();
    doWork();
    doneAtMs_ = millis();
    requestUpdate();
    return;
  }

  // After a result screen, tap or Back exits.
  int x = 0, y = 0;
  if (state_ != WAITING_FOR_WIFI &&
      (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
       mappedInput.wasScreenTapped(x, y) ||
       (doneAtMs_ != 0 && (millis() - doneAtMs_) > 2500))) {
    finish();
  }
}

void MetadataLookupActivity::doWork() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR(TAG, "WiFi lost before metadata fetch");
    state_ = NO_WIFI;
    return;
  }

  // Guard against low-heap TLS OOM.
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < HttpDownloader::MIN_TLS_FREE_HEAP ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR(TAG, "Heap too low for TLS (free=%d largest=%d)",
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    state_ = FAILED;
    return;
  }

  LOG_INF(TAG, "Searching OpenLibrary: %s", titleHint_.c_str());
  OpenLibraryResult result;
  if (!OpenLibraryClient::search(titleHint_, result)) {
    LOG_INF(TAG, "Not found on OpenLibrary");
    state_ = NOT_FOUND;
    return;
  }

  LOG_INF(TAG, "Found: '%s' by '%s' coverId=%d", result.title.c_str(), result.author.c_str(),
          result.coverId);

  // Copy found values into fixed buffers for the render method.
  strncpy(foundTitle_, result.title.c_str(), sizeof(foundTitle_) - 1);
  strncpy(foundAuthor_, result.author.c_str(), sizeof(foundAuthor_) - 1);

  // Persist the sidecar override — this also deletes the stale book.bin so
  // the next Epub::load() bakes the new metadata in permanently.
  MetadataOverride::save(epubPath_, result.title, result.author);

  // Download and convert the cover to a 1-bit BMP thumbnail sized for the
  // current theme.  HomeActivity calls UITheme::getCoverThumbPath() which
  // replaces the "[HEIGHT]" token with the live homeCoverHeight value, so the
  // stored path must use that token to remain valid across theme switches.
  const std::string cachePath = MetadataOverride::getCachePath(epubPath_);
  std::string thumbPath;

  if (result.coverId > 0) {
    const std::string jpegTmp = cachePath + "/cover_tmp.jpg";

    // Match the dimensions the home screen will request.
    const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
    const int coverWidth = static_cast<int>(coverHeight * 0.6f);
    const std::string thumbBmp =
        cachePath + "/thumb_" + std::to_string(coverHeight) + ".bmp";

    LOG_INF(TAG, "Cover: coverId=%d h=%d w=%d dst=%s", result.coverId, coverHeight, coverWidth,
            thumbBmp.c_str());
    Storage.ensureDirectoryExists(cachePath.c_str());

    if (OpenLibraryClient::downloadCoverJpeg(result.coverId, jpegTmp)) {
      LOG_INF(TAG, "JPEG downloaded to %s", jpegTmp.c_str());
      HalFile jpegFile, bmpFile;
      if (Storage.openFileForRead(TAG, jpegTmp, jpegFile) &&
          Storage.openFileForWrite(TAG, thumbBmp, bmpFile)) {
        LOG_INF(TAG, "Converting JPEG->1-bit BMP");
        if (JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpegFile, bmpFile,
                                                                 coverWidth, coverHeight)) {
          // Path stored with placeholder so getCoverThumbPath() resolves it for
          // any theme height, not just the one active at lookup time.
          thumbPath = cachePath + "/thumb_[HEIGHT].bmp";
          LOG_INF(TAG, "Conversion OK thumbPath=%s", thumbPath.c_str());
        } else {
          LOG_ERR(TAG, "1-bit BMP conversion failed");
        }
        // Both HalFiles close on destruction.
      } else {
        LOG_ERR(TAG, "Failed to open JPEG or BMP file for conversion");
      }
      // Remove temp jpeg regardless of conversion outcome.
      Storage.remove(jpegTmp.c_str());
    } else {
      LOG_ERR(TAG, "JPEG download failed for coverId=%d", result.coverId);
    }
  } else {
    LOG_INF(TAG, "No cover on OpenLibrary (coverId=0)");
  }

  // Update RecentBooks with the fresh title, author, and thumb path so the
  // home screen shows the new values immediately (before the next book open).
  LOG_INF(TAG, "updateBook thumbPath='%s'", thumbPath.c_str());
  RECENT_BOOKS.updateBook(epubPath_, result.title, result.author, thumbPath);

  state_ = SUCCESS;
}

void MetadataLookupActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_SEARCH));

  const int midY = pageHeight / 2;

  switch (state_) {
    case WAITING_FOR_WIFI:
      // WifiSelectionActivity is covering the screen; nothing to draw.
      break;

    case LOOKING_UP:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_METADATA_LOOKING_UP));
      break;

    case SUCCESS:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_METADATA_FOUND), true,
                                EpdFontFamily::BOLD);
      if (foundTitle_[0] != '\0') {
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, foundTitle_);
      }
      if (foundAuthor_[0] != '\0') {
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 30, foundAuthor_);
      }
      break;

    case NOT_FOUND:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_METADATA_NOT_FOUND), true,
                                EpdFontFamily::BOLD);
      break;

    case NO_WIFI:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_METADATA_NO_WIFI), true,
                                EpdFontFamily::BOLD);
      break;

    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_METADATA_FAILED), true,
                                EpdFontFamily::BOLD);
      break;
  }

  if (state_ != LOOKING_UP && state_ != WAITING_FOR_WIFI) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
