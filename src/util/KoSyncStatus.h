#pragma once

class GfxRenderer;

/**
 * "Last KOReader-sync attempt did not finish successfully" indicator.
 *
 * Shown as a small boxed X (reader status bar, screen headers, sleep screen)
 * ONLY while unsynced. No X = the last automatic/manual sync succeeded, so it's
 * safe to pick the book up on another device.
 *
 * The state is a marker file on the SD card, cached in RAM, so it survives
 * sleep and reboots. An attempt is recorded as unsynced up front and only
 * cleared once it succeeds -- so a failure, a crash/watchdog reset, or power
 * loss mid-sync all leave the X up.
 */
namespace KoSyncStatus {

// True while the last recorded sync attempt has not succeeded.
bool isUnsynced();

// Call right before a sync attempt begins.
void markAttemptStarted();

// Call when a sync completes successfully (or when there is nothing to sync).
void markSynced();

// Draws the boxed X with its top-left corner at (x, y), size x size pixels.
// Only draws in normal black-and-white render mode; it is a no-op during the
// reader's grayscale passes (the black-and-white pass already carries it).
// whiteBackground fills the box white first, for use over images.
void drawMark(const GfxRenderer& renderer, int x, int y, int size, bool whiteBackground = false);

// For screens drawn in three passes (black-and-white base + two grayscale
// passes, e.g. cover/custom sleep images): call right after drawing the image
// in EACH grayscale pass so the box region is left alone (pure black/white)
// instead of being toned gray by the image underneath. No-op in BW mode.
void clearGrayscaleRegion(const GfxRenderer& renderer, int x, int y, int size);

}  // namespace KoSyncStatus
