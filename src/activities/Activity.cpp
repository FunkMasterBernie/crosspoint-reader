#include "Activity.h"

#include "ActivityManager.h"
#include "reader/KOReaderSyncActivity.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) {
  // The one place a user opens a book: Home's recent list, the Library and the file
  // browser all arrive here, and nothing internal does -- the sync activity's own
  // return, the boot resume and the bookmark/chapter re-entries all call
  // goToReader() directly, so they cannot re-trigger the pull below.
  if (KOReaderSyncActivity::pullBeforeOpen(renderer, mappedInput, path)) return;
  activityManager.goToReader(path);
}

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
