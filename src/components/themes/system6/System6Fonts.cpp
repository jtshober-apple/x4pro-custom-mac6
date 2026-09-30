#include "System6Fonts.h"

#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "System6FontData.h"
#include "fontIds.h"

namespace {
constexpr int smallId = 0x536601;
constexpr int bodyId = 0x536602;
constexpr int largeId = 0x536603;
const EpdFont smallFont(&system6_small);
const EpdFont bodyFont(&system6_body);
const EpdFont largeFont(&system6_large);
}  // namespace

void registerSystem6Fonts(GfxRenderer& renderer) {
  // Register under private IDs for any direct-ID usage.
  renderer.insertFont(smallId, EpdFontFamily(&smallFont));
  renderer.insertFont(bodyId, EpdFontFamily(&bodyFont));
  renderer.insertFont(largeId, EpdFontFamily(&largeFont));

  // Override the standard UI font slots so every draw call that uses
  // SMALL_FONT_ID / UI_10_FONT_ID / UI_12_FONT_ID automatically uses
  // Chicago instead of Ubuntu/Noto Sans.  Called after the upstream
  // registrations in setupDisplayAndFonts(), so these win.
  renderer.insertFont(SMALL_FONT_ID, EpdFontFamily(&smallFont));
  renderer.insertFont(UI_10_FONT_ID, EpdFontFamily(&bodyFont));
  renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(&largeFont));
}
