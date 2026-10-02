#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity, private UiAppHost {
 public:
  /**
   * Where the device lands once the sync is done.
   *
   * Reader is the manual case: the user asked for the sync mid-book and expects
   * the page back, so the activity draws its progress and its result.
   *
   * Sleep is the automatic case, and it is the only moment a sync is free.
   * Every other WiFi session ends in silentRestart() to clear the fragmentation
   * the radio leaves behind; sleep is already a full chip reset on wake, so
   * main.cpp's deepSleepInProgress latch turns that reboot into a no-op. The
   * device was leaving anyway. This mode draws nothing at all -- it leaves the
   * page on the panel and sleeps over the top of it.
   */
  enum class ReturnTo : uint8_t { Reader, Sleep };

  // Manual sync, started from a live reader that has already mapped its position.
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                CrossPointPosition localPosition, SavedProgressPosition localKoPos,
                                std::string localChapterName);

  // Automatic sync with no reader to ask: the position is read back from the book's
  // own progress.bin and mapped here. ReturnTo::Sleep is the on-sleep push;
  // ReturnTo::Reader is the on-open pull, which opens the book once the server has
  // been consulted.
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                ReturnTo returnTo);

  /**
   * Consults the sync server before `epubPath` is opened, so a book continued on
   * another device opens where that device left it rather than jumping forward at
   * the end of the session.
   *
   * Returns true when it has taken over the open; the caller must not also open the
   * book. False means nothing was owed or possible and the caller should proceed.
   */
  static bool pullBeforeOpen(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

  // Headless runs leave the reader's page on the panel: no status screen, no
  // result screen, nothing for the sleep image to paint over. Suppressing the
  // requests is enough -- ActivityManager only renders when one arrives.
  void requestUpdate(bool immediate = false) override {
    if (!headless()) Activity::requestUpdate(immediate);
  }
  void requestUpdateAndWait() override {
    if (!headless()) Activity::requestUpdateAndWait();
  }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  CrossPointPosition localPosition;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;
  // Long enough to read an error on the way past it.
  static constexpr unsigned long AUTO_RETURN_ERROR_DELAY_MS = 3000;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  ReturnTo returnTo = ReturnTo::Reader;

  // True when nothing asked for this sync, so nothing is waiting on a keypress to
  // dismiss it: failures time out to the destination instead of parking an error
  // screen in front of someone who was on their way somewhere. Distinct from
  // headless(), because the on-open pull is automatic but still draws: the user is
  // standing there waiting for the book.
  bool automatic = false;
  bool isAutomatic() const { return automatic; }
  bool headless() const { return returnTo == ReturnTo::Sleep; }

  // Set when the position has to come from progress.bin rather than from a
  // reader that handed it over.
  bool loadLocalFromDisk = false;

  // How long to chase the network before giving up. The sleep path can afford to
  // wait -- nobody is watching -- but on open the user is holding a device that has
  // not shown them their book yet, so it gives up sooner.
  static constexpr unsigned long SILENT_CONNECT_TIMEOUT_MS = 12000;
  static constexpr unsigned long SILENT_CONNECT_TIMEOUT_ON_OPEN_MS = 6000;
  unsigned long silentConnectTimeout() const {
    return headless() ? SILENT_CONNECT_TIMEOUT_MS : SILENT_CONNECT_TIMEOUT_ON_OPEN_MS;
  }
  // Sleeps in a row that may fail before the owed sync is dropped. Without a cap,
  // a week away from a known network costs a radio session on every single sleep.
  static constexpr uint8_t MAX_SYNC_ATTEMPTS = 3;

  void onWifiSelectionComplete(bool success);
  bool connectSilently();
  bool loadLocalProgressFromDisk();
  void clearPendingSync();
  void recordFailedAttempt();
  void performSync();
  void performUpload();
  bool smartSyncEnabled() const;
  void markAutoReturn(unsigned long delayMs = AUTO_RETURN_DELAY_MS);
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToCaller();
  void failSync(const char* message);

  // The UiAppHost app hosts the interactive states (SHOWING_RESULT compare
  // rows and the NO_REMOTE_PROGRESS upload prompt) so they get themed
  // rows/buttons and tap-flash; the header stays on GUI.drawHeader for the
  // battery indicator and the purely-informational states keep their raw
  // centered text.
  static void resultScreen(UiScreen& screen, void* user);
  static void onResultRow(const freeink::ui::ActionEvent& event, void* user);
  void buildResultScreen(UiScreen& screen);
  void chooseResultOption();
  void startUpload();
};
