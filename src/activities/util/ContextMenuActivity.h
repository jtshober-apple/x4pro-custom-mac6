#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <functional>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// A lightweight context-menu activity that shows a short labeled list.
// onChoice is called with the 0-based index of the selected option before
// finish() is called.  onChoice(-1) means the user cancelled (Back button).
//
// Usage:
//   startActivityForResult(
//     std::make_unique<ContextMenuActivity>(renderer, mappedInput, "Action",
//       std::vector<std::string>{"Edit Name", "Delete"},
//       [this](int choice) {
//         if (choice == 0) launchRename();
//         else if (choice == 1) launchDelete();
//       }),
//     [](const ActivityResult&) {});   // result handled by onChoice callback
class ContextMenuActivity : public Activity {
 public:
  ContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading,
                      std::vector<std::string> options, std::function<void(int)> onChoice)
      : Activity("ContextMenu", renderer, mappedInput),
        heading(std::move(heading)),
        options(std::move(options)),
        onChoice(std::move(onChoice)) {}

  void loop() override {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int count = static_cast<int>(options.size());

    // Touch: row tap selects
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, rowsTop, rowHeight + metrics.menuSpacing, count, 0, INT32_MAX, rowHeight);
    if (touch == MappedInputManager::RowTouch::Tap) {
      if (row >= 0 && row < count) {
        onChoice(row);
        ActivityResult res;
        res.isCancelled = false;
        setResult(std::move(res));
        finish();
        return;
      }
    }

    // Physical Confirm button
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onChoice(selected);
      ActivityResult res;
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }
    // Back = cancel
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onChoice(-1);
      ActivityResult res;
      res.isCancelled = true;
      setResult(std::move(res));
      finish();
      return;
    }
    // Navigate with Up/Down buttons
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selected = (selected + 1) % count;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selected = (selected + count - 1) % count;
      requestUpdate();
    }
  }

  void render(RenderLock&&) override {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int pageW = renderer.getScreenWidth();
    const int font = uiScaleSpec().bodyFontId;
    const int smallFont = uiScaleSpec().smallFontId;
    rowHeight = renderer.getLineHeight(font) + 16;

    renderer.clearScreen();

    // Heading band
    const int headingH = renderer.getLineHeight(smallFont) + 12;
    renderer.drawText(smallFont, metrics.contentSidePadding, (headingH - renderer.getLineHeight(smallFont)) / 2,
                      heading.c_str());
    renderer.drawLine(0, headingH, pageW, headingH);

    rowsTop = headingH + metrics.menuSpacing;

    for (int i = 0; i < static_cast<int>(options.size()); i++) {
      const int y = rowsTop + i * (rowHeight + metrics.menuSpacing);
      const bool sel = (i == selected);
      if (sel) {
        renderer.fillRect(0, y, pageW, rowHeight, true);
        renderer.drawText(font, metrics.contentSidePadding,
                          y + (rowHeight - renderer.getLineHeight(font)) / 2, options[i].c_str(), false);
      } else {
        renderer.drawRect(0, y, pageW, rowHeight);
        renderer.drawText(font, metrics.contentSidePadding,
                          y + (rowHeight - renderer.getLineHeight(font)) / 2, options[i].c_str(), true);
      }
    }

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

 private:
  std::string heading;
  std::vector<std::string> options;
  std::function<void(int)> onChoice;
  int selected = 0;
  int rowsTop = 0;
  int rowHeight = 48;
};