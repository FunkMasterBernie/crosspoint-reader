#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session. The live frontlight
// state rides along in the same flag so the reboot is invisible: the light
// comes back exactly as it was, regardless of the Restore Light on Wake
// preference.

void silentRestart();            // home screen
void silentRestartToReader();    // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings();  // settings screen

// Reboots immediately after an activity releases exclusive raw storage. The
// RTC target ensures setup() lands on Home instead of resuming a reader.
void restartToHomeAfterStorageHandoff();

// Marks the next silent reboot as the tail end of an automatic sync, so the book
// it resumes into does not immediately sync again. Rides the same RTC_NOINIT word
// as the frontlight state: it survives ESP.restart() and not a power cycle, which
// is exactly the lifetime wanted — after a cold boot, syncing on open is correct.
void armSkipSyncOnNextReboot();

// Reads and clears that flag. True only for the first book opened after a sync's
// own reboot.
bool consumeSkipSyncOnOpen();
