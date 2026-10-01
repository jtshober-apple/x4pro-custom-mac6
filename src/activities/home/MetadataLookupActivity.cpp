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

#include "RecentBooksStore.h"
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
  state_ = LOOKING_UP;
  foundTitle_[0] = '\0';
  foundAuthor_[0] = '\0';
  doneAtMs_ = 0;
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
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasScreenTapped(x, y) ||
      (doneAtMs_ != 0 && (millis() - doneAtMs_) > 2500)) {
    finish();
  }
}

void MetadataLookupActivity::doWork() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF(TAG, "No WiFi — skipping metadata lookup");
    state_ = NO_WIFI;
    return;
  }

  // Guard against low-heap TLS OOM.
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < HttpDownloader::MIN_TLS_FREE_HEAP ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR(TAG, "Heap too low for TLS");
    state_ = FAILED;
    return;
  }

  OpenLibraryResult result;
  if (!OpenLibraryClient::search(titleHint_, result)) {
    state_ = NOT_FOUND;
    return;
  }

  // Copy found values into fixed buffers for the render method.
  strncpy(foundTitle_, result.title.c_str(), sizeof(foundTitle_) - 1);
  strncpy(foundAuthor_, result.author.c_str(), sizeof(foundAuthor_) - 1);

  // Persist the sidecar override — this also deletes the stale book.bin so
  // the next Epub::load() bakes the new metadata in permanently.
  MetadataOverride::save(epubPath_, result.title, result.author);

  // Download and convert the cover if one is available.
  if (result.coverId > 0) {
    const std::string cachePath = MetadataOverride::getCachePath(epubPath_);
    const std::string jpegTmp = cachePath + "/cover_tmp.jpg";
    const std::string coverBmp = cachePath + "/cover.bmp";

    Storage.ensureDirectoryExists(cachePath.c_str());

    if (OpenLibraryClient::downloadCoverJpeg(result.coverId, jpegTmp)) {
      HalFile jpegFile, bmpFile;
      if (Storage.openFileForRead(TAG, jpegTmp, jpegFile) &&
          Storage.openFileForWrite(TAG, coverBmp, bmpFile)) {
        if (!JpegToBmpConverter::jpegFileToBmpStream(jpegFile, bmpFile, /*crop=*/false)) {
          LOG_ERR(TAG, "Cover conversion failed — keeping without cover");
        }
        // Both HalFiles close on destruction.
      }
      // Remove temp jpeg regardless of conversion outcome.
      Storage.remove(jpegTmp.c_str());
    }
  }

  // Update RecentBooks with the fresh title, author, and cover path so the
  // home screen shows the new values immediately (before the next book open).
  const std::string coverBmpPath =
      result.coverId > 0 ? MetadataOverride::getCoverBmpPath(epubPath_) : std::string();
  RECENT_BOOKS.updateBook(epubPath_, result.title, result.author, coverBmpPath);

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

  if (state_ != LOOKING_UP) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
