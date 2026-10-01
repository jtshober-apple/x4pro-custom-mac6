#include "System6Theme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "RecentBooksStore.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Bounded UTF-8 truncation: a small stack buffer, no temporary strings.
int fitLabel(const GfxRenderer& r, int font, const char* text, int width, char (&out)[160]) {
  size_t n = 0;
  if (!text || width <= 0) {
    out[0] = '\0';
    return 0;
  }
  while (n < sizeof(out) - 1 && text[n]) {
    out[n] = text[n];
    ++n;
  }
  if (text[n])
    while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
  out[n] = '\0';
  int measured = r.getTextWidth(font, out);
  if (measured <= width) return measured;

  // Doesn't fit: shrink and append an ellipsis so a cut label reads as
  // truncated rather than as a different, complete word.
  constexpr char kEllipsis[] = "...";
  constexpr size_t kEllipsisLen = sizeof(kEllipsis) - 1;
  while (true) {
    if (n == 0) {
      std::memcpy(out, kEllipsis, kEllipsisLen + 1);
      measured = r.getTextWidth(font, out);
      if (measured > width) {
        out[0] = '\0';
        measured = 0;
      }
      return measured;
    }
    --n;
    while (n && (static_cast<unsigned char>(out[n]) & 0xc0) == 0x80) --n;
    if (n + kEllipsisLen < sizeof(out) - 1) {
      std::memcpy(out + n, kEllipsis, kEllipsisLen + 1);
      measured = r.getTextWidth(font, out);
      if (measured <= width) return measured;
    } else {
      out[n] = '\0';
    }
  }
}
void label(const GfxRenderer& r, int font, int x, int y, int width, const char* text, bool black = true) {
  char clipped[160];
  fitLabel(r, font, text, width, clipped);
  r.drawText(font, x, y, clipped, black);
}
void frame(const GfxRenderer& r, Rect b) {
  if (b.width < 8 || b.height < 8) return;
  r.fillRect(b.x + 3, b.y + 3, b.width, b.height);
  r.fillRect(b.x, b.y, b.width, b.height, false);
  r.drawRect(b.x, b.y, b.width, b.height);
  r.drawRect(b.x + 2, b.y + 2, b.width - 4, b.height - 4);
}
void desktop(const GfxRenderer& r, int top, int bottom) {
  // One-bit checkerboard, drawn into the existing buffer. No background bitmap.
  for (int y = top; y < bottom; y += 2) {
    for (int x = ((y / 2) & 1) * 2; x < r.getScreenWidth(); x += 4) r.fillRect(x, y, 2, std::min(2, bottom - y));
  }
}
void macIcon(const GfxRenderer& r, int x, int y, int scale = 1, bool black = true) {
  r.drawRect(x, y, 24 * scale, 29 * scale, black);
  r.drawRect(x + 3 * scale, y + 3 * scale, 18 * scale, 17 * scale, black);
  r.fillRect(x + 7 * scale, y + 8 * scale, 2 * scale, 2 * scale, black);
  r.fillRect(x + 15 * scale, y + 8 * scale, 2 * scale, 2 * scale, black);
  r.drawLine(x + 8 * scale, y + 14 * scale, x + 15 * scale, y + 14 * scale, black);
  r.drawLine(x + 10 * scale, y + 24 * scale, x + 20 * scale, y + 24 * scale, black);
  r.drawRect(x + 2 * scale, y + 30 * scale, 20 * scale, 3 * scale, black);
}
void documentIcon(const GfxRenderer& r, int x, int y, bool black) {
  r.drawRect(x, y, 22, 26, black);
  r.drawLine(x + 15, y, x + 15, y + 7, black);
  r.drawLine(x + 15, y + 7, x + 21, y + 7, black);
  for (int line = 12; line <= 20; line += 4) r.drawLine(x + 5, y + line, x + 16, y + line, black);
}
void menuIcon(const GfxRenderer& r, UIIcon icon, int x, int y, bool black) {
  switch (icon) {
    case Folder:
    case Library:
      r.drawRect(x, y + 6, 26, 19, black);
      r.drawRect(x + 2, y + 2, 11, 5, black);
      break;
    case Settings:
      r.drawRect(x, y + 1, 26, 24, black);
      for (int i = 0; i < 3; ++i) {
        r.drawLine(x + 4, y + 6 + i * 7, x + 22, y + 6 + i * 7, black);
        r.fillRect(x + 7 + (i % 2) * 9, y + 4 + i * 7, 3, 5, black);
      }
      break;
    case Transfer:
    case Wifi:
    case Hotspot:
      r.drawRect(x, y + 1, 26, 18, black);
      r.drawLine(x + 13, y + 19, x + 13, y + 24, black);
      r.drawLine(x + 6, y + 25, x + 20, y + 25, black);
      break;
    case Recent:
      r.drawRect(x + 3, y + 2, 22, 22, black);
      r.drawLine(x + 14, y + 6, x + 14, y + 14, black);
      r.drawLine(x + 14, y + 14, x + 20, y + 14, black);
      break;
    case Book:
      r.drawRect(x + 3, y + 1, 22, 25, black);
      r.drawLine(x + 8, y + 2, x + 8, y + 25, black);
      r.drawLine(x + 11, y + 7, x + 21, y + 7, black);
      break;
    default:
      documentIcon(r, x + 2, y, black);
      break;
  }
}
}  // namespace

void System6Theme::drawHeader(const GfxRenderer& r, Rect rect, const char* title, const char* subtitle) const {
  drawHeaderWithRightReserve(r, rect, title, subtitle, 0);
}

void System6Theme::drawHeaderWithRightReserve(const GfxRenderer& r, Rect rect, const char* title, const char* subtitle,
                                              int rightReserve) const {
  const bool home = title == nullptr;
  int top, right, bottom, left;
  r.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int x = std::max(rect.x + 8, left + 4);
  const int end = std::min(rect.x + rect.width - 8, r.getScreenWidth() - right - 4);
  const int h = System6Metrics::titleBarHeight;
  const int y = std::max(rect.y, top);
  if (end - x < 80 || y < top) return;
  const auto& m = UITheme::getInstance().getMetrics();
  const int contentBottom = r.getScreenHeight() - std::max(bottom, m.buttonHintsHeight) - 4;
  desktop(r, 0, r.getScreenHeight());
  if (!home) {
    frame(r, Rect{x, y, end - x, std::max(h, contentBottom - y)});
    // Window grow box, as on the original monochrome desktop.
    for (int d = 5; d < 18; d += 4) r.drawLine(end - d - 3, contentBottom - 5, end - 4, contentBottom - d - 4);
  }
  frame(r, Rect{x, y, end - x, h});
  for (int dy = 7; dy < h - 5; dy += 4) r.drawLine(x + 6, y + dy, end - 7, y + dy);

  const int statusEnd = std::min(end, rect.x + rect.width - std::max(0, rightReserve));
  // Smaller font keeps the menu bar uncluttered, matching the original Mac feel.
  constexpr int statusFont = SMALL_FONT_ID;
  const int statusTextY = y + (h - r.getLineHeight(statusFont)) / 2;

  // Battery: bare number only (no icon, no % sign). A "+" prefix when charging
  // is the clearest cue on 1-bit e-ink where bold weight is not distinguishable.
  // Slot is always sized for "+100" so the layout never shifts between states.
  const bool showPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const bool charging = gpio.isUsbConnected();
  char percentageText[6] = {};
  if (charging) {
    std::snprintf(percentageText, sizeof(percentageText), "+%u", static_cast<unsigned>(percentage));
  } else {
    std::snprintf(percentageText, sizeof(percentageText), "%u", static_cast<unsigned>(percentage));
  }
  // Fixed slot mirrors the Mac icon's 32-px cleared area so the title is
  // always centred in the bar regardless of digit count or charging state.
  constexpr int batterySlotWidth = 32;
  const int batterySlotX = statusEnd - 10 - batterySlotWidth;
  int statusLeft = showPercentage ? batterySlotX : statusEnd;

  // Clock is not in the menu bar — it lives as a desk accessory below the title
  // bar on the home screen, keeping the bar clean: [mac][stripes]title[stripes][%].
  char timeText[9] = {};
  const bool showClock =
      halClock.isAvailable() &&
      halClock.formatTime(timeText, sizeof(timeText), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1);

  int subtitleX = statusLeft;
  char clippedSubtitle[160] = {};
  if (!home && subtitle && subtitle[0]) {
    const int subtitleWidth = fitLabel(r, statusFont, subtitle, std::max(1, (end - x) / 4), clippedSubtitle);
    subtitleX = statusLeft - 10 - subtitleWidth;
    statusLeft = subtitleX;
  }

  r.fillRect(statusLeft - 6, y + 3, statusEnd - statusLeft + 6, h - 6, false);
  if (!home && subtitle && subtitle[0]) r.drawText(statusFont, subtitleX, statusTextY, clippedSubtitle);
  if (showPercentage) {
    const int actualWidth = r.getTextWidth(statusFont, percentageText);
    const int centeredX = batterySlotX + (batterySlotWidth - actualWidth) / 2;
    r.drawText(statusFont, centeredX, statusTextY, percentageText);
  }

  const int font = uiScaleSpec().bodyFontId;
  const char* text = title && title[0] ? title : tr(STR_THEME_SYSTEM6);
  char clipped[160];
  const int titleLeft = x + (home ? 42 : 32);
  const int titleRight = std::max(titleLeft + 1, statusLeft - 10);
  const int width = fitLabel(r, font, text, titleRight - titleLeft, clipped);
  const int centeredX = x + (end - x - width) / 2;
  const int tx = std::clamp(centeredX, titleLeft, std::max(titleLeft, titleRight - width));
  r.fillRect(tx - 7, y + 3, width + 14, h - 6, false);
  r.drawText(font, tx, y + (h - r.getLineHeight(font)) / 2, clipped);
  if (home) {
    r.fillRect(x + 6, y + 2, 32, h - 4, false);
    macIcon(r, x + 9, y + 3);
  } else {
    // TouchHeaderBackButton uses this window box as its Back affordance.
    r.fillRect(x + 9, y + 13, 14, 14, false);
    r.drawRect(x + 9, y + 13, 14, 14);
  }

}

void System6Theme::drawButtonHints(GfxRenderer& r, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  if (gpio.hasTouch()) return;

  const auto orientation = r.getOrientation();
  r.setOrientation(GfxRenderer::Orientation::Portrait);
  const int pageHeight = r.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonBandHeight = System6Metrics::values.buttonHintsHeight;
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = r.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  int topMargin, rightMargin, bottomMargin, leftMargin;
  r.getOrientedViewableTRBL(&topMargin, &rightMargin, &bottomMargin, &leftMargin);
  (void)topMargin;
  (void)rightMargin;
  (void)leftMargin;
  const int buttonTop = pageHeight - buttonBandHeight;
  constexpr int keyTopInset = 5;
  constexpr int keyBottomInset = 5;
  constexpr int keyShadowOffset = 3;
  const int keyTop = buttonTop + keyTopInset;
  const int buttonHeight = std::max(1, buttonBandHeight - bottomMargin - keyTopInset - keyBottomInset);

  // The header paints the same checker over the whole screen. Clearing this
  // band first also makes screens without a header use the identical pattern.
  r.fillRect(0, buttonTop, r.getScreenWidth(), buttonBandHeight, false);
  desktop(r, buttonTop, pageHeight);
  for (int i = 0; i < 4; ++i) {
    const int x = buttonPositions[i];
    // Touch targets for on-screen buttons are registered by the activity layer.
    r.fillRect(x + keyShadowOffset, keyTop + keyShadowOffset, buttonWidth, buttonHeight);
    r.fillRect(x, keyTop, buttonWidth, buttonHeight, false);
    r.drawRect(x, keyTop, buttonWidth, buttonHeight);
    r.drawLine(x + 2, keyTop + 2, x + buttonWidth - 3, keyTop + 2);
  }

  r.setOrientation(GfxRenderer::Orientation::Portrait);
  for (int i = 0; i < 4; ++i) {
    if (!labels[i] || !labels[i][0]) continue;
    const int x = buttonPositions[i];
    constexpr int preferredPadding = 8;
    constexpr int minPadding = 2;
    constexpr int font = UI_10_FONT_ID;
    char clipped[160];
    int textWidth = fitLabel(r, font, labels[i], buttonWidth - preferredPadding * 2, clipped);
    if (strcmp(clipped, labels[i]) != 0) {
      char tighter[160];
      const int tighterWidth = fitLabel(r, font, labels[i], buttonWidth - minPadding * 2, tighter);
      if (strcmp(tighter, labels[i]) == 0) {
        textWidth = tighterWidth;
        std::memcpy(clipped, tighter, sizeof(clipped));
      }
    }
    const int textOffset = std::max(0, (buttonHeight - r.getLineHeight(font)) / 2);
    const int textY = keyTop + textOffset;
    r.drawText(font, x + (buttonWidth - textWidth) / 2, textY, clipped);
  }
  r.setOrientation(orientation);
}

Rect System6Theme::drawPopup(const GfxRenderer& r, const char* message) const {
  constexpr int titleHeight = 31;
  const int maxWidth = r.getScreenWidth() - 48;
  const int messageWidth = r.getTextWidth(UI_10_FONT_ID, message ? message : "");
  const int width = std::clamp(messageWidth + 72, 230, maxWidth);
  constexpr int height = 112;
  const int x = (r.getScreenWidth() - width) / 2;
  const int y = std::max(18, r.getScreenHeight() / 12);
  frame(r, Rect{x, y, width, height});
  for (int stripeY = y + 8; stripeY < y + titleHeight - 5; stripeY += 4) {
    r.drawLine(x + 7, stripeY, x + width - 8, stripeY);
  }
  const char* title = tr(STR_THEME_SYSTEM6);
  const int titleWidth = r.getTextWidth(UI_10_FONT_ID, title);
  const int titleX = x + (width - titleWidth) / 2;
  r.fillRect(titleX - 7, y + 4, titleWidth + 14, titleHeight - 6, false);
  r.drawText(UI_10_FONT_ID, titleX, y + (titleHeight - r.getLineHeight(UI_10_FONT_ID)) / 2, title);
  r.drawLine(x + 3, y + titleHeight, x + width - 4, y + titleHeight);
  documentIcon(r, x + 15, y + 48, true);
  char clipped[160];
  fitLabel(r, UI_10_FONT_ID, message, width - 66, clipped);
  r.drawText(UI_10_FONT_ID, x + 50, y + 50, clipped);
  r.displayBuffer();
  return Rect{x, y, width, height};
}

void System6Theme::fillPopupProgress(const GfxRenderer& r, const Rect& layout, const int progress) const {
  const int barX = layout.x + 50;
  const int barY = layout.y + layout.height - 25;
  const int barWidth = layout.width - 65;
  constexpr int barHeight = 10;
  const int fillWidth = std::max(0, (barWidth - 4) * std::clamp(progress, 0, 100) / 100);
  r.fillRect(barX, barY, barWidth, barHeight, false);
  r.drawRect(barX, barY, barWidth, barHeight);
  for (int x = barX + 2; x < barX + 2 + fillWidth; x += 4) {
    r.fillRect(x, barY + 2, std::min(2, barX + 2 + fillWidth - x), barHeight - 4);
  }
  r.displayBuffer(HalDisplay::FAST_REFRESH);
}

int System6Theme::getMenuFirstRowY(Rect rect, int buttonCount) const {
  const auto& m = System6Metrics::values;
  const int step = m.menuRowHeight + m.menuSpacing;
  const int rows = std::min(7, (rect.height - 44) / step);
  if (rows <= 0) return rect.y + 37;
  const int visible = std::min(rows, std::max(1, buttonCount));
  const int menuBoxH = visible * step + 40;
  const int menuY = rect.y + std::max(2, (rect.height - menuBoxH) / 2);
  return menuY + 37;
}

void System6Theme::drawButtonMenu(GfxRenderer& r, Rect rect, int count, int selected,
                                  const std::function<std::string(int)>& buttonLabel,
                                  const std::function<UIIcon(int)>& rowIcon) const {
  const auto& m = System6Metrics::values;
  const int step = m.menuRowHeight + m.menuSpacing;
  const int rows = std::min(7, (rect.height - 44) / step);
  if (count <= 0 || rows <= 0 || rect.width < 80) return;
  const int start = std::clamp(selected, 0, count - 1) / rows * rows;
  const int visible = std::min(rows, count - start);
  const int x = rect.x + 14;
  const int width = rect.width - 32;
  const int menuBoxH = visible * step + 40;
  const int menuY = rect.y + std::max(2, (rect.height - menuBoxH) / 2);
  frame(r, Rect{x, menuY, width, menuBoxH});
  for (int dy = 6; dy < 26; dy += 4) r.drawLine(x + 6, menuY + dy, x + width - 7, menuY + dy);
  const int titleWidth = r.getTextWidth(UI_10_FONT_ID, tr(STR_MENU));
  const int titleX = x + (width - titleWidth) / 2;
  r.fillRect(titleX - 8, menuY + 2, titleWidth + 16, 26, false);
  r.drawText(UI_10_FONT_ID, titleX, menuY + 4, tr(STR_MENU));
  r.drawLine(x + 3, menuY + 30, x + width - 4, menuY + 30);
  for (int n = 0; n < visible; ++n) {
    const int i = start + n;
    const int y = menuY + 37 + n * step;
    const bool active = i == selected;
    if (active) r.fillRect(x + 5, y, width - 10, m.menuRowHeight);
    menuIcon(r, rowIcon ? rowIcon(i) : File, x + 12, y + 9, !active);
    const int font = uiScaleSpec().bodyFontId;
    const std::string rowLabel = buttonLabel ? buttonLabel(i) : std::string{};
    label(r, font, x + 49, y + (m.menuRowHeight - r.getLineHeight(font)) / 2, width - 72,
          rowLabel.c_str(), !active);
    // Touch targets for menu rows are registered by the activity layer.
  }
  if (start > 0) {
    r.drawLine(x + width - 18, menuY + 44, x + width - 14, menuY + 40);
    r.drawLine(x + width - 14, menuY + 40, x + width - 10, menuY + 44);
  }
  if (start + visible < count) {
    const int y = menuY + 30 + visible * step;
    r.drawLine(x + width - 18, y - 4, x + width - 14, y);
    r.drawLine(x + width - 14, y, x + width - 10, y - 4);
  }
}

void System6Theme::drawRecentBookCover(GfxRenderer& r, Rect rect, const std::vector<RecentBook>& books, int selected,
                                       bool& rendered, bool& stored, bool& restored,
                                       std::function<bool()> store) const {
  (void)store;
  // The cover is streamed a row at a time through a 2x2 monochrome dither. No
  // image-sized buffer or Home cover snapshot is retained.
  rendered = false;
  stored = false;
  restored = false;
  if (rect.height < 70 || rect.width < 100) return;
  const Rect tile{rect.x + 24, rect.y + 14, rect.width - 52, rect.height - 30};
  frame(r, tile);
  const bool active = !books.empty() && selected == 0;
  if (active) r.fillRect(tile.x + 5, tile.y + 5, tile.width - 10, tile.height - 10);
  bool coverDrawn = false;
  int textX = tile.x + 82;
  if (!books.empty() && !books.front().coverBmpPath.empty()) {
    const std::string coverPath =
        UITheme::getCoverThumbPath(books.front().coverBmpPath, System6Metrics::values.homeCoverHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const int maxCoverWidth = std::min(104, tile.width / 3);
        const int maxCoverHeight = std::max(1, tile.height - 28);
        int coverHeight = maxCoverHeight;
        int coverWidth = bitmap.getWidth() * coverHeight / bitmap.getHeight();
        if (coverWidth > maxCoverWidth) {
          coverWidth = maxCoverWidth;
          coverHeight = bitmap.getHeight() * coverWidth / bitmap.getWidth();
        }
        coverWidth = std::max(1, coverWidth);
        coverHeight = std::max(1, coverHeight);
        const int coverX = tile.x + 18 + (maxCoverWidth - coverWidth) / 2;
        const int coverY = tile.y + (tile.height - coverHeight) / 2;
        r.fillRect(coverX - 3, coverY - 3, coverWidth + 6, coverHeight + 6, false);
        r.drawBitmap(bitmap, coverX, coverY, coverWidth, coverHeight);
        r.drawRect(coverX - 2, coverY - 2, coverWidth + 4, coverHeight + 4);
        coverDrawn = true;
        textX = tile.x + 18 + maxCoverWidth + 18;
      }
    }
  }
  if (!coverDrawn) documentIcon(r, tile.x + 28, tile.y + (tile.height - 26) / 2, !active);
  const int font = uiScaleSpec().bodyFontId;
  const int textWidth = tile.x + tile.width - textX - 18;
  const int lineHeight = r.getLineHeight(font);
  const int titleY = tile.y + 10;
  const int maxTitleLines = std::max(1, (tile.height - 20) / std::max(1, lineHeight));
  const auto titleLines = r.wrappedText(font, books.empty() ? tr(STR_NO_RECENT_BOOKS) : books.front().title.c_str(),
                                        textWidth, maxTitleLines, EpdFontFamily::BOLD);
  for (size_t i = 0; i < titleLines.size(); ++i) {
    r.drawText(font, textX, titleY + static_cast<int>(i) * lineHeight, titleLines[i].c_str(), !active,
               EpdFontFamily::BOLD);
  }
  // Touch target for the book cover is registered by the activity layer.
}

void System6Theme::drawHomeGap(GfxRenderer& r, Rect gapRect) const {
  // Draw a Mac-style clock desk accessory centered in the gray desktop gap
  // between the book cover tile and the menu box.
  if (gapRect.height < 24 || gapRect.width < 60) return;

  char timeText[9] = {};
  const bool showClock =
      halClock.isAvailable() &&
      halClock.formatTime(timeText, sizeof(timeText), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1);
  if (!showClock) return;

  constexpr int statusFont = UI_10_FONT_ID;
  constexpr int kPadX = 12;
  constexpr int kPadY = 6;
  const int clockW = r.getTextWidth(statusFont, timeText) + kPadX * 2;
  const int clockH = r.getLineHeight(statusFont) + kPadY * 2;

  // Center the widget in the gap both horizontally and vertically.
  const int clockBoxX = gapRect.x + (gapRect.width - clockW) / 2;
  const int clockBoxY = gapRect.y + (gapRect.height - clockH) / 2;

  // Mac double-frame with drop shadow (same style as menu and book tile).
  frame(r, Rect{clockBoxX, clockBoxY, clockW, clockH});
  r.drawText(statusFont, clockBoxX + kPadX, clockBoxY + kPadY, timeText);
}