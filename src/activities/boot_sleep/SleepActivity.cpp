#include "SleepActivity.h"

#include <Epub.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PNGdec.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"
#include "util/KoSyncStatus.h"

namespace {

// Kept separate from /sleep.bmp and /.sleep so alpha-overlay art does not mix with full-screen wallpapers.
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr char TRANSPARENT_SLEEP_DIR[] = "/.sleep-overlay";
constexpr char TRANSPARENT_SLEEP_LEGACY_DIR[] = "/sleep-overlay";
constexpr size_t MAX_SLEEP_FILE_NAME_LEN = 256;
constexpr uint8_t MIN_VISIBLE_ALPHA = 8;

struct BitmapPlacement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;
};

struct OverlayBmpInfo {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t dataOffset = 0;
  uint32_t rowBytes = 0;
};

uint16_t readLE16(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t readLE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);
  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

uint32_t readBE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return 0;
  return (static_cast<uint32_t>(c0) << 24) | (static_cast<uint32_t>(c1) << 16) | (static_cast<uint32_t>(c2) << 8) |
         static_cast<uint32_t>(c3);
}

bool isValidPngHeader(HalFile& file) {
  static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static constexpr uint32_t MAX_SOURCE_PIXELS = 2048u * 1536u;
  uint8_t signature[8];
  if (!file.seek(0) || file.read(signature, sizeof(signature)) != static_cast<int>(sizeof(signature)) ||
      !std::equal(std::begin(signature), std::end(signature), std::begin(PNG_SIGNATURE))) {
    return false;
  }

  const uint32_t ihdrLength = readBE32(file);
  char chunkType[4];
  if (file.read(reinterpret_cast<uint8_t*>(chunkType), sizeof(chunkType)) != static_cast<int>(sizeof(chunkType)) ||
      ihdrLength != 13 || !std::equal(std::begin(chunkType), std::end(chunkType), "IHDR")) {
    return false;
  }

  const uint32_t width = readBE32(file);
  const uint32_t height = readBE32(file);
  const int bitDepth = file.read();
  const int colorType = file.read();
  const int compression = file.read();
  const int filter = file.read();
  const int interlace = file.read();

  const bool supportedBitDepth =
      bitDepth == 8 || ((colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_INDEXED) &&
                        (bitDepth == 1 || bitDepth == 2 || bitDepth == 4));
  const bool supportedColorType = colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_TRUECOLOR ||
                                  colorType == PNG_PIXEL_INDEXED || colorType == PNG_PIXEL_GRAY_ALPHA ||
                                  colorType == PNG_PIXEL_TRUECOLOR_ALPHA;
  return width > 0 && height > 0 && width <= 2048 && height <= 3072 && width * height <= MAX_SOURCE_PIXELS &&
         supportedBitDepth && supportedColorType && compression == 0 && filter == 0 && interlace == 0;
}

BitmapPlacement calculateBitmapPlacement(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer) {
  BitmapPlacement placement;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (bitmapWidth > pageWidth || bitmapHeight > pageHeight) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - placement.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      placement.x = 0;
      placement.y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - placement.cropY) * static_cast<float>(bitmapHeight));
      }
      placement.x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      placement.y = 0;
    }
  } else {
    placement.x = (pageWidth - bitmapWidth) / 2;
    placement.y = (pageHeight - bitmapHeight) / 2;
  }

  return placement;
}

bool parseOverlayBmpHeader(HalFile& file, OverlayBmpInfo& info, const bool logErrors) {
  if (!file) return false;
  if (!file.seek(0)) return false;

  if (readLE16(file) != 0x4D42) {
    if (logErrors) LOG_ERR("SLP", "Transparent overlay is not a BMP");
    return false;
  }

  file.seekCur(8);
  info.dataOffset = readLE32(file);

  const uint32_t dibSize = readLE32(file);
  if (dibSize < 40) {
    if (logErrors) LOG_ERR("SLP", "Unsupported BMP DIB header: %u", static_cast<unsigned>(dibSize));
    return false;
  }

  info.width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  if (rawHeight == std::numeric_limits<int32_t>::min()) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, rawHeight);
    return false;
  }
  info.topDown = rawHeight < 0;
  info.height = info.topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  const uint16_t bpp = readLE16(file);
  const uint32_t compression = readLE32(file);

  // Match Bitmap::parseHeaders(): accept BI_RGB (0) and 32bpp BI_BITFIELDS (3), but keep the same
  // byte-layout assumption as custom sleep BMPs. The renderer below treats pixels as BGRA and does not parse masks.
  if (planes != 1 || bpp != 32 || !(compression == 0 || compression == 3)) {
    if (logErrors) {
      LOG_ERR("SLP", "Transparent overlay must be 32-bit BGRA BMP (planes=%u bpp=%u comp=%u)", planes, bpp,
              static_cast<unsigned>(compression));
    }
    return false;
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (info.width <= 0 || info.height <= 0 || info.width > MAX_IMAGE_WIDTH || info.height > MAX_IMAGE_HEIGHT) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, info.height);
    return false;
  }

  info.rowBytes = static_cast<uint32_t>(info.width) * 4u;
  if (!file.seek(info.dataOffset)) {
    if (logErrors) LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  return true;
}

uint8_t bayerThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

enum class TransparentOverlayPass : uint8_t { BW, GrayscaleLsb, GrayscaleMsb };

uint8_t quantizeOverlayLum(const uint8_t lum) {
  // Match Bitmap's native-palette path: 0, 85, 170, 255 map directly to levels 0..3.
  return lum >> 6;
}

bool renderTransparentOverlayPass(HalFile& file, const OverlayBmpInfo& info, const BitmapPlacement& placement,
                                  const GfxRenderer& renderer, uint8_t* row, const TransparentOverlayPass pass) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int cropPixX = std::floor(info.width * placement.cropX / 2.0f);
  const int cropPixY = std::floor(info.height * placement.cropY / 2.0f);
  const float croppedWidth = (1.0f - placement.cropX) * static_cast<float>(info.width);
  const float croppedHeight = (1.0f - placement.cropY) * static_cast<float>(info.height);

  float scale = 1.0f;
  if (croppedWidth > 0.0f && croppedHeight > 0.0f) {
    const float widthScale = static_cast<float>(pageWidth) / croppedWidth;
    const float heightScale = static_cast<float>(pageHeight) / croppedHeight;
    scale = std::min(widthScale, heightScale);
    if (scale > 1.0f) scale = 1.0f;
  }
  const bool isScaled = scale < 1.0f;

  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read in transparent overlay row %d", bmpY);
      return false;
    }

    int screenY = -cropPixY + (info.topDown ? bmpY : info.height - 1 - bmpY);
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += placement.y;

    if (screenY >= pageHeight) {
      if (info.topDown) break;
      continue;
    }
    if (screenY < 0) {
      if (!info.topDown) break;
      continue;
    }

    for (int bmpX = cropPixX; bmpX < info.width - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) screenX = std::floor(screenX * scale);
      screenX += placement.x;

      if (screenX >= renderer.getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t* pixel = row + (static_cast<size_t>(bmpX) * 4u);
      const uint8_t alpha = pixel[3];
      if (alpha < MIN_VISIBLE_ALPHA || alpha <= bayerThreshold4x4(screenX, screenY)) continue;

      const uint8_t lum = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
      const uint8_t level = quantizeOverlayLum(lum);

      switch (pass) {
        case TransparentOverlayPass::BW:
          // Same first pass as custom bitmap sleep: all non-white levels are painted black.
          // Transparent overlay's only difference is that opaque white explicitly erases underlying text.
          renderer.drawPixel(screenX, screenY, level < 3);
          break;
        case TransparentOverlayPass::GrayscaleLsb:
          if (level == 1) renderer.drawPixel(screenX, screenY, false);
          break;
        case TransparentOverlayPass::GrayscaleMsb:
          if (level == 1 || level == 2) renderer.drawPixel(screenX, screenY, false);
          break;
      }
    }
  }

  return true;
}

enum class AlphaOverlayResult : uint8_t { Rendered, NotAlphaOverlay, Error };
enum class AlphaScanResult : uint8_t { Useful, NotUseful, Error };

AlphaScanResult scanForUsefulAlpha(HalFile& file, const OverlayBmpInfo& info, uint8_t* row) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return AlphaScanResult::Error;
  }

  bool hasVisiblePixel = false;
  bool hasNonOpaquePixel = false;
  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read while checking transparent overlay row %d", bmpY);
      return AlphaScanResult::Error;
    }

    for (int bmpX = 0; bmpX < info.width; bmpX++) {
      const uint8_t alpha = row[static_cast<size_t>(bmpX) * 4u + 3u];
      hasVisiblePixel |= alpha >= MIN_VISIBLE_ALPHA;
      hasNonOpaquePixel |= alpha < 255;
      if (hasVisiblePixel && hasNonOpaquePixel) return AlphaScanResult::Useful;
    }
  }

  return AlphaScanResult::NotUseful;
}

AlphaOverlayResult tryRenderTransparentOverlayBmp(HalFile& file, GfxRenderer& renderer, const char* pathForLog) {
  OverlayBmpInfo info;
  if (!parseOverlayBmpHeader(file, info, false)) return AlphaOverlayResult::NotAlphaOverlay;

  const auto placement = calculateBitmapPlacement(info.width, info.height, renderer);
  auto row = makeUniqueNoThrow<uint8_t[]>(info.rowBytes);
  if (!row) {
    LOG_ERR("SLP", "OOM: transparent overlay row (%u bytes)", static_cast<unsigned>(info.rowBytes));
    return AlphaOverlayResult::Error;
  }

  const auto alphaScanResult = scanForUsefulAlpha(file, info, row.get());
  if (alphaScanResult == AlphaScanResult::Error) return AlphaOverlayResult::Error;
  if (alphaScanResult == AlphaScanResult::NotUseful) return AlphaOverlayResult::NotAlphaOverlay;

  LOG_DBG("SLP", "Rendering transparent overlay: %s (%dx%d)", pathForLog, info.width, info.height);

  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::BW))
    return AlphaOverlayResult::Error;
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleLsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    // The BW composite is already on the panel. Keep it instead of falling
    // through to another overlay with this grayscale work buffer cleared.
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleMsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return AlphaOverlayResult::Rendered;
}

enum class SleepRecentKind : uint8_t { Standard, Overlay };

bool isRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx, const uint8_t window) {
  return recentKind == SleepRecentKind::Overlay ? APP_STATE.isRecentOverlaySleep(idx, window)
                                                : APP_STATE.isRecentSleep(idx, window);
}

void pushRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx) {
  if (recentKind == SleepRecentKind::Overlay) {
    APP_STATE.pushRecentOverlaySleep(idx);
  } else {
    APP_STATE.pushRecentSleep(idx);
  }
}

bool findNextValidSleepImage(HalFile& dir, const SleepRecentKind recentKind, char* name) {
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) continue;

    dirFile.getName(name, MAX_SLEEP_FILE_NAME_LEN);
    if (name[0] == '\0' || name[0] == '.') continue;

    const bool isBmp = FsHelpers::hasBmpExtension(name);
    const bool isPng = recentKind == SleepRecentKind::Overlay && FsHelpers::hasPngExtension(std::string_view{name});
    if (!isBmp && !isPng) {
      LOG_DBG("SLP", "Skipping unsupported sleep image: %s", name);
      continue;
    }

    const bool isValid = isBmp ? [&dirFile]() {
      Bitmap bitmap(dirFile);
      return bitmap.parseHeaders() == BmpReaderError::Ok;
    }()
                               : isValidPngHeader(dirFile);
    if (!isValid) {
      LOG_DBG("SLP", "Skipping invalid sleep image: %s", name);
      continue;
    }
    return true;
  }
  return false;
}

bool selectRandomSleepFile(const char* dirPath, const SleepRecentKind recentKind, std::string& selectedPath) {
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;

  auto name = makeUniqueNoThrow<char[]>(MAX_SLEEP_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR("SLP", "OOM: sleep filename buffer");
    return false;
  }

  uint16_t fileCount = 0;
  while (fileCount < UINT16_MAX && findNextValidSleepImage(dir, recentKind, name.get())) ++fileCount;
  if (fileCount == 0) return false;

  // Pick a random wallpaper, excluding recently shown ones.
  // Window: up to SLEEP_RECENT_COUNT entries, capped at fileCount-1.
  const uint8_t recentFill =
      recentKind == SleepRecentKind::Overlay ? APP_STATE.recentOverlaySleepFill : APP_STATE.recentSleepFill;
  const uint8_t window = static_cast<uint8_t>(std::min<uint16_t>(recentFill, fileCount - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && isRecentSleepIndex(recentKind, randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  dir.rewindDirectory();
  for (uint16_t index = 0; index <= randomFileIndex; ++index) {
    if (!findNextValidSleepImage(dir, recentKind, name.get())) return false;
  }

  selectedPath.reserve(strlen(dirPath) + 1 + strlen(name.get()));
  selectedPath = dirPath;
  selectedPath += "/";
  selectedPath += name.get();
  pushRecentSleepIndex(recentKind, randomFileIndex);
  APP_STATE.saveToFile();
  return true;
}

bool drawSleepPopupPreservingFrame(GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int frameThickness = metrics.popupFrameThickness;
  const int popupY = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int popupHeight = renderer.getLineHeight(UI_12_FONT_ID) + metrics.popupMarginY * 2;
  const int bandTop = std::max(0, popupY - frameThickness);
  const int bandBottom = std::min(renderer.getScreenHeight(), popupY + popupHeight + frameThickness);
  const int bandHeight = bandBottom - bandTop;
  const size_t bandBytes = renderer.getRegionByteSize(0, bandTop, renderer.getScreenWidth(), bandHeight);

  auto savedBand = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  if (!savedBand) {
    LOG_ERR("SLP", "OOM: sleep popup background (%u bytes)", static_cast<unsigned>(bandBytes));
    return false;
  }
  if (!renderer.copyRegionToBuffer(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to save sleep popup background");
    return false;
  }

  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  if (!renderer.copyBufferToRegion(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to restore sleep popup background");
    return false;
  }
  return true;
}

void releaseSdFontCachesForDecode(const GfxRenderer& renderer) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("SLP", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("SLP", "Free heap before sleep image decode: %d bytes", ESP.getFreeHeap());
  }
}

// "Last KOSync attempt failed" mark, pinned to the top-right corner of the
// sleep screen (white box so it reads over any cover or custom image). The
// sleep-time sync has already finished by the time a sleep screen is drawn, so
// this reflects its result.
constexpr int SYNC_MARK_SIZE = 44;
constexpr int SYNC_MARK_INSET = 16;

void drawSyncMarkIfNeeded(const GfxRenderer& renderer) {
  if (!KoSyncStatus::isUnsynced()) return;
  KoSyncStatus::drawMark(renderer, renderer.getScreenWidth() - SYNC_MARK_INSET - SYNC_MARK_SIZE, SYNC_MARK_INSET,
                         SYNC_MARK_SIZE, /*whiteBackground=*/true);
}

// Call after drawing the image in each grayscale pass, so the mark's box stays
// pure black/white instead of picking up the image's gray tones.
void clearSyncMarkForGrayscale(const GfxRenderer& renderer) {
  if (!KoSyncStatus::isUnsynced()) return;
  KoSyncStatus::clearGrayscaleRegion(renderer, renderer.getScreenWidth() - SYNC_MARK_INSET - SYNC_MARK_SIZE,
                                     SYNC_MARK_INSET, SYNC_MARK_SIZE);
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool frameWasInverted = display.isInverted();

  // Sleep screens always use normal polarity. This activity draws directly
  // from onEnter (outside ActivityManager's per-render polarity resolution),
  // so clear any inversion left over from a night-mode reader render.
  display.setInverted(false);

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM) {
    // Transparent mode retains the current framebuffer. Materialize any
    // output-level inversion first so the retained content keeps its visible
    // polarity after the display driver returns to normal.
    if (frameWasInverted) renderer.invertScreen();
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    }
    drawSleepPopupPreservingFrame(renderer);
    if (APP_STATE.lastSleepFromReader) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    }
    releaseSdFontCachesForDecode(renderer);
    return renderTransparentCustomSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::SYSTEM6_MAC):
      return renderSystem6SleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  std::string selectedPath;
  if (!selectRandomSleepFile("/.sleep", SleepRecentKind::Standard, selectedPath)) {
    selectRandomSleepFile("/sleep", SleepRecentKind::Standard, selectedPath);
  }

  if (!selectedPath.empty()) {
    HalFile randFile;
    if (Storage.openFileForRead("SLP", selectedPath, randFile)) {
      LOG_DBG("SLP", "Randomly loading: %s", selectedPath.c_str());
      delay(100);
      Bitmap bitmap(randFile, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap);
        randFile.close();
        return;
      }
      randFile.close();
    }
  }

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  drawSyncMarkIfNeeded(renderer);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement = calculateBitmapPlacement(bitmap.getWidth(), bitmap.getHeight(), renderer);
  const int x = placement.x;
  const int y = placement.y;
  const float cropX = placement.cropX;
  const float cropY = placement.cropY;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground) renderer.clearScreen();

  const bool hasGreyscale =
      bitmap.hasGreyscale() && (preserveBackground || SETTINGS.sleepScreenCoverFilter ==
                                                          CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (!preserveBackground &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Drawn after the optional invert so the mark is never inverted with the image.
  drawSyncMarkIfNeeded(renderer);

  if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    clearSyncMarkForGrayscale(renderer);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    clearSyncMarkForGrayscale(renderer);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

bool SleepActivity::renderSleepOverlayFile(HalFile& file, const char* pathForLog) const {
  const auto alphaResult = tryRenderTransparentOverlayBmp(file, renderer, pathForLog);
  if (alphaResult == AlphaOverlayResult::Rendered) return true;
  if (alphaResult == AlphaOverlayResult::Error) return false;

  Bitmap bitmap(file);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid sleep overlay BMP %s: %s", pathForLog, Bitmap::errorToString(parseResult));
    return false;
  }

  LOG_DBG("SLP", "Rendering regular BMP sleep overlay: %s (%dx%d)", pathForLog, bitmap.getWidth(), bitmap.getHeight());
  // drawBitmap leaves white pixels untouched; skipping the initial clear makes
  // them transparent while retaining the existing grayscale pipeline.
  renderBitmapSleepScreen(bitmap, true);
  return true;
}

bool SleepActivity::renderTransparentOverlayPng(const std::string& path) const {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dimensions)) return false;

  const auto placement = calculateBitmapPlacement(dimensions.width, dimensions.height, renderer);
  RenderConfig config;
  config.x = placement.x;
  config.y = placement.y;
  config.maxWidth = renderer.getScreenWidth();
  config.maxHeight = renderer.getScreenHeight();
  config.useDithering = false;
  config.sourceCropX = placement.cropX;
  config.sourceCropY = placement.cropY;
  config.useExactDimensions = placement.cropX > 0.0f || placement.cropY > 0.0f;
  config.preserveAlpha = true;

  PngToFramebufferConverter converter;
  LOG_DBG("SLP", "Rendering transparent PNG overlay: %s (%dx%d)", path.c_str(), dimensions.width, dimensions.height);

  if (!converter.decodeToFramebuffer(path, renderer, config)) return false;
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

bool SleepActivity::renderSleepOverlayPath(const std::string& path) const {
  if (FsHelpers::hasPngExtension(path)) {
    return Storage.exists(path.c_str()) && renderTransparentOverlayPng(path);
  }

  HalFile file;
  return Storage.openFileForRead("SLP", path, file) && renderSleepOverlayFile(file, path.c_str());
}

void SleepActivity::renderTransparentCustomSleepScreen() const {
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_BMP)) return;
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_PNG)) return;

  std::string selectedPath;
  if (!selectRandomSleepFile(TRANSPARENT_SLEEP_DIR, SleepRecentKind::Overlay, selectedPath)) {
    selectRandomSleepFile(TRANSPARENT_SLEEP_LEGACY_DIR, SleepRecentKind::Overlay, selectedPath);
  }

  if (!selectedPath.empty() && renderSleepOverlayPath(selectedPath)) return;

  LOG_ERR("SLP", "No valid transparent sleep overlay found");
  renderDefaultSleepScreen();
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  if (gpio.deviceIsX3()) {
    // The controller still holds the displayed page, so its differential base
    // waveform can add the moon without a full-screen flash.
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  drawSyncMarkIfNeeded(renderer);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

// ─────────────────────────────────────────────────────────────────────────────
// System 6 / Mac Plus monitor sleep screen
// ─────────────────────────────────────────────────────────────────────────────
//
// Draws a monochrome Mac Plus monitor silhouette on the e-ink screen.
// If a cover BMP is available for the current book, it fills the
// monitor's "glass" area.  When there is no cover, a byte-perfect
// System 6 desktop scene is rendered instead: menu bar, grey
// checkerboard, an open Finder window with the book title, a Hard
// Disk icon and a Trash icon.
//
// All geometry is computed from the live screen dimensions so the
// layout is orientation-agnostic.

namespace {

// ── primitive helpers (stand-alone; do not depend on System6Theme) ──────────

// One-bit checkerboard over a subrect.
void mac6Checkerboard(const GfxRenderer& r, int x, int y, int w, int h) {
  for (int row = 0; row < h; ++row) {
    const int phase = ((y + row) / 2) & 1;
    for (int col = phase * 2; col < w; col += 4)
      r.fillRect(x + col, y + row, std::min(2, w - col), 1);
  }
}

// Outline rectangle with a second border 2px inside (Mac double-frame style).
void mac6Frame(const GfxRenderer& r, int x, int y, int w, int h) {
  if (w < 8 || h < 8) return;
  // drop shadow
  r.fillRect(x + 3, y + 3, w, h);
  // white fill
  r.fillRect(x, y, w, h, false);
  // outer and inner borders
  r.drawRect(x, y, w, h);
  r.drawRect(x + 2, y + 2, w - 4, h - 4);
}

// Small document icon (22×26).
void mac6DocIcon(const GfxRenderer& r, int x, int y, bool black = true) {
  r.drawRect(x, y, 22, 26, black);
  r.drawLine(x + 15, y, x + 15, y + 7, black);
  r.drawLine(x + 15, y + 7, x + 21, y + 7, black);
  for (int line = 12; line <= 20; line += 4)
    r.drawLine(x + 4, y + line, x + 16, y + line, black);
}

// Simple trash can (24×28 + label) — horizontal ribs (authentic System 6 style).
void mac6Trash(const GfxRenderer& r, int x, int y, bool black = true) {
  // handle (centered)
  r.drawRect(x + 7, y + 1, 10, 6, black);
  // lid (extends past body)
  r.drawLine(x - 2, y + 7, x + 26, y + 7, black);
  // body
  r.drawRect(x, y + 7, 24, 20, black);
  // horizontal ribs
  for (int ry = y + 11; ry < y + 26; ry += 4)
    r.drawLine(x + 1, ry, x + 22, ry, black);
}

// 3.5-inch floppy disk icon (28×26).
void mac6Floppy(const GfxRenderer& r, int x, int y, bool black = true) {
  // outer body
  r.drawRect(x, y, 28, 26, black);
  // label area (white)
  r.fillRect(x + 1, y + 1, 26, 10, false);
  // label slot border
  r.drawRect(x + 2, y + 2, 17, 7, black);
  // shutter / metal shell (lower portion, dark)
  r.fillRect(x + 1, y + 11, 26, 14, black);
  // sliding door cutout (white rectangle in shutter)
  r.fillRect(x + 3, y + 12, 16, 12, false);
  // hub border
  r.drawRect(x + 20, y + 13, 6, 8, black);
  // hub interior (white)
  r.fillRect(x + 21, y + 14, 4, 6, false);
}

// Hard-disk icon (28×22 + label).
void mac6HardDisk(const GfxRenderer& r, int x, int y, bool black = true) {
  r.drawRect(x, y, 28, 22, black);
  // Label stripe
  r.fillRect(x + 2, y + 2, 24, 7, black);
  // Platter dots
  r.fillRect(x + 4, y + 14, 4, 4, black);
  r.fillRect(x + 20, y + 14, 4, 4, black);
}

// Truncate text to fit `maxW` pixels wide (font SMALL_FONT_ID).
void mac6TruncLabel(const GfxRenderer& r, const char* text, int maxW, char* out, int outLen) {
  if (!text || outLen <= 0) { if (out) out[0] = '\0'; return; }
  int n = 0;
  while (n < outLen - 1 && text[n]) { out[n] = text[n]; ++n; }
  out[n] = '\0';
  if (r.getTextWidth(SMALL_FONT_ID, out) <= maxW) return;
  const char* kEll = "...";
  while (n > 0) {
    --n;
    while (n && (static_cast<unsigned char>(out[n]) & 0xc0) == 0x80) --n;
    if (n + 3 < outLen) {
      std::memcpy(out + n, kEll, 4);
      if (r.getTextWidth(SMALL_FONT_ID, out) <= maxW) return;
    }
  }
  std::memcpy(out, kEll, 4);
}

}  // namespace (anonymous, System 6 helpers)

// ── renderSystem6SleepScreen ─────────────────────────────────────────────────

void SleepActivity::renderSystem6SleepScreen() const {
  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  // ── Monitor geometry ──────────────────────────────────────────────────────
  // The Mac Plus reference is 296 × 362 px.  The case fills the entire display.
  // Horizontal dims scale from W/296; vertical dims scale from H/362 so the
  // screen glass and bottom section each keep their correct share of the height.
  auto sc = [W](int v) -> int { return v * W / 296; };  // horizontal
  auto sv = [H](int v) -> int { return v * H / 362; };  // vertical

  const int CASE_W = W;
  const int CASE_H = H;
  const int caseX  = 0;
  const int caseY  = 0;

  // Screen "glass" area inside the bezel.
  // Left/right bezel scales with width; top bezel and screen height with height.
  const int BEZ_LR  = sc(38);
  const int BEZ_TOP = sv(36);
  int SCR_W   = CASE_W - BEZ_LR * 2;
  int SCR_H   = sv(166);
  int scrX = caseX + BEZ_LR;
  int scrY = caseY + BEZ_TOP;

  // Bottom section metrics.
  const int botY = scrY + SCR_H;                  // top of floppy/speaker zone
  const int botH = (caseY + CASE_H) - botY;        // pixels remaining in case

  // ── Step 1: clear the screen ──────────────────────────────────────────────
  renderer.clearScreen();

  // ── Custom frame (optional) ───────────────────────────────────────────────
  // Drop /mac-frame.bmp (480×800) on the SD card root to replace the built-in
  // programmatic Mac body.  The image must be the full display size with the
  // screen viewport area left as solid white; the desktop scene renders on top
  // and naturally fills that region.  When the file is absent the programmatic
  // case is drawn instead.
  static constexpr char MAC_FRAME_BMP[]        = "/.mac-frame.bmp";
  static constexpr char MAC_FRAME_CFG_HIDDEN[] = "/.mac-frame.cfg";  // dot-prefix (hidden on macOS)
  static constexpr char MAC_FRAME_CFG_PLAIN[]  = "/mac-frame.cfg";   // plain name (easier to create)
  const bool hasCustomFrame = [&]() -> bool {
    if (!Storage.exists(MAC_FRAME_BMP)) return false;
    HalFile f;
    if (!Storage.openFileForRead("SLP", MAC_FRAME_BMP, f)) return false;
    Bitmap bmp(f);
    if (bmp.parseHeaders() != BmpReaderError::Ok) return false;
    renderer.drawBitmap(bmp, 0, 0, W, H, 0.0f, 0.0f);
    // Optional viewport override + inversion: place mac-frame.cfg (or .mac-frame.cfg) on the SD
    // card alongside .mac-frame.bmp with keys "vx=N", "vy=N", "vw=N", "vh=N" to relocate the
    // desktop scene, and "inv=1" to invert the frame image colors (useful when a beige/tan case
    // renders too dark on e-ink).  Both dot-prefix and plain filename are accepted so the file can
    // be created without Terminal.
    const char* cfgPath = Storage.exists(MAC_FRAME_CFG_HIDDEN) ? MAC_FRAME_CFG_HIDDEN
                        : Storage.exists(MAC_FRAME_CFG_PLAIN)  ? MAC_FRAME_CFG_PLAIN
                        : nullptr;
    char cfgBuf[128] = {};
    if (cfgPath && Storage.readFileToBuffer(cfgPath, cfgBuf, sizeof(cfgBuf)) > 0) {
      LOG_DBG("SLP", "mac-frame.cfg (%s): %s", cfgPath, cfgBuf);
      auto parseKey = [](const char* hay, const char* key) -> int {
        const char* p = strstr(hay, key);
        if (!p) return -1;
        p += strlen(key);
        return (*p == '=') ? atoi(p + 1) : -1;
      };
      const int nx = parseKey(cfgBuf, "vx"), ny = parseKey(cfgBuf, "vy");
      const int nw = parseKey(cfgBuf, "vw"), nh = parseKey(cfgBuf, "vh");
      const int ninv = parseKey(cfgBuf, "inv");
      LOG_DBG("SLP", "cfg parsed: vx=%d vy=%d vw=%d vh=%d inv=%d", nx, ny, nw, nh, ninv);
      if (nx >= 0 && ny >= 0 && nw > 0 && nh > 0) {
        scrX = nx; scrY = ny; SCR_W = nw; SCR_H = nh;
        LOG_DBG("SLP", "viewport override applied: x=%d y=%d w=%d h=%d", scrX, scrY, SCR_W, SCR_H);
      } else {
        LOG_ERR("SLP", "mac-frame.cfg: missing or invalid keys (vx=%d vy=%d vw=%d vh=%d)", nx, ny, nw, nh);
      }
      if (ninv == 1) {
        // Invert the entire framebuffer to flip frame colors, then restore the
        // viewport region to white so the desktop scene renders on a clean ground.
        renderer.invertScreen();
        renderer.fillRect(scrX, scrY, SCR_W, SCR_H, false);
        LOG_DBG("SLP", "frame inverted; viewport restored to white");
      }
    } else {
      LOG_INF("SLP", "no mac-frame.cfg found; using default viewport");
    }
    return true;
  }();

  // ── Step 2: cover image (if any) ─────────────────────────────────────────
  // Try to locate a cover BMP for the current book via RECENT_BOOKS so we
  // avoid loading the entire EPUB again.
  bool hasCover = false;
  if (!APP_STATE.openEpubPath.empty()) {
    const auto& books = RECENT_BOOKS.getBooks();
    for (const auto& book : books) {
      if (book.path != APP_STATE.openEpubPath) continue;
      if (book.coverBmpPath.empty()) break;

      // The full-res cover BMP lives at book.coverBmpPath; the thumbnail
      // at a height-derived path.  Try the thumbnail first (it's already
      // small enough for the 220-px monitor screen), then fall back to
      // the full-res version.
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, SCR_H);
      const char* tryPaths[2] = { thumbPath.c_str(), book.coverBmpPath.c_str() };
      for (const char* path : tryPaths) {
        if (!Storage.exists(path)) continue;
        HalFile file;
        if (!Storage.openFileForRead("SLP", path, file)) continue;
        Bitmap bmp(file);
        if (bmp.parseHeaders() != BmpReaderError::Ok) { file.close(); continue; }

        // Draw centered inside the screen rect.
        // drawBitmap(bmp, x, y, clipW, clipH, cropX, cropY):
        //   x/y = top-left of the image on screen,
        //   clipW/clipH = maximum width/height to draw.
        // We centre the bitmap within the screen rect; drawBitmap clips to clipW×clipH.
        const int bw = bmp.getWidth();
        const int bh = bmp.getHeight();
        // Scale to fit: find the largest scale where bw*s <= SCR_W and bh*s <= SCR_H.
        // drawBitmap does not scale, so we offset to centre and clip to the rect.
        const int offX = scrX + std::max(0, (SCR_W - bw) / 2);
        const int offY = scrY + std::max(0, (SCR_H - bh) / 2);
        renderer.drawBitmap(bmp, offX, offY, W, H, 0.0f, 0.0f);
        file.close();
        hasCover = true;
        break;
      }
      break;
    }
  }

  // ── Step 3: System 6 desktop scene (no cover) ─────────────────────────────
  if (!hasCover) {
    // ── Desktop background ────────────────────────────────────────────────────
    // Drop mac-desktop.bmp (480×800, 1-bit BMP) on the SD card root to supply
    // the full desktop image (chassis frame, menu bar, icons — everything).
    // When absent, a plain checkerboard fills the viewport as a fallback.
    // Plain name (no dot prefix) so macOS Finder does not hide the file when
    // the user copies it from the unzipped export to the SD card.
    static constexpr char MAC_DESKTOP_BMP[] = "/mac-desktop.bmp";
    const bool hasDesktopImage = [&]() -> bool {
      if (!Storage.exists(MAC_DESKTOP_BMP)) return false;
      HalFile df;
      if (!Storage.openFileForRead("SLP", MAC_DESKTOP_BMP, df)) return false;
      Bitmap dbmp(df);
      const BmpReaderError parseResult = dbmp.parseHeaders();
      if (parseResult != BmpReaderError::Ok) {
        LOG_ERR("SLP", "mac-desktop.bmp parse error: %s", Bitmap::errorToString(parseResult));
        return false;
      }
      renderer.drawBitmap(dbmp, 0, 0, W, H, 0.0f, 0.0f);
      return true;
    }();
    if (!hasDesktopImage)
      mac6Checkerboard(renderer, scrX, scrY, SCR_W, SCR_H);

    // ── Book Finder window — the only thing drawn programmatically ────────────
    // Derive book title from open EPUB.
    std::string bookTitle;
    if (!APP_STATE.openEpubPath.empty()) {
      const auto& books = RECENT_BOOKS.getBooks();
      for (const auto& book : books) {
        if (book.path == APP_STATE.openEpubPath && !book.title.empty()) {
          bookTitle = book.title;
          break;
        }
      }
      if (bookTitle.empty()) {
        const auto slash = APP_STATE.openEpubPath.rfind('/');
        const auto dot   = APP_STATE.openEpubPath.rfind('.');
        const size_t s   = (slash == std::string::npos ? 0 : slash + 1);
        const size_t e   = (dot   == std::string::npos || dot < s
                              ? APP_STATE.openEpubPath.size() : dot);
        bookTitle = APP_STATE.openEpubPath.substr(s, e - s);
      }
    }
    if (bookTitle.empty()) bookTitle = "Documents";

    const int WIN_W = sc(145);
    const int WIN_H = sc(98);
    const int winX  = 42;
    const int winY  = 241;
    const int SB    = sc(11);
    const int TB_H  = sc(11);
    mac6Frame(renderer, winX, winY, WIN_W, WIN_H);

    // Title bar: venetian-blind stripe pattern (active window).
    renderer.fillRect(winX + 1, winY + 1, WIN_W - 2, TB_H, false);
    for (int row = winY + 1; row < winY + 1 + TB_H; row += 2)
      renderer.fillRect(winX + 1, row, WIN_W - 2, 1);

    // Close box.
    renderer.drawRect(winX + 1,     winY + sc(2), sc(9), sc(9), false);
    renderer.drawRect(winX + sc(2), winY + sc(3), sc(7), sc(7), false);

    // Zoom box (top-right of title bar).
    const int zbX = winX + WIN_W - SB - 1;
    renderer.drawRect(zbX,     winY + sc(2), sc(9), sc(9), false);
    renderer.drawRect(zbX + 1, winY + sc(3), sc(7), sc(7), false);

    // Sync-pending indicator: small filled square inside the zoom box when
    // unsynced, replacing the decorative inner rect so it stays within our
    // bounds and is visible against the stripe background.
    if (KoSyncStatus::isUnsynced()) {
      const int dotSz = std::max(3, SB / 3);
      renderer.fillRect(zbX + (SB - dotSz) / 2, winY + (TB_H - dotSz) / 2,
                        dotSz, dotSz);  // black dot in zoom-box area
    }

    // Window title (white on stripes, centred between close box and zoom box).
    {
      char wtitle[40];
      const int titleAreaW = zbX - (winX + sc(12));
      mac6TruncLabel(renderer, bookTitle.c_str(), titleAreaW, wtitle, sizeof(wtitle));
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, wtitle);
      renderer.drawText(SMALL_FONT_ID,
                        winX + sc(12) + std::max(0, (titleAreaW - tw) / 2),
                        winY + 2, wtitle, /*black=*/false);
    }

    // Content area (white fill).
    const int contW = WIN_W - SB - 2;
    const int contH = WIN_H - TB_H - SB - 2;
    renderer.fillRect(winX + 1, winY + TB_H + 1, contW, contH, false);

    // Header separator row.
    const int HDR_H = sc(10);
    const int hdrY  = winY + TB_H + 1;
    renderer.fillRect(winX + 1, hdrY + HDR_H - 1, contW, 1);
    renderer.drawText(SMALL_FONT_ID, winX + sc(6), hdrY + 1, "Name");

    // File entry.
    const int fileY = hdrY + HDR_H + sc(3);
    mac6DocIcon(renderer, winX + sc(4), fileY);
    {
      char ftitle[40];
      mac6TruncLabel(renderer, bookTitle.c_str(), contW - sc(28), ftitle, sizeof(ftitle));
      renderer.drawText(SMALL_FONT_ID, winX + sc(28), fileY + sc(4), ftitle);
    }

    // ── Right scroll bar ──────────────────────────────────────────────────────
    const int rsbX    = winX + WIN_W - SB - 1;
    const int rsbTopY = winY + TB_H + 1;
    const int rsbBotY = winY + WIN_H - SB - 1;

    renderer.drawRect(rsbX, rsbTopY, SB, SB);
    renderer.fillRect(rsbX + 1, rsbTopY + 1, SB - 2, SB - 2, false);
    {
      const int cx = rsbX + SB / 2, ty = rsbTopY + 2;
      renderer.fillRect(cx,     ty,     1, 1);
      renderer.fillRect(cx - 1, ty + 1, 3, 1);
      renderer.fillRect(cx - 2, ty + 2, 5, 1);
      if (SB > 14) renderer.fillRect(cx - 3, ty + 3, 7, 1);
    }
    mac6Checkerboard(renderer, rsbX, rsbTopY + SB, SB, rsbBotY - (rsbTopY + SB));
    {
      const int rTrackH = rsbBotY - (rsbTopY + SB);
      const int thumbH  = std::max(SB, rTrackH / 4);
      renderer.fillRect(rsbX + 1, rsbTopY + SB + 1, SB - 2, thumbH, false);
      renderer.drawRect(rsbX + 1, rsbTopY + SB + 1, SB - 2, thumbH);
    }
    renderer.drawRect(rsbX, rsbBotY, SB, SB);
    renderer.fillRect(rsbX + 1, rsbBotY + 1, SB - 2, SB - 2, false);
    {
      const int cx = rsbX + SB / 2, ty = rsbBotY + 2;
      if (SB > 14) renderer.fillRect(cx - 3, ty,     7, 1);
      renderer.fillRect(cx - 2, ty + (SB > 14 ? 1 : 0), 5, 1);
      renderer.fillRect(cx - 1, ty + (SB > 14 ? 2 : 1), 3, 1);
      renderer.fillRect(cx,     ty + (SB > 14 ? 3 : 2), 1, 1);
    }

    // ── Bottom scroll bar ─────────────────────────────────────────────────────
    const int bsbY      = winY + WIN_H - SB - 1;
    const int bsbRightX = rsbX - SB;

    renderer.fillRect(winX + 1, bsbY, contW - 1, SB, false);

    renderer.drawRect(winX + 1, bsbY, SB, SB);
    renderer.fillRect(winX + 2, bsbY + 1, SB - 2, SB - 2, false);
    {
      const int cy = bsbY + SB / 2, lx = winX + SB - 2;
      renderer.fillRect(lx,     cy,     1, 1);
      renderer.fillRect(lx - 1, cy - 1, 1, 3);
      renderer.fillRect(lx - 2, cy - 2, 1, 5);
      if (SB > 14) renderer.fillRect(lx - 3, cy - 3, 1, 7);
    }
    {
      const int bTrackLeft = winX + 1 + SB;
      mac6Checkerboard(renderer, bTrackLeft, bsbY + 1, bsbRightX - bTrackLeft, SB - 2);
      const int bTrackW = bsbRightX - bTrackLeft;
      const int bthumbW = std::max(SB, bTrackW / 4);
      renderer.fillRect(bTrackLeft + 1, bsbY + 1, bthumbW, SB - 2, false);
      renderer.drawRect(bTrackLeft + 1, bsbY + 1, bthumbW, SB - 2);
    }
    renderer.drawRect(bsbRightX, bsbY, SB, SB);
    renderer.fillRect(bsbRightX + 1, bsbY + 1, SB - 2, SB - 2, false);
    {
      const int cy = bsbY + SB / 2, rx = bsbRightX + 2;
      renderer.fillRect(rx,     cy,     1, 1);
      renderer.fillRect(rx + 1, cy - 1, 1, 3);
      renderer.fillRect(rx + 2, cy - 2, 1, 5);
      if (SB > 14) renderer.fillRect(rx + 3, cy - 3, 1, 7);
    }

    // Size box (lower-right corner).
    renderer.fillRect(rsbX, bsbY, SB + 1, SB + 1, false);
    renderer.drawRect(rsbX + 1, bsbY + 1, SB - 3, SB - 3);
    renderer.fillRect(rsbX + 2, bsbY + 2, SB - 5, SB - 5, false);
    renderer.drawRect(rsbX + 3, bsbY + 3, SB - 5, SB - 5);
  }

  // ── Step 4: monitor case frame (skipped when a custom frame image is used) ─
  if (!hasCustomFrame) {
    // Mask everything outside the case with white.
    renderer.fillRect(0, 0, caseX, H, false);
    renderer.fillRect(caseX + CASE_W, 0, W - (caseX + CASE_W), H, false);
    renderer.fillRect(caseX, 0, CASE_W, caseY, false);
    renderer.fillRect(caseX, caseY + CASE_H, CASE_W, H - (caseY + CASE_H), false);

    // Bezel areas (white, mask cover image or desktop outside the glass).
    renderer.fillRect(caseX, caseY, CASE_W, BEZ_TOP, false);           // top bezel
    renderer.fillRect(caseX, botY, CASE_W, botH, false);               // bottom section
    renderer.fillRect(caseX, scrY, BEZ_LR, SCR_H, false);             // left bezel
    renderer.fillRect(scrX + SCR_W, scrY, BEZ_LR, SCR_H, false);     // right bezel

    // Screen surround (double thin border around the glass).
    renderer.drawRect(scrX - sc(2), scrY - sc(2), SCR_W + sc(4), SCR_H + sc(4));
    renderer.drawRect(scrX - sc(1), scrY - sc(1), SCR_W + sc(2), SCR_H + sc(2));

    // Outer case border (double-line for thickness).
    renderer.drawRect(caseX, caseY, CASE_W, CASE_H);
    renderer.drawRect(caseX + 1, caseY + 1, CASE_W - 2, CASE_H - 2);

    // Separator between screen bezel and bottom section.
    renderer.drawLine(caseX + 1, botY, caseX + CASE_W - 2, botY);

    // ── Floppy disk slot (centred in bottom section, ~1/3 down) ────────────
    const int FLOP_W = sc(76), FLOP_H = sc(8);
    const int flopX = caseX + (CASE_W - FLOP_W) / 2;
    const int flopY = botY + botH / 3 - FLOP_H / 2;
    renderer.drawRect(flopX, flopY, FLOP_W, FLOP_H);
    // Eject notch (small inset rectangle on the right end).
    renderer.drawRect(flopX + FLOP_W - sc(10), flopY + sc(2), sc(6), FLOP_H - sc(4));

    // ── Speaker grille (left side of bottom section: 3 columns × 6 rows) ──
    const int spkX = caseX + sc(14);
    const int spkY = botY + sc(12);
    for (int row = 0; row < 6; ++row)
      for (int col = 0; col < 3; ++col)
        renderer.fillRect(spkX + col * sc(6), spkY + row * sc(7), sc(3), sc(4));

    // ── "Macintosh" wordmark (centred below floppy slot) ───────────────────
    const int logoY = flopY + FLOP_H + sc(6);
    const int logoX = caseX + (CASE_W - renderer.getTextWidth(SMALL_FONT_ID, "Macintosh")) / 2;
    renderer.drawText(SMALL_FONT_ID, logoX, logoY, "Macintosh");

    // ── Power indicator dot (bottom-right corner of case, inside border) ────
    renderer.fillRect(caseX + CASE_W - sc(12), caseY + CASE_H - sc(12), sc(5), sc(5));

    // ── Wear and texture ────────────────────────────────────────────────────
    // Screen recess: 1-px shadow on the left and top inner edges of the bezel
    // opening, suggesting the screen glass is set back into the plastic.
    renderer.drawLine(scrX - 1, scrY,     scrX - 1, scrY + SCR_H - 1, true);  // left
    renderer.drawLine(scrX,     scrY - 1, scrX + SCR_W - 1, scrY - 1, true);  // top

    // Scuff marks: short horizontal strokes on the bottom bezel body.
    // Placed clear of the speaker grille (left edge) and floppy/wordmark (centre).
    // Each mark is 2 px tall — second row shorter for a natural taper.
    const struct { int x, y, w; } kScuffs[] = {
        { sc(19),  sv(28), sc(9)  },   // left, above grille
        { sc(23),  sv(58), sc(5)  },   // left, mid
        { sc(240), sv(20), sc(10) },   // right, upper
        { sc(244), sv(52), sc(6)  },   // right, mid
        { sc(195), sv(82), sc(7)  },   // lower centre-right
        { sc(88),  sv(88), sc(5)  },   // lower centre-left
    };
    for (const auto& s : kScuffs) {
        renderer.fillRect(caseX + s.x,     botY + s.y,     s.w,     1, true);
        renderer.fillRect(caseX + s.x + 1, botY + s.y + 1, s.w - 2, 1, true);
    }
  }

  // ── Step 5: refresh ───────────────────────────────────────────────────────
  if (hasCustomFrame) {
    // Custom frame: the image already carries its own aesthetic; plain BW refresh.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    // Built-in frame: 4-level grayscale tints the case body from pure white to
    // light grey (warm beige translation on the X4 Pro panel).
    // Must stay HALF: the grey-nudge LUT is calibrated against HALF waveform.
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

    // LSB plane — leave at 0x00 (no dark-grey regions in the case body).
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.copyGrayscaleLsbBuffers();

    // MSB plane — mark each bezel region so it resolves to the light-grey step.
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.fillRect(caseX,        caseY, CASE_W, BEZ_TOP, false);  // top bezel
    renderer.fillRect(caseX,        scrY,  BEZ_LR, SCR_H,   false);  // left bezel
    renderer.fillRect(scrX + SCR_W, scrY,  BEZ_LR, SCR_H,   false);  // right bezel
    renderer.fillRect(caseX,        botY,  CASE_W, botH,    false);   // bottom section
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}
