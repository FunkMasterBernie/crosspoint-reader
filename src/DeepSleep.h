#pragma once

// Finishes a sleep that an activity deferred. main.cpp's enterDeepSleep() can
// hand control to an activity instead of sleeping (see
// KOReaderSyncActivity::ReturnTo::Sleep); that activity calls this when it is
// done, and the sleep proceeds without being intercepted a second time.
// Does not return.
void requestDeepSleep();
