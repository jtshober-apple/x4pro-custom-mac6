#pragma once

#include <GfxRenderer.h>

#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Shown immediately after a rename. Connects to OpenLibrary, fetches title,
// author and cover, writes a sidecar override and converts + saves the cover
// BMP so the next Epub::load() rebuilds book.bin with the correct metadata.
//
// Modelled on ClockSyncActivity: requestUpdateAndWait() forces the "Looking
// up…" screen before the blocking network work begins.
class MetadataLookupActivity : public Activity {
 public:
  MetadataLookupActivity(GfxRenderer& renderer, MappedInputManager& input,
                         const std::string& epubPath, const std::string& titleHint);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { LOOKING_UP, SUCCESS, NOT_FOUND, NO_WIFI, FAILED };

  State state_ = LOOKING_UP;
  std::string epubPath_;
  std::string titleHint_;
  char foundTitle_[256] = {};
  char foundAuthor_[128] = {};
  unsigned long doneAtMs_ = 0;

  void doWork();
};
