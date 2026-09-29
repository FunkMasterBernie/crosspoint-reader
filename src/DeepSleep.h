#pragma once

// Finishes a sleep that an activity deferred. main.cpp's enterDeepSleep() can
// hand control to an activity instead of sleeping (see
// KOReaderSyncActivity::ReturnTo::Sleep); that activity calls this when it is
// done. It only releases the interrupted sleep, which then finishes on the
// stack frame that started it -- so this returns normally, and the caller
// should stop touching the device afterwards.
void requestDeepSleep();
