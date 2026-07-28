#include "stems/stemstatuscontrolprovider.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QPointer>
#include <QRunnable>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <utility>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "mixer/basetrackplayer.h"
#include "moc_stemstatuscontrolprovider.cpp"
#include "stems/demucsstemseparationprocessor.h"
#include "stems/stemalternatesourcelinker.h"
#include "stems/stemlivesessionregistry.h"
#include "stems/stemmodelmanager.h"
#include "track/track.h"

namespace mixxx::stems {
namespace {

const QString kGlobalGroup =
        QStringLiteral("[StemSeparation]");
const QString kModelDirectoryName = QStringLiteral("model");
const QString kCacheDirectoryName = QStringLiteral("cache");
const QString kLiveDirectoryName = QStringLiteral("live");
const QString kQueueFileName = QStringLiteral("queue.json");

std::unique_ptr<ControlObject> readOnlyControl(
        const QString& group,
        const QString& item,
        double initialValue = 0.0) {
    auto pControl =
            std::make_unique<ControlObject>(ConfigKey(group, item));
    pControl->setReadOnly();
    pControl->setAndConfirm(initialValue);
    return pControl;
}

} // namespace

StemStatusControlProvider::StemStatusControlProvider(
        Settings settings,
        std::shared_ptr<StemSeparationProcessor> pProcessor,
        QObject* pParent)
        : QObject(pParent),
          m_settings(std::move(settings)),
          m_pProcessor(std::move(pProcessor)) {
    m_fingerprintPool.setMaxThreadCount(1);
    m_fingerprintPool.setThreadPriority(QThread::LowPriority);

    m_pEnabled = std::make_unique<ControlPushButton>(
            ConfigKey(kGlobalGroup, QStringLiteral("enabled")),
            true,
            1.0);
    m_pEnabled->setButtonMode(
            mixxx::control::ButtonMode::Toggle);
    m_pQueueSize = readOnlyControl(
            kGlobalGroup, QStringLiteral("queue_size"));
    m_pActiveJobs = readOnlyControl(
            kGlobalGroup, QStringLiteral("active_jobs"));
    m_pCacheSizeBytes = readOnlyControl(
            kGlobalGroup, QStringLiteral("cache_size_bytes"));
    m_pCacheLimitBytes = readOnlyControl(kGlobalGroup,
            QStringLiteral("cache_limit_bytes"),
            static_cast<double>(m_settings.cacheLimits.quotaBytes));
    m_pWorkerState = readOnlyControl(
            kGlobalGroup, QStringLiteral("worker_state"));
    m_pModelAvailable = readOnlyControl(
            kGlobalGroup, QStringLiteral("model_available"));
    m_pModelDownloadProgress = readOnlyControl(kGlobalGroup,
            QStringLiteral("model_download_progress"));
    m_pActiveMode = std::make_unique<ControlPushButton>(
            ConfigKey(kGlobalGroup,
                    QStringLiteral("active_mode")),
            true,
            0.0);
    m_pActiveMode->setButtonMode(
            mixxx::control::ButtonMode::Toggle);
#if defined(Q_OS_WIN)
    constexpr int kPlatformThreadLimit = 8;
#else
    constexpr int kPlatformThreadLimit = 4;
#endif
    const auto maximumThreadCount = std::clamp(
            QThread::idealThreadCount(),
            1,
            kPlatformThreadLimit);
    m_pInferenceThreads =
            std::make_unique<ControlPotmeter>(
                    ConfigKey(kGlobalGroup,
                            QStringLiteral(
                                    "inference_threads")),
                    1.0,
                    static_cast<double>(maximumThreadCount),
                    false,
                    true,
                    false,
                    true,
                    static_cast<double>(std::clamp(
                            m_settings.inferenceThreadCount,
                            1,
                            maximumThreadCount)));
    m_pInferenceThreads->setStepCount(
            std::max(1, maximumThreadCount - 1));

    connect(m_pEnabled.get(),
            &ControlObject::valueChanged,
            this,
            &StemStatusControlProvider::enabledChanged);
    connect(m_pActiveMode.get(),
            &ControlObject::valueChanged,
            this,
            [this](double value) {
                for (const auto& pDeckState : m_decks) {
                    const ConfigKey key(
                            pDeckState->pDeck->getGroup(),
                            QStringLiteral("stem_active_mode"));
                    if (ControlObject::exists(key)) {
                        ControlObject::set(key, value);
                    }
                }
            });
    connect(m_pInferenceThreads.get(),
            &ControlObject::valueChanged,
            this,
            [this](double value) {
                if (m_pManager) {
                    m_pManager->setInferenceThreadCount(
                            std::max(1,
                                    static_cast<int>(
                                            std::lround(value))));
                }
            });
}

StemStatusControlProvider::~StemStatusControlProvider() {
    m_shuttingDown = true;
    m_fingerprintPool.clear();
    m_fingerprintPool.waitForDone();
    for (const auto& pDeckState : m_decks) {
        if (!pDeckState->liveSessionId.isEmpty()) {
            StemLiveSessionRegistry::release(
                    pDeckState->liveSessionId,
                    pDeckState->pLiveSessionTrack);
            pDeckState->liveSessionId.clear();
            pDeckState->pLiveSessionTrack.reset();
        }
    }
    m_pManager.reset();
}

bool StemStatusControlProvider::initialize(
        QString* pErrorMessage) {
    if (m_initialized) {
        return true;
    }
    if (m_settings.rootDirectoryPath.isEmpty() ||
            m_settings.inferenceThreadCount <= 0 ||
            m_settings.bitRatePerStream <= 0 ||
            !QDir().mkpath(m_settings.rootDirectoryPath)) {
        if (pErrorMessage) {
            *pErrorMessage = QStringLiteral(
                    "Invalid stem separation service settings");
        }
        return false;
    }
    const QDir root(m_settings.rootDirectoryPath);
    const QDir liveDirectory(
            root.filePath(kLiveDirectoryName));
    if (!QDir().mkpath(liveDirectory.path())) {
        if (pErrorMessage) {
            *pErrorMessage = QStringLiteral(
                    "Failed to create temporary stem directory");
        }
        return false;
    }
    StemLiveSessionRegistry::removeStaleFiles(
            liveDirectory.path());
    const auto cachePath =
            root.filePath(kCacheDirectoryName);
    m_pAlternateSourceLinker =
            std::make_unique<StemAlternateSourceLinker>(
                    cachePath);
    if (!m_pAlternateSourceLinker->initialize(
                pErrorMessage)) {
        return false;
    }

    if (m_settings.modelRequired) {
        m_pModelManager = std::make_unique<StemModelManager>(
                StemModelManager::pinnedSettings(
                        root.filePath(kModelDirectoryName)));
        connect(m_pModelManager.get(),
                &StemModelManager::availabilityChanged,
                this,
                &StemStatusControlProvider::
                        modelAvailabilityChanged);
        connect(m_pModelManager.get(),
                &StemModelManager::downloadProgressChanged,
                this,
                [this](double percentage) {
                    m_pModelDownloadProgress->setAndConfirm(
                            percentage);
                    updateWorkerState();
                });
        connect(m_pModelManager.get(),
                &StemModelManager::stateChanged,
                this,
                [this] {
                    updateWorkerState();
                });
    }

    if (!m_pProcessor) {
        if (!m_pModelManager) {
            if (pErrorMessage) {
                *pErrorMessage = QStringLiteral(
                        "A model manager or test processor is required");
            }
            return false;
        }
        const auto modelPath =
                m_pModelManager->modelFilePath();
        const auto modelSha =
                m_pModelManager->modelSha256();
        m_pProcessor =
                std::make_shared<DemucsStemSeparationProcessor>(
                        DemucsStemSeparationProcessor::Settings{
                                modelPath,
                                modelSha,
                                cachePath,
                                m_settings.cacheLimits,
                                std::max(1,
                                        static_cast<int>(
                                                std::lround(
                                                        m_pInferenceThreads
                                                                ->get()))),
                                m_settings.bitRatePerStream,
                        },
                        [this] {
                            return protectedEntryIds();
                        });
    }
    m_pManager = std::make_unique<StemSeparationManager>(
            root.filePath(kQueueFileName), m_pProcessor);
    connect(m_pManager.get(),
            &StemSeparationManager::jobChanged,
            this,
            &StemStatusControlProvider::jobChanged);
    connect(m_pManager.get(),
            &StemSeparationManager::queueChanged,
            this,
            &StemStatusControlProvider::queueChanged);
    connect(m_pManager.get(),
            &StemSeparationManager::workerPausedChanged,
            this,
            [this] {
                updateWorkerState();
            });
    connect(m_pManager.get(),
            &StemSeparationManager::jobFramesAvailable,
            this,
            [this](const QString& jobId,
                    qulonglong readyFrameCount,
                    qulonglong) {
                if (readyFrameCount == 0) {
                    return;
                }
                for (const auto& pDeckState : m_decks) {
                    if (pDeckState->jobId == jobId) {
                        pDeckState->pLiveReady
                                ->setAndConfirm(1.0);
                        pDeckState->pDeck
                                ->publishStemFramesAvailable(
                                        static_cast<SINT>(
                                                readyFrameCount));
                    }
                }
            });
    if (!m_pManager->initialize(pErrorMessage)) {
        m_pManager.reset();
        return false;
    }
    m_initialized = true;
    if (m_pModelManager) {
        m_pModelManager->initialize();
    } else {
        m_pModelAvailable->setAndConfirm(1.0);
    }
    enabledChanged(m_pEnabled->get());
    queueChanged(
            m_pManager->queueSize(),
            m_pManager->activeJobCount());
    updateCacheSize();
    return true;
}

void StemStatusControlProvider::registerDeck(
        BaseTrackPlayer* pDeck) {
    if (!pDeck || m_decks.contains(pDeck->getGroup())) {
        return;
    }
    const auto group = pDeck->getGroup();
    auto pDeckState = std::make_shared<DeckState>();
    pDeckState->pDeck = pDeck;
    pDeckState->pTrigger =
            std::make_unique<ControlPushButton>(
                    ConfigKey(group,
                            QStringLiteral(
                                    "separation_trigger")));
    pDeckState->pTrigger->setButtonMode(
            mixxx::control::ButtonMode::Trigger);
    pDeckState->pCancel =
            std::make_unique<ControlPushButton>(
                    ConfigKey(group,
                            QStringLiteral(
                                    "separation_cancel")));
    pDeckState->pCancel->setButtonMode(
            mixxx::control::ButtonMode::Trigger);
    pDeckState->pPercentage = readOnlyControl(
            group, QStringLiteral("separation_percentage"));
    pDeckState->pState = readOnlyControl(group,
            QStringLiteral("separation_state"),
            static_cast<double>(modelReady()
                            ? StemSeparationState::Idle
                            : StemSeparationState::Unavailable));
    pDeckState->pQueuePosition = readOnlyControl(
            group, QStringLiteral("separation_queue_position"), -1.0);
    pDeckState->pCacheStatus = readOnlyControl(
            group, QStringLiteral("stem_cache_status"));
    pDeckState->pLiveReady = readOnlyControl(
            group, QStringLiteral("stem_live_ready"));
    pDeckState->pError = readOnlyControl(
            group, QStringLiteral("separation_error"));
    m_decks.insert(group, pDeckState);
    const ConfigKey activeModeKey(
            group, QStringLiteral("stem_active_mode"));
    if (ControlObject::exists(activeModeKey)) {
        ControlObject::set(
                activeModeKey, m_pActiveMode->get());
    }
    refreshProtectedEntryIds();

    connect(pDeckState->pTrigger.get(),
            &ControlObject::valueChanged,
            this,
            [this, group](double value) {
                if (value > 0.0) {
                    trigger(group);
                }
            });
    connect(pDeckState->pCancel.get(),
            &ControlObject::valueChanged,
            this,
            [this, group](double value) {
                if (value > 0.0) {
                    cancel(group);
                }
            });
    connect(pDeck,
            &BaseTrackPlayer::loadingTrack,
            this,
            [this, group](const TrackPointer& pNewTrack,
                    const TrackPointer& pOldTrack) {
                trackLoading(group, pNewTrack, pOldTrack);
            });
    connect(pDeck,
            &BaseTrackPlayer::newTrackLoaded,
            this,
            [this, group](const TrackPointer& pTrack) {
                const auto pDeckState = deckState(group);
                if (pTrack && pDeckState &&
                        pTrack->hasStem() &&
                        pDeckState->liveSessionId.isEmpty()) {
                    pDeckState->pLiveReady
                            ->setAndConfirm(1.0);
                    pDeckState->pState->setAndConfirm(
                            static_cast<double>(
                                    StemSeparationState::Ready));
                    pDeckState->pPercentage
                            ->setAndConfirm(100.0);
                    pDeckState->pCacheStatus->setAndConfirm(
                            static_cast<double>(
                                    CacheStatus::Ready));
                    return;
                }
                if (pTrack && pDeckState &&
                        pDeckState->pCacheStatus->get() !=
                                static_cast<double>(
                                        CacheStatus::Ready)) {
                    trigger(group,
                            StemSeparationPriority::
                                    LoadedNotPlaying);
                }
            });
    pDeck->setAlternateAudioSourceResolver(
            [weakThis = QPointer<StemStatusControlProvider>(this),
                    group](const TrackPointer& pTrack) {
                return weakThis
                        ? weakThis->resolveForDeck(group, pTrack)
                        : QUrl{};
            });
}

QSet<QString>
StemStatusControlProvider::protectedEntryIds() const {
    const QMutexLocker locker(&m_protectedEntryIdsMutex);
    return m_protectedEntryIds;
}

std::shared_ptr<StemStatusControlProvider::DeckState>
StemStatusControlProvider::deckState(
        const QString& group) const {
    const auto deck = m_decks.constFind(group);
    return deck == m_decks.cend() ? nullptr : deck.value();
}

void StemStatusControlProvider::trigger(
        const QString& group,
        StemSeparationPriority priority) {
    auto pDeckState = deckState(group);
    if (!pDeckState || !m_initialized ||
            !m_pEnabled->toBool()) {
        return;
    }
    const auto pTrack = pDeckState->pDeck->getLoadedTrack();
    if (!pTrack ||
            !QFileInfo(pTrack->getLocation()).isFile()) {
        pDeckState->pError->setAndConfirm(
                static_cast<double>(ErrorCode::InvalidSource));
        pDeckState->pState->setAndConfirm(
                static_cast<double>(
                        StemSeparationState::Failed));
        return;
    }
    pDeckState->sourceFilePath = pTrack->getLocation();
    pDeckState->requestedPriority = priority;
    pDeckState->pError->setAndConfirm(
            static_cast<double>(ErrorCode::None));
    refreshProtectedEntryIds();
    if (!modelReady()) {
        pDeckState->pendingModel = true;
        pDeckState->pState->setAndConfirm(
                static_cast<double>(
                        StemSeparationState::Unavailable));
        pDeckState->pError->setAndConfirm(
                static_cast<double>(
                        ErrorCode::ModelUnavailable));
        m_pModelManager->requestAvailable();
        updateWorkerState();
        return;
    }
    if (!pDeckState->liveSessionId.isEmpty()) {
        const auto pLiveSession =
                StemLiveSessionRegistry::find(
                        pDeckState->liveSessionId);
        const auto pTemporarySession = pLiveSession
                ? pLiveSession->temporarySession()
                : nullptr;
        if (pTemporarySession) {
            const auto readyFrameCount =
                    pTemporarySession->readyFrameCount.load(
                            std::memory_order_acquire);
            if (readyFrameCount > 0) {
                pDeckState->pLiveReady->setAndConfirm(1.0);
                pDeckState->pDeck
                        ->publishStemFramesAvailable(
                                static_cast<SINT>(
                                        readyFrameCount));
            }
            if (pTemporarySession->complete.load(
                        std::memory_order_acquire)) {
                pDeckState->pState->setAndConfirm(
                        static_cast<double>(
                                StemSeparationState::Ready));
                pDeckState->pPercentage->setAndConfirm(100.0);
                pDeckState->pCacheStatus->setAndConfirm(
                        static_cast<double>(
                                CacheStatus::Ready));
                return;
            }
        }
        pDeckState->pState->setAndConfirm(
                static_cast<double>(
                        StemSeparationState::Preparing));
        pDeckState->pPercentage->setAndConfirm(0.0);
        pDeckState->pQueuePosition->setAndConfirm(-1.0);
        pDeckState->pCacheStatus->setAndConfirm(
                static_cast<double>(CacheStatus::Processing));
        QString error;
        enqueueRequest(pDeckState,
                pDeckState->sourceFilePath,
                pDeckState->liveSessionId,
                QFileInfo(pDeckState->sourceFilePath)
                        .fileName(),
                &error);
        return;
    }
    beginFingerprint(pDeckState);
}

void StemStatusControlProvider::cancel(
        const QString& group) {
    auto pDeckState = deckState(group);
    if (!pDeckState) {
        return;
    }
    if (pDeckState->pFingerprintCancellation) {
        pDeckState->pFingerprintCancellation->store(
                true, std::memory_order_release);
        pDeckState->pFingerprintCancellation.reset();
    }
    ++pDeckState->fingerprintGeneration;
    pDeckState->pendingModel = false;
    if (m_pManager && !pDeckState->jobId.isEmpty()) {
        m_pManager->cancel(pDeckState->jobId);
    }
    pDeckState->jobId.clear();
    pDeckState->pState->setAndConfirm(
            static_cast<double>(
                    StemSeparationState::Cancelled));
    pDeckState->pPercentage->setAndConfirm(0.0);
    pDeckState->pQueuePosition->setAndConfirm(-1.0);
    pDeckState->pCacheStatus->setAndConfirm(
            static_cast<double>(CacheStatus::None));
    pDeckState->pError->setAndConfirm(
            static_cast<double>(ErrorCode::None));
}

void StemStatusControlProvider::beginFingerprint(
        const std::shared_ptr<DeckState>& pDeckState) {
    const auto group = pDeckState->pDeck->getGroup();
    const auto sourceFilePath = pDeckState->sourceFilePath;
    const auto displayName =
            QFileInfo(sourceFilePath).fileName();
    const auto generation =
            ++pDeckState->fingerprintGeneration;
    if (pDeckState->pFingerprintCancellation) {
        pDeckState->pFingerprintCancellation->store(
                true, std::memory_order_release);
    }
    const auto pCancellation =
            std::make_shared<std::atomic_bool>(false);
    pDeckState->pFingerprintCancellation = pCancellation;
    pDeckState->pendingModel = false;
    pDeckState->pState->setAndConfirm(
            static_cast<double>(
                    StemSeparationState::Preparing));
    pDeckState->pPercentage->setAndConfirm(0.0);
    pDeckState->pQueuePosition->setAndConfirm(-1.0);
    pDeckState->pCacheStatus->setAndConfirm(
            static_cast<double>(CacheStatus::Processing));

    const QPointer<StemStatusControlProvider> weakThis(this);
    const auto modelSha = m_pModelManager
            ? m_pModelManager->modelSha256()
            : QByteArray(64, '0');
    const auto bitRate = m_settings.bitRatePerStream;
    m_fingerprintPool.start(QRunnable::create(
            [weakThis,
                    group,
                    generation,
                    sourceFilePath,
                    displayName,
                    modelSha,
                    bitRate,
                    pCancellation] {
                QString error;
                const auto key = StemCacheKey::fromSourceFile(
                        sourceFilePath,
                        QStringLiteral("htdemucs"),
                        QStringLiteral("955717e8"),
                        modelSha,
                        QStringLiteral("aac-lc"),
                        bitRate,
                        1,
                        &error,
                        [weakThis, pCancellation] {
                            return !weakThis ||
                                    pCancellation->load(
                                            std::memory_order_acquire);
                        });
                if (!weakThis) {
                    return;
                }
                QMetaObject::invokeMethod(weakThis,
                        [weakThis,
                                group,
                                generation,
                                sourceFilePath,
                                displayName,
                                key,
                                error] {
                            if (weakThis) {
                                weakThis->fingerprintFinished(
                                        group,
                                        generation,
                                        sourceFilePath,
                                        displayName,
                                        key,
                                        error);
                            }
                        });
            }));
}

void StemStatusControlProvider::fingerprintFinished(
        const QString& group,
        quint64 generation,
        QString sourceFilePath,
        QString displayName,
        std::optional<StemCacheKey> key,
        QString error) {
    auto pDeckState = deckState(group);
    if (m_shuttingDown || !pDeckState ||
            pDeckState->fingerprintGeneration != generation ||
            pDeckState->sourceFilePath != sourceFilePath) {
        return;
    }
    pDeckState->pFingerprintCancellation.reset();
    if (!key) {
        pDeckState->pState->setAndConfirm(
                static_cast<double>(
                        StemSeparationState::Failed));
        pDeckState->pCacheStatus->setAndConfirm(
                static_cast<double>(CacheStatus::None));
        pDeckState->pError->setAndConfirm(
                static_cast<double>(ErrorCode::InvalidSource));
        return;
    }
    enqueueRequest(pDeckState,
            std::move(sourceFilePath),
            key->id(),
            std::move(displayName),
            &error);
}

void StemStatusControlProvider::enqueueRequest(
        const std::shared_ptr<DeckState>& pDeckState,
        QString sourceFilePath,
        QString cacheEntryId,
        QString displayName,
        QString* pErrorMessage) {
    if (!pDeckState || !m_pManager) {
        return;
    }
    pDeckState->cacheEntryId = std::move(cacheEntryId);
    pDeckState->jobId = m_pManager->enqueue(
            StemSeparationRequest{
                    std::move(sourceFilePath),
                    pDeckState->cacheEntryId,
                    std::move(displayName),
                    pDeckState->requestedPriority,
                    2,
                    pDeckState->liveSessionId,
            },
            pErrorMessage);
    if (pDeckState->jobId.isEmpty()) {
        pDeckState->pState->setAndConfirm(
                static_cast<double>(
                        StemSeparationState::Failed));
        pDeckState->pError->setAndConfirm(
                static_cast<double>(ErrorCode::QueueFailure));
        return;
    }
    const auto snapshot =
            m_pManager->snapshot(pDeckState->jobId);
    if (snapshot) {
        updateDeckFromSnapshot(
                pDeckState.get(), *snapshot);
    }
    refreshProtectedEntryIds();
}

QUrl StemStatusControlProvider::resolveForDeck(
        const QString& group, const TrackPointer& pTrack) {
    auto pDeckState = deckState(group);
    if (!pDeckState || !pTrack ||
            !m_pAlternateSourceLinker) {
        return {};
    }
    const auto sourceFilePath = pTrack->getLocation();
    if (pDeckState->sourceFilePath !=
            sourceFilePath) {
        pDeckState->previousSourceFilePath =
                pDeckState->sourceFilePath;
        pDeckState->previousCacheEntryId =
                pDeckState->cacheEntryId;
        pDeckState->previousLiveSessionId =
                pDeckState->liveSessionId;
        pDeckState->pPreviousLiveSessionTrack =
                pDeckState->pLiveSessionTrack;
        resetDeck(pDeckState.get(), pTrack);
    }
    const auto lowerSourceFilePath =
            sourceFilePath.toLower();
    if (pTrack->hasStem() ||
            lowerSourceFilePath.endsWith(
                    QStringLiteral(".stem.mp4")) ||
            lowerSourceFilePath.endsWith(
                    QStringLiteral(".stem.m4a"))) {
        pDeckState->pLiveReady->setAndConfirm(1.0);
        return {};
    }
    QString error;
    const auto resolution =
            m_pAlternateSourceLinker->resolve(
                    sourceFilePath, &error);
    if (resolution) {
        pDeckState->cacheEntryId = resolution->entryId;
        pDeckState->pCacheStatus->setAndConfirm(
                static_cast<double>(CacheStatus::Ready));
        pDeckState->pState->setAndConfirm(
                static_cast<double>(
                        StemSeparationState::Ready));
        pDeckState->pPercentage->setAndConfirm(100.0);
        pDeckState->pLiveReady->setAndConfirm(1.0);
        refreshProtectedEntryIds();
        return QUrl::fromLocalFile(
                resolution->filePath);
    }
    if (!pDeckState->liveSessionId.isEmpty()) {
        const auto pLiveSession =
                StemLiveSessionRegistry::find(
                        pDeckState->liveSessionId);
        if (pLiveSession) {
            return pLiveSession->url();
        }
    }
    const auto pLiveSession =
            StemLiveSessionRegistry::acquire(
                    QDir(m_settings.rootDirectoryPath)
                            .filePath(kLiveDirectoryName),
                    pTrack);
    if (!pLiveSession) {
        return {};
    }
    pDeckState->liveSessionId =
            pLiveSession->id();
    pDeckState->pLiveSessionTrack = pTrack;
    return pLiveSession->url();
}

void StemStatusControlProvider::trackLoading(
        const QString& group,
        const TrackPointer& pNewTrack,
        const TrackPointer& pOldTrack) {
    auto pDeckState = deckState(group);
    if (!pDeckState) {
        return;
    }
    auto oldSourceFilePath =
            pDeckState->previousSourceFilePath;
    auto oldCacheEntryId =
            pDeckState->previousCacheEntryId;
    auto oldLiveSessionId =
            pDeckState->previousLiveSessionId;
    auto pOldLiveSessionTrack =
            pDeckState->pPreviousLiveSessionTrack;
    pDeckState->previousSourceFilePath.clear();
    pDeckState->previousCacheEntryId.clear();
    pDeckState->previousLiveSessionId.clear();
    pDeckState->pPreviousLiveSessionTrack.reset();
    if (pOldTrack && pNewTrack &&
            pNewTrack->getLocation() ==
                    pOldTrack->getLocation()) {
        return;
    }
    if (oldSourceFilePath.isEmpty() && pOldTrack &&
            pDeckState->sourceFilePath ==
                    pOldTrack->getLocation()) {
        oldSourceFilePath = pDeckState->sourceFilePath;
        oldCacheEntryId = pDeckState->cacheEntryId;
        oldLiveSessionId =
                pDeckState->liveSessionId;
        pOldLiveSessionTrack =
                pDeckState->pLiveSessionTrack;
    }
    if (!pNewTrack) {
        resetDeck(pDeckState.get(), {});
    } else if (pDeckState->sourceFilePath !=
            pNewTrack->getLocation()) {
        resetDeck(pDeckState.get(), pNewTrack);
    }
    if (!oldLiveSessionId.isEmpty()) {
        StemLiveSessionRegistry::release(
                oldLiveSessionId,
                pOldLiveSessionTrack
                        ? pOldLiveSessionTrack
                        : pOldTrack);
    }
    if (!pOldTrack || !m_pManager) {
        return;
    }
    if (oldSourceFilePath.isEmpty()) {
        oldSourceFilePath = pOldTrack->getLocation();
    }
    const bool stillLoaded = std::any_of(
            m_decks.cbegin(),
            m_decks.cend(),
            [&](const auto& otherDeckState) {
                return otherDeckState->sourceFilePath ==
                        oldSourceFilePath;
            });
    if (stillLoaded) {
        return;
    }
    QSet<QString> discardedEntryIds;
    if (!oldCacheEntryId.isEmpty()) {
        discardedEntryIds.insert(oldCacheEntryId);
    }
    for (const auto& snapshot : m_pManager->snapshots()) {
        if (snapshot.request.sourceFilePath ==
                oldSourceFilePath) {
            m_pManager->cancel(snapshot.jobId);
            discardedEntryIds.insert(
                    snapshot.request.cacheEntryId);
        }
    }
    for (const auto& entryId : discardedEntryIds) {
        m_pManager->discardCached(entryId);
    }
}

void StemStatusControlProvider::modelAvailabilityChanged(
        bool available) {
    m_pModelAvailable->setAndConfirm(available ? 1.0 : 0.0);
    if (available) {
        for (const auto& pDeckState : m_decks) {
            if (pDeckState->pendingModel &&
                    !pDeckState->sourceFilePath.isEmpty()) {
                pDeckState->pendingModel = false;
                pDeckState->pError->setAndConfirm(
                        static_cast<double>(ErrorCode::None));
                trigger(pDeckState->pDeck->getGroup(),
                        pDeckState->requestedPriority);
            }
        }
    }
    updateWorkerState();
}

void StemStatusControlProvider::jobChanged(
        const QString& jobId) {
    if (!m_pManager) {
        return;
    }
    const auto snapshot = m_pManager->snapshot(jobId);
    if (!snapshot) {
        return;
    }
    for (const auto& pDeckState : m_decks) {
        if (pDeckState->jobId == jobId ||
                (!pDeckState->sourceFilePath.isEmpty() &&
                        pDeckState->sourceFilePath ==
                                snapshot->request
                                        .sourceFilePath)) {
            pDeckState->jobId = jobId;
            pDeckState->cacheEntryId =
                    snapshot->request.cacheEntryId;
            updateDeckFromSnapshot(
                    pDeckState.get(), *snapshot);
        }
    }
    updateCacheSize();
    refreshProtectedEntryIds();
}

void StemStatusControlProvider::queueChanged(
        int queueSize, int activeJobs) {
    m_pQueueSize->setAndConfirm(queueSize);
    m_pActiveJobs->setAndConfirm(activeJobs);
    refreshProtectedEntryIds();
    updateWorkerState();
}

void StemStatusControlProvider::enabledChanged(double value) {
    if (!m_pManager) {
        return;
    }
    m_pManager->setPaused(value <= 0.0);
    updateWorkerState();
}

void StemStatusControlProvider::updateDeckFromSnapshot(
        DeckState* pDeckState,
        const StemSeparationSnapshot& snapshot) {
    pDeckState->pState->setAndConfirm(
            static_cast<double>(snapshot.state));
    pDeckState->pPercentage->setAndConfirm(
            snapshot.percentage);
    pDeckState->pQueuePosition->setAndConfirm(
            snapshot.queuePosition);
    pDeckState->pError->setAndConfirm(
            errorCodeForMessage(snapshot.error));
    switch (snapshot.state) {
    case StemSeparationState::Ready:
        pDeckState->pCacheStatus->setAndConfirm(
                static_cast<double>(CacheStatus::Ready));
        break;
    case StemSeparationState::Cancelled:
    case StemSeparationState::Failed:
    case StemSeparationState::Unavailable:
        pDeckState->pCacheStatus->setAndConfirm(
                static_cast<double>(CacheStatus::None));
        break;
    default:
        pDeckState->pCacheStatus->setAndConfirm(
                static_cast<double>(CacheStatus::Processing));
        break;
    }
}

void StemStatusControlProvider::resetDeck(
        DeckState* pDeckState, const TrackPointer& pTrack) {
    ++pDeckState->fingerprintGeneration;
    if (pDeckState->pFingerprintCancellation) {
        pDeckState->pFingerprintCancellation->store(
                true, std::memory_order_release);
        pDeckState->pFingerprintCancellation.reset();
    }
    pDeckState->sourceFilePath =
            pTrack ? pTrack->getLocation() : QString{};
    pDeckState->jobId.clear();
    pDeckState->cacheEntryId.clear();
    pDeckState->liveSessionId.clear();
    pDeckState->pLiveSessionTrack.reset();
    pDeckState->pendingModel = false;
    pDeckState->pPercentage->setAndConfirm(0.0);
    pDeckState->pState->setAndConfirm(
            static_cast<double>(modelReady()
                            ? StemSeparationState::Idle
                            : StemSeparationState::Unavailable));
    pDeckState->pQueuePosition->setAndConfirm(-1.0);
    pDeckState->pCacheStatus->setAndConfirm(
            static_cast<double>(CacheStatus::None));
    pDeckState->pLiveReady->setAndConfirm(0.0);
    pDeckState->pError->setAndConfirm(
            static_cast<double>(ErrorCode::None));
    if (!pTrack || !m_pManager) {
        refreshProtectedEntryIds();
        return;
    }
    for (const auto& snapshot : m_pManager->snapshots()) {
        if (snapshot.request.sourceFilePath ==
                pDeckState->sourceFilePath) {
            pDeckState->jobId = snapshot.jobId;
            pDeckState->cacheEntryId =
                    snapshot.request.cacheEntryId;
            updateDeckFromSnapshot(pDeckState, snapshot);
            break;
        }
    }
    refreshProtectedEntryIds();
}

void StemStatusControlProvider::updateWorkerState() {
    double state = 0.0;
    if (!modelReady()) {
        state = m_pModelManager &&
                        m_pModelManager->state() ==
                                StemModelManager::State::Downloading
                ? 4.0
                : 3.0;
    } else if (m_pManager && m_pManager->isPaused()) {
        state = 2.0;
    } else if (m_pManager &&
            m_pManager->activeJobCount() > 0) {
        state = 1.0;
    }
    m_pWorkerState->setAndConfirm(state);
}

void StemStatusControlProvider::updateCacheSize() {
    const QDir cacheDirectory(
            QDir(m_settings.rootDirectoryPath)
                    .filePath(kCacheDirectoryName));
    qint64 total = 0;
    const auto entries = cacheDirectory.entryInfoList(
            {QStringLiteral("*.stem.mp4")}, QDir::Files);
    for (const auto& entry : entries) {
        total += entry.size();
    }
    m_pCacheSizeBytes->setAndConfirm(
            static_cast<double>(total));
}

void StemStatusControlProvider::refreshProtectedEntryIds() {
    QSet<QString> protectedIds;
    for (const auto& pDeckState : m_decks) {
        if (!pDeckState->cacheEntryId.isEmpty()) {
            protectedIds.insert(pDeckState->cacheEntryId);
        }
    }
    if (m_pManager) {
        for (const auto& snapshot : m_pManager->snapshots()) {
            if (snapshot.state != StemSeparationState::Ready &&
                    snapshot.state !=
                            StemSeparationState::Cancelled &&
                    snapshot.state !=
                            StemSeparationState::Failed &&
                    !snapshot.request.cacheEntryId.isEmpty()) {
                protectedIds.insert(
                        snapshot.request.cacheEntryId);
            }
        }
    }
    const QMutexLocker locker(&m_protectedEntryIdsMutex);
    m_protectedEntryIds = std::move(protectedIds);
}

bool StemStatusControlProvider::modelReady() const {
    return !m_settings.modelRequired ||
            (m_pModelManager &&
                    m_pModelManager->isAvailable());
}

double StemStatusControlProvider::errorCodeForMessage(
        const QString& error) {
    return static_cast<double>(error.isEmpty()
                    ? ErrorCode::None
                    : ErrorCode::ProcessingFailure);
}

} // namespace mixxx::stems
