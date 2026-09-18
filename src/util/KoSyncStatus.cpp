#include "util/KoSyncStatus.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace {

constexpr char MARKER_DIR[] = "/.crosspoint";
constexpr char MARKER_PATH[] = "/.crosspoint/kosync_unsynced";

// Read the marker file once, then keep the answer in RAM: isUnsynced() is
// called from every header/status-bar render and must not touch the SD card.
std::atomic<bool> loaded{false};
std::atomic<bool> unsynced{false};

void ensureLoaded() {
  if (loaded.load()) return;
  unsynced.store(Storage.exists(MARKER_PATH));
  loaded.store(true);
}

}  // namespace

namespace KoSyncStatus {

bool isUnsynced() {
  ensureLoaded();
  return unsynced.load();
}

void markAttemptStarted() {
  ensureLoaded();
  if (unsynced.load()) return;  // already recorded, nothing to write

  Storage.ensureDirectoryExists(MARKER_DIR);
  HalFile file;
  if (Storage.openFileForWrite("KOS", MARKER_PATH, file)) {
    const uint8_t marker = 1;
    file.write(&marker, 1);
    file.close();
  } else {
    LOG_ERR("KOS", "Could not write sync marker file (mark is RAM-only this session)");
  }
  unsynced.store(true);
}

void markSynced() {
  ensureLoaded();
  if (!unsynced.load()) return;  // nothing recorded, nothing to clear

  const bool gone = Storage.remove(MARKER_PATH) || !Storage.exists(MARKER_PATH);
  if (gone) {
    unsynced.store(false);
  } else {
    LOG_ERR("KOS", "Could not remove sync marker file");
  }
}

void drawMark(const GfxRenderer& renderer, const int x, const int y, const int size, const bool whiteBackground) {
  if (renderer.getRenderMode() != GfxRenderer::BW) return;

  if (whiteBackground) {
    renderer.fillRect(x, y, size, size, false);
  }
  const int border = std::max(2, size / 14);
  renderer.drawRect(x, y, size, size, border, true);

  const int pad = size / 4;
  const int left = x + pad;
  const int right = x + size - 1 - pad;
  const int top = y + pad;
  const int bottom = y + size - 1 - pad;
  const int stroke = std::max(2, size / 12);
  renderer.drawLine(left, top, right, bottom, stroke, true);
  renderer.drawLine(left, bottom, right, top, stroke, true);
}

void clearGrayscaleRegion(const GfxRenderer& renderer, const int x, const int y, const int size) {
  if (renderer.getRenderMode() == GfxRenderer::BW) return;
  // In the grayscale passes a set bit means "tone this pixel gray" and a clear
  // bit means "leave it alone", so drawing "black" here clears the bits.
  renderer.fillRect(x, y, size, size, true);
}

}  // namespace KoSyncStatus
