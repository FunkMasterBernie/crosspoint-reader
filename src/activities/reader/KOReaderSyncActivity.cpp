#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>

#include "CrossPointState.h"
#include "DeepSleep.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ProgressComparison.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"  // list icons for the compare rows
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// One action id for both interactive states: the compare rows (SHOWING_RESULT)
// and the upload button (NO_REMOTE_PROGRESS) never coexist, so state
// disambiguates them in the handler.
constexpr fui::ActionId ACTION_ROW = 1;

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

}  // namespace

KOReaderSyncActivity::KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& epubPath, CrossPointPosition localPosition,
                                           SavedProgressPosition localKoPos, std::string localChapterName)
    : Activity("KOReaderSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      localChapterName(std::move(localChapterName)),
      localPosition(localPosition),
      remoteProgress{},
      remotePosition{},
      localProgress(std::move(localKoPos)) {}

KOReaderSyncActivity::KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& epubPath)
    : Activity("KOReaderSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      localPosition{},
      remoteProgress{},
      remotePosition{},
      localProgress{},
      returnTo(ReturnTo::Sleep),
      loadLocalFromDisk(true) {}

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  std::optional<uint32_t> offset;
  if (remotePosition.hasVisibleTextOffset && remotePosition.spineIndex == spineIndex) {
    offset = remotePosition.visibleTextOffset;
  }
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0, offset)) {
    failSync(tr(STR_SAVE_PROGRESS_FAILED));
    return;
  }
  // The local position is now the server's, so nothing is owed either way.
  clearPendingSync();
  returnToCaller();
}

void KOReaderSyncActivity::returnToCaller() {
  if (returnTo == ReturnTo::Sleep) {
    requestDeepSleep();  // does not return
    return;
  }
  activityManager.goToReader(epubPath);
}

void KOReaderSyncActivity::clearPendingSync() {
  if (APP_STATE.pendingSyncPath.empty()) return;
  if (headless()) LOG_INF("KOSync", "Sleep sync done: %s", APP_STATE.pendingSyncPath.c_str());
  APP_STATE.clearSyncPending();
  APP_STATE.saveToFile();
}

// A headless sync that could not deliver keeps the book owed, so the next sleep
// tries again -- but only a few times. Away from every known network, each retry
// is a dozen seconds of radio for nothing.
void KOReaderSyncActivity::recordFailedAttempt() {
  if (!headless() || APP_STATE.pendingSyncPath.empty()) return;
  if (++APP_STATE.pendingSyncAttempts >= MAX_SYNC_ATTEMPTS) {
    LOG_DBG("KOSync", "Giving up on %s after %u attempts", APP_STATE.pendingSyncPath.c_str(),
            static_cast<unsigned>(APP_STATE.pendingSyncAttempts));
    APP_STATE.clearSyncPending();
  }
  APP_STATE.saveToFile();
}

void KOReaderSyncActivity::failSync(const char* message) {
  {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage = message ? message : "";
  }
  // Nobody is waiting to acknowledge an automatic sync, so don't strand the
  // error on screen -- show it briefly, then carry on out of the book.
  if (headless()) LOG_INF("KOSync", "Sleep sync failed: %s", message ? message : "unknown");
  recordFailedAttempt();
  if (isAutomatic()) markAutoReturn(AUTO_RETURN_ERROR_DELAY_MS);
  requestUpdate(true);
}

bool KOReaderSyncActivity::smartSyncEnabled() const {
  // Nobody is watching an automatic sync, so "Ask Every Time" has nobody to ask.
  // Resolving by furthest progress is the only option that can finish on its own.
  return isAutomatic() || KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn(const unsigned long delayMs) {
  // The delays exist so a result stays on screen long enough to read. A headless
  // run has no result on screen, so it goes on the next loop tick.
  autoReturnAt = millis() + (headless() ? 0 : delayMs);
}

void KOReaderSyncActivity::completeAlreadySynced() {
  clearPendingSync();
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    if (headless()) LOG_INF("KOSync", "Sleep sync failed: no network");
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    recordFailedAttempt();
    returnToCaller();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Keep the station fully awake for the short sync transaction. The web server
  // does the same because ESP32 modem sleep can introduce multi-second network
  // stalls that surface as HTTP timeouts. WiFi is torn down when this activity exits.
  WiFi.setSleep(false);
  LOG_DBG("KOSync", "WiFi sleep disabled for sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  // KOSync requests from CrossPoint do not include a client timestamp.
  performSync();
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    failSync(tr(STR_HASH_FAILED));
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress. In smart mode, retain the alternate document-id
  // record until both records can be mapped after the Epub is reloaded.
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

  KOReaderProgress alternateProgress;
  bool hasAlternateProgress = false;
  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK) {
        alternateProgress = std::move(altProgress);
        hasAlternateProgress = true;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND && hasAlternateProgress) {
    remoteProgress = std::move(alternateProgress);
    hasAlternateProgress = false;
    result = KOReaderSyncClient::OK;
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found for known document hashes; uploading local %.6f",
              localProgress.percentage);
      performUpload();
      return;
    }

    if (isAutomatic()) {
      LOG_DBG("KOSync", "Automatic sync: no remote progress, uploading local %.6f", localProgress.percentage);
      performUpload();
      return;
    }

    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    failSync(KOReaderSyncClient::errorString(result));
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    failSync(nullptr);
    return;
  }

  {
    RenderLock lock;
    GfxRenderer::FrameBufferLoan loan(renderer);

    const auto mapRemoteProgress = [&](const KOReaderProgress& progress) {
      // The standard KOReader progress XPath is the authoritative content anchor.
      // The CrossPoint server's existing rich page hints remain a legacy fallback.
      const SavedProgressPosition koPos = {progress.progress, progress.percentage};
      CrossPointPosition mapped =
          ProgressMapper::toCrossPoint(epub, koPos, renderer, localPosition.spineIndex, localPosition.totalPages);
      if (!mapped.hasVisibleTextOffset && progress.position.has_value()) {
        // toCrossPoint above already tried koPos.xpath; if the rich position carries the same XPath,
        // tell fromRichPosition to skip re-resolving it and use its page hints directly.
        const bool sameXPath = progress.position->xpath == progress.progress;
        if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *progress.position, renderer, sameXPath)) {
          mapped = *richMapped;
        }
      }
      return mapped;
    };

    remotePosition = mapRemoteProgress(remoteProgress);
    if (hasAlternateProgress) {
      const CrossPointPosition alternatePosition = mapRemoteProgress(alternateProgress);
      if (selectRemoteRecord(remotePosition, remoteProgress.percentage, alternatePosition,
                             alternateProgress.percentage) == RemoteRecordChoice::Alternate) {
        remoteProgress = std::move(alternateProgress);
        remotePosition = alternatePosition;
        LOG_DBG("KOSync", "Selected alternate remote record after mapped-position comparison");
      } else {
        LOG_DBG("KOSync", "Kept primary remote record after mapped-position comparison");
      }
    }
  }

  const ProgressComparison comparison =
      compareProgress(localPosition, localProgress.percentage, remotePosition, remoteProgress.percentage);
  if (smartSyncEnabled()) {
    LOG_DBG("KOSync", "Smart decision: doc=%s result=%d local=%.6f remote=%.6f remoteXpath=%s mapped=%d/%d",
            primaryHash.c_str(), static_cast<int>(comparison), localProgress.percentage, remoteProgress.percentage,
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
    switch (comparison) {
      case ProgressComparison::Synchronized:
        completeAlreadySynced();
        return;
      case ProgressComparison::LocalAhead:
        performUpload();
        return;
      case ProgressComparison::RemoteAhead:
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
        return;
      case ProgressComparison::Unknown:
        if (isAutomatic()) {
          LOG_DBG("KOSync", "Comparison unknown on an automatic sync; uploading local position");
          performUpload();
          return;
        }
        LOG_DBG("KOSync", "Smart sync comparison unknown; opening manual selection");
        break;
    }
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    selectedOption = comparison == ProgressComparison::LocalAhead ? 1 : 0;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  // Rich CrossPoint position for the default CrossPoint sync server (lossless
  // CrossPoint<->CrossPoint sync). The HTTP client also enforces this boundary
  // before serializing the extension.
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(localPosition.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(localPosition.pageNumber);
    pos.totalPages = static_cast<uint16_t>(localPosition.totalPages > 0 ? localPosition.totalPages : 1);
    if (localPosition.hasParagraphIndex) {
      pos.paragraphIndex = localPosition.paragraphIndex;
    }
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading from NO_REMOTE_PROGRESS the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    failSync(KOReaderSyncClient::errorString(result));
    return;
  }

  clearPendingSync();
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  // A headless run paints nothing, so it has no layout to orient and no business
  // touching what the outgoing activity left on the panel.
  if (!headless()) ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  resetUi();
  app.on(ACTION_ROW, &KOReaderSyncActivity::onResultRow, this);
  app.setScreen(&KOReaderSyncActivity::resultScreen, this);

  // The press that started this sync -- a long Back on the way out of the book,
  // or a long Confirm -- is often still pending, and the download progress
  // callback below reads input so Back can cancel. Swallow that edge here or it
  // cancels the sync it just started.
  mappedInput.update(true);
  (void)mappedInput.wasReleased(MappedInputManager::Button::Back);
  (void)mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  (void)mappedInput.wasHomeGesture();

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // The reader is gone by now on the sleep path, so recover the position the way
  // the reader itself would on the next open: straight out of progress.bin.
  if (loadLocalFromDisk && !loadLocalProgressFromDisk()) {
    LOG_ERR("KOSync", "No local progress to sync for %s", epubPath.c_str());
    clearPendingSync();  // nothing to send, and retrying will not change that
    returnToCaller();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // A headless sync must never put a network picker in front of someone who
  // pressed the power button. Try the last known network and give up quietly.
  if (headless()) {
    onWifiSelectionComplete(connectSilently());
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

bool KOReaderSyncActivity::connectSilently() {
  // The credential store is normally populated by WifiSelectionActivity, which
  // this path deliberately skips -- load it here or there is nothing to join.
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) {
    LOG_DBG("KOSync", "No last-connected network to join silently");
    return false;
  }
  const auto credential = WIFI_STORE.findCredential(ssid);
  if (!credential) {
    LOG_DBG("KOSync", "No saved password for %s", ssid.c_str());
    return false;
  }

  LOG_DBG("KOSync", "Joining %s silently", ssid.c_str());
  WiFi.persistent(false);  // credentials live in WifiCredentialStore, not SDK NVS
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  if (credential->password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), credential->password.c_str());
  }

  const unsigned long deadline = millis() + SILENT_CONNECT_TIMEOUT_MS;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED) {
      LOG_DBG("KOSync", "Joined %s", ssid.c_str());
      return true;
    }
    delay(100);
  }
  LOG_DBG("KOSync", "Silent join timed out after %lums", SILENT_CONNECT_TIMEOUT_MS);
  return false;
}

bool KOReaderSyncActivity::loadLocalProgressFromDisk() {
  ensureEpubLoaded();
  if (!epub) return false;

  bool loaded = false;
  {
    HalFile progressFile;
    if (Storage.openFileForRead("KOSync", epub->getCachePath() + "/progress.bin", progressFile)) {
      // Same 4/6/10-byte layout EpubReaderActivity writes and reads back.
      uint8_t data[10];
      const int size = progressFile.read(data, sizeof(data));
      if (size == 4 || size == 6 || size == 10) {
        localPosition.spineIndex = data[0] | (data[1] << 8);
        localPosition.pageNumber = data[2] | (data[3] << 8);
        if (localPosition.pageNumber == UINT16_MAX) localPosition.pageNumber = 0;
        localPosition.totalPages = (size >= 6) ? (data[4] | (data[5] << 8)) : 0;
        if (size == 10) {
          localPosition.visibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                            (static_cast<uint32_t>(data[8]) << 16) |
                                            (static_cast<uint32_t>(data[9]) << 24);
          localPosition.hasVisibleTextOffset = true;
        }
        loaded = true;
      }
    }
  }

  if (!loaded) {
    epub.reset();
    return false;
  }

  {
    // No rendering may run while the chapter mapper borrows the framebuffer.
    GfxRenderer::FrameBufferLoan loan(renderer);
    localProgress = ProgressMapper::toSavedProgress(epub, localPosition);
  }
  // Released before TLS, as everywhere else on this path.
  epub.reset();

  LOG_DBG("KOSync", "Local progress from disk: spine=%d page=%d pct=%.6f", localPosition.spineIndex,
          localPosition.pageNumber, localProgress.percentage);
  return true;
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    // Sleep is its own heap reset on wake, and enterDeepSleep() has already
    // latched deepSleepInProgress by the time this runs, so the reboot would be
    // suppressed anyway -- skip it explicitly rather than relying on that.
    if (!headless()) silentRestartToReader();
  }
}

void KOReaderSyncActivity::chooseResultOption() {
  if (selectedOption == 0) {
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
  } else {
    performUpload();
  }
}

void KOReaderSyncActivity::startUpload() {
  if (documentHash.empty()) {
    documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                       ? KOReaderDocumentId::calculateFromFilename(epubPath)
                       : KOReaderDocumentId::calculate(epubPath);
  }
  performUpload();
}

void KOReaderSyncActivity::onResultRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<KOReaderSyncActivity*>(user);
  // Activation leaves this screen (applies/uploads); drop the flash so it does
  // not ghost onto the next paint.
  self->app.clearTapFlash();
  if (self->state == SHOWING_RESULT) {
    if (event.value < 0 || event.value > 1) return;
    self->selectedOption = event.value;
    self->chooseResultOption();
  } else if (self->state == NO_REMOTE_PROGRESS) {
    self->startUpload();
  }
}

void KOReaderSyncActivity::resultScreen(UiScreen& screen, void* user) {
  static_cast<KOReaderSyncActivity*>(user)->buildResultScreen(screen);
}

void KOReaderSyncActivity::buildResultScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Side padding is 0 here (like the other FreeInkApp screens): the action list
  // supplies its own theme side padding, and the raw comparison text is indented
  // to line up with the list rows below (see labelIndent).
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == SHOWING_RESULT) {
    // Chapter names (remote requires the lazily-loaded Epub; local was
    // pre-computed before the Epub was released).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    char localChapterFallback[32];
    const char* localChapter = localChapterName.c_str();
    if (localChapterName.empty()) {
      snprintf(localChapterFallback, sizeof(localChapterFallback), "%s%d", tr(STR_SECTION_PREFIX),
               localPosition.spineIndex + 1);
      localChapter = localChapterFallback;
    }

    char remoteVal[64];
    snprintf(remoteVal, sizeof(remoteVal), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    char localVal[64];
    snprintf(localVal, sizeof(localVal), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), localPosition.pageNumber + 1,
             localPosition.totalPages, localProgress.percentage * 100);
    char deviceStr[80];
    deviceStr[0] = '\0';
    if (!remoteProgress.device.empty()) {
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
    }

    // Labeled, multi-line comparison flowing from the top. Indent everything to
    // the list rows' content-left (the row inset + side padding the list adds
    // below) so the "Remote"/"Local" labels sit directly above the row icons.
    auto labelStyle = screen.theme().bodyText;
    labelStyle.bold = true;
    auto detailStyle = screen.theme().smallText;
    const int16_t labelH = screen.target().lineHeight(labelStyle.font);
    const int16_t detailH = screen.target().lineHeight(detailStyle.font);
    const int16_t labelIndent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
    const int16_t detailIndent = static_cast<int16_t>(labelIndent + screen.theme().spaceMd);
    const auto textLine = [&](const char* text, const fui::TextStyle& style, int16_t height, int16_t indent,
                              int16_t gap) {
      fui::Rect r = screen.takeTop(height, gap);
      r.x = static_cast<int16_t>(r.x + indent);
      r.width = static_cast<int16_t>(r.width - indent);
      screen.target().text(r, text, style);
    };
    const auto labelLine = [&](const char* text) {
      textLine(text, labelStyle, labelH, labelIndent, screen.theme().spaceSm);
    };
    const auto detailLine = [&](const char* text) {
      textLine(text, detailStyle, detailH, detailIndent, screen.theme().spaceXs);
    };

    labelLine(tr(STR_REMOTE_LABEL));
    detailLine(remoteChapter.c_str());
    detailLine(remoteVal);
    if (deviceStr[0] != '\0') detailLine(deviceStr);
    screen.spacer(screen.theme().spaceLg);
    labelLine(tr(STR_LOCAL_LABEL));
    detailLine(localChapter);
    detailLine(localVal);

    // Two themed action rows flowing directly below the labels (not anchored to
    // the bottom). Rendered through the list component so they inherit the
    // active theme's row radius, insets, and selection style, matching every
    // other selectable list in the UI. Apply Remote pulls (download), Upload
    // Local pushes (upload); the selected row highlights for physical-button
    // users and tap works either way.
    screen.spacer(screen.theme().spaceMd);
    fui::ListItem actions[2];
    actions[0].label = tr(STR_APPLY_REMOTE);
    actions[0].icon = fui::bitmapFromIcon(icon_download_24);
    actions[0].actionValue = 0;
    actions[1].label = tr(STR_UPLOAD_LOCAL);
    actions[1].icon = fui::bitmapFromIcon(icon_upload_24);
    actions[1].actionValue = 1;
    fui::ListProps actionProps;
    actionProps.items = actions;
    actionProps.count = 2;
    actionProps.selectedIndex = static_cast<int16_t>(selectedOption);
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;  // physical buttons stay in loop()
    actionProps.scrollIndicator = false;      // never scrolls; no indicator needed
    // Non-touch hardware (X3/X4) keeps the original, denser row height instead
    // of FreeInkUI's touch-target-sized default (see
    // UiListActivity::syncListViewport); actionsBand must use the same value
    // or the band and the rows it contains fall out of sync.
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    // Keep the theme's row inset + side padding so the selected-row highlight has
    // the same padding around its icon/label as every other list in the UI; the
    // labels above are indented to match this content-left.
    const auto actionsBand =
        static_cast<int16_t>(actionRowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm);
    screen.list(actionProps, actionsBand);
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    auto centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    auto centeredBold = centered;
    centeredBold.bold = true;
    const int16_t lineH = screen.target().lineHeight(centered.font);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceSm), tr(STR_NO_REMOTE_MSG), centeredBold);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), tr(STR_UPLOAD_PROMPT), centered);

    // Single themed action row anchored to the bottom, matching the lists used
    // everywhere else (inherits the theme's row radius + selection style).
    fui::ListItem action;
    action.label = tr(STR_UPLOAD_LOCAL);
    action.actionValue = 0;
    fui::ListProps actionProps;
    actionProps.items = &action;
    actionProps.count = 1;
    actionProps.selectedIndex = 0;
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;
    actionProps.scrollIndicator = false;
    // See the equivalent override above; keeps actionsBand in sync with the
    // row height actually used on non-touch hardware (X3/X4).
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    const auto actionsBand = static_cast<int16_t>(actionRowHeight + screen.theme().spaceMd);
    screen.list(actionProps, actionsBand, fui::LayoutAnchor::Bottom);
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 state == SHOWING_RESULT ? tr(STR_PROGRESS_FOUND) : tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Comparison rows + option selection render through the FreeInkApp
    // (themed rows, tap-flash); the header above shows "Progress Found".
    renderUi();

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Prompt text + upload button render through the FreeInkApp.
    renderUi();

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToCaller();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToCaller();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Touch goes through the FreeInkApp: render() registered the compare rows;
    // route the snapshot and let onResultRow apply/upload on tap.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow

    // Navigate the two options with physical buttons.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseResultOption();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToCaller();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Touch goes through the FreeInkApp: render() registered the upload button.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow -> startUpload

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToCaller();
    }
    return;
  }
}
