#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <memory>
#include <optional>

#include "stems/stemcache.h"
#include "stems/stemseparationmanager.h"
#include "track/track_decl.h"

class BaseTrackPlayer;
class ControlObject;
class ControlPotmeter;
class ControlPushButton;

namespace mixxx::stems {

class StemAlternateSourceLinker;
class StemModelManager;

/// Connects the separation backend to stable ControlObjects and deck loads.
///
/// All filesystem hashing and inference work is delegated to background
/// workers. This QObject and its ControlObjects live on the GUI thread.
class StemStatusControlProvider final : public QObject {
    Q_OBJECT

  public:
    struct Settings {
        QString rootDirectoryPath;
        StemCache::Limits cacheLimits;
        int inferenceThreadCount = 2;
        int bitRatePerStream = 128000;
        bool modelRequired = true;
    };

    explicit StemStatusControlProvider(Settings settings,
            std::shared_ptr<StemSeparationProcessor> pProcessor = {},
            QObject* pParent = nullptr);
    ~StemStatusControlProvider() override;

    bool initialize(QString* pErrorMessage = nullptr);
    void registerDeck(BaseTrackPlayer* pDeck);

    QSet<QString> protectedEntryIds() const;

  private:
    enum class CacheStatus {
        None = 0,
        Ready = 1,
        Processing = 2,
    };

    enum class ErrorCode {
        None = 0,
        ModelUnavailable = 1,
        InvalidSource = 2,
        QueueFailure = 3,
        ProcessingFailure = 4,
    };

    struct DeckState {
        BaseTrackPlayer* pDeck = nullptr;
        QString sourceFilePath;
        QString jobId;
        QString cacheEntryId;
        QString liveSessionId;
        TrackPointer pLiveSessionTrack;
        QString previousSourceFilePath;
        QString previousCacheEntryId;
        QString previousLiveSessionId;
        TrackPointer pPreviousLiveSessionTrack;
        StemSeparationPriority requestedPriority =
                StemSeparationPriority::LoadedNotPlaying;
        quint64 fingerprintGeneration = 0;
        std::shared_ptr<std::atomic_bool>
                pFingerprintCancellation;
        bool pendingModel = false;
        std::unique_ptr<ControlPushButton> pTrigger;
        std::unique_ptr<ControlPushButton> pCancel;
        std::unique_ptr<ControlObject> pPercentage;
        std::unique_ptr<ControlObject> pState;
        std::unique_ptr<ControlObject> pQueuePosition;
        std::unique_ptr<ControlObject> pCacheStatus;
        std::unique_ptr<ControlObject> pLiveReady;
        std::unique_ptr<ControlObject> pError;
    };

    std::shared_ptr<DeckState> deckState(
            const QString& group) const;
    void trigger(const QString& group,
            StemSeparationPriority priority =
                    StemSeparationPriority::Manual);
    void cancel(const QString& group);
    void beginFingerprint(
            const std::shared_ptr<DeckState>& pDeckState);
    void fingerprintFinished(const QString& group,
            quint64 generation,
            QString sourceFilePath,
            QString displayName,
            std::optional<StemCacheKey> key,
            QString error);
    void enqueueRequest(
            const std::shared_ptr<DeckState>& pDeckState,
            QString sourceFilePath,
            QString cacheEntryId,
            QString displayName,
            QString* pErrorMessage = nullptr);
    QUrl resolveForDeck(
            const QString& group, const TrackPointer& pTrack);
    void trackLoading(const QString& group,
            const TrackPointer& pNewTrack,
            const TrackPointer& pOldTrack);
    void modelAvailabilityChanged(bool available);
    void jobChanged(const QString& jobId);
    void queueChanged(int queueSize, int activeJobs);
    void enabledChanged(double value);
    void updateDeckFromSnapshot(
            DeckState* pDeckState,
            const StemSeparationSnapshot& snapshot);
    void resetDeck(
            DeckState* pDeckState, const TrackPointer& pTrack);
    void updateWorkerState();
    void updateCacheSize();
    void refreshProtectedEntryIds();
    bool modelReady() const;
    static double errorCodeForMessage(const QString& error);

    Settings m_settings;
    std::shared_ptr<StemSeparationProcessor> m_pProcessor;
    std::unique_ptr<StemModelManager> m_pModelManager;
    std::unique_ptr<StemAlternateSourceLinker>
            m_pAlternateSourceLinker;
    std::unique_ptr<StemSeparationManager> m_pManager;
    QThreadPool m_fingerprintPool;
    QHash<QString, std::shared_ptr<DeckState>> m_decks;
    mutable QMutex m_protectedEntryIdsMutex;
    QSet<QString> m_protectedEntryIds;
    std::unique_ptr<ControlPushButton> m_pEnabled;
    std::unique_ptr<ControlObject> m_pQueueSize;
    std::unique_ptr<ControlObject> m_pActiveJobs;
    std::unique_ptr<ControlObject> m_pCacheSizeBytes;
    std::unique_ptr<ControlObject> m_pCacheLimitBytes;
    std::unique_ptr<ControlObject> m_pWorkerState;
    std::unique_ptr<ControlObject> m_pModelAvailable;
    std::unique_ptr<ControlObject> m_pModelDownloadProgress;
    std::unique_ptr<ControlPushButton> m_pActiveMode;
    std::unique_ptr<ControlPotmeter> m_pInferenceThreads;
    bool m_initialized = false;
    bool m_shuttingDown = false;
};

} // namespace mixxx::stems
