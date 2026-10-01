#pragma once

#include <GfxRenderer.h>

#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Shown immediately after a rename. Connects to OpenLibrary, fetches title,
// author and cover, writes a sidecar override and converts + saves the cover
// BMP so the next Epub::load() rebuilds book.bin with the correct metadata.
//
// If WiFi is not already connected it launches WifiSelectionActivity first,
// then tears down WiFi on exit — matching the ClockSyncActivity pattern.
class MetadataLookupActivity : public Activity {
 public:
  MetadataLookupActivity(GfxRenderer& renderer, MappedInputManager& input,
                         const std::string& epubPath, const std::string& titleHint);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { WAITING_FOR_WIFI, LOOKING_UP, SUCCESS, NOT_FOUND, NO_WIFI, FAILED };

  State state_ = WAITING_FOR_WIFI;
  std::string epubPath_;
  std::string titleHint_;
  char foundTitle_[256] = {};
  char foundAuthor_[128] = {};
  unsigned long doneAtMs_ = 0;
  bool shouldTearDownWifiOnExit_ = false;

  void doWork();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
};
