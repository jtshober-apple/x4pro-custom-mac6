#pragma once

#include "components/themes/BaseTheme.h"

namespace System6Metrics {
constexpr int titleBarHeight = 48;
constexpr ThemeMetrics makeValues() {
  auto v = BaseMetrics::values;
  v.batteryBarHeight = 0;
  v.headerHeight = 58;
  v.homeTopPadding = 58;
  v.homeCoverHeight = 180;
  v.homeCoverTileHeight = 210;
  v.menuRowHeight = 56;
  v.menuSpacing = 8;
  v.listInset = 12;
  v.listSidePadding = 12;
  v.listScrollWidth = 8;
  v.listRowRadius = 0;
  v.listSelectionStyle = 0;
  v.popupFrameThickness = 3;
  v.popupCornerRadius = 0;
  v.keyboardKeySpacing = 4;
  v.keyboardCenteredText = true;
  return v;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace System6Metrics

// No state, image assets, or extra framebuffer. Decoration is drawn in place.
class System6Theme final : public BaseTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawHeaderWithRightReserve(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                                  int rightReserve) const;
  int getMenuFirstRowY(Rect rect, int buttonCount) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int)>& buttonLabel,
                      const std::function<UIIcon(int)>& rowIcon) const override;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, int progress) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
};
static_assert(sizeof(System6Theme) == sizeof(BaseTheme), "Theme must not add resident state");