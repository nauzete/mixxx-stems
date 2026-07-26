#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace mixxx::stems {

enum class StemSeparationState {
    Idle = 0,
    Queued = 1,
    Preparing = 2,
    Separating = 3,
    Encoding = 4,
    Validating = 5,
    Ready = 6,
    Cancelled = 7,
    Failed = 8,
    Unavailable = 9,
    Paused = 10,
};

enum class StemSeparationPriority {
    LoadedNotPlaying = 0,
    NextSelected = 1,
    Manual = 2,
    Batch = 3,
};

struct StemSeparationRequest final {
    QString sourceFilePath;
    QString cacheEntryId;
    QString displayName;
    StemSeparationPriority priority = StemSeparationPriority::Manual;
    int maximumRetries = 2;
};

struct StemSeparationSnapshot final {
    QString jobId;
    StemSeparationRequest request;
    StemSeparationState state = StemSeparationState::Idle;
    float percentage = 0.0F;
    int queuePosition = -1;
    int attemptCount = 0;
    QString error;
};

class StemSeparationProcessor {
  public:
    enum class Outcome {
        Completed,
        Cancelled,
        Paused,
        RetryableFailure,
        PermanentFailure,
    };

    struct Result {
        Outcome outcome = Outcome::PermanentFailure;
        QString error;
    };

    struct Callbacks {
        std::function<void(StemSeparationState)> publishState;
        std::function<void(float)> publishProgress;
        std::function<bool()> cancelled;
        std::function<bool()> pauseRequested;
    };

    virtual ~StemSeparationProcessor() = default;
    virtual Result process(const StemSeparationRequest& request,
            const Callbacks& callbacks) = 0;
};

/// Owns a persistent priority queue and runs at most one separation processor.
///
/// Public methods belong to this object's thread. Processor work runs on a
/// dedicated low-priority thread and communicates through queued callbacks.
/// No method is intended for Mixxx's real-time audio thread.
class StemSeparationManager final : public QObject {
    Q_OBJECT
  public:
    explicit StemSeparationManager(
            QString queueFilePath,
            std::shared_ptr<StemSeparationProcessor> pProcessor,
            QObject* pParent = nullptr);
    ~StemSeparationManager() override;

    bool initialize(QString* pErrorMessage = nullptr);
    QString enqueue(const StemSeparationRequest& request,
            QString* pErrorMessage = nullptr);
    bool cancel(const QString& jobId);
    bool setPriority(
            const QString& jobId, StemSeparationPriority priority);
    void setPaused(bool paused);

    bool isPaused() const noexcept;
    int queueSize() const noexcept;
    int activeJobCount() const noexcept;
    std::optional<StemSeparationSnapshot> snapshot(
            const QString& jobId) const;
    QList<StemSeparationSnapshot> snapshots() const;

  signals:
    void jobChanged(const QString& jobId);
    void queueChanged(int queueSize, int activeJobs);
    void workerPausedChanged(bool paused);

  private:
    struct Cancellation final {
        std::atomic_bool cancelled{false};
        std::atomic_bool pauseRequested{false};
    };

    struct JobRecord final {
        StemSeparationSnapshot snapshot;
        qint64 sequence = 0;
        std::shared_ptr<Cancellation> pCancellation;
    };

    bool loadQueue(QString* pErrorMessage);
    bool saveQueue(QString* pErrorMessage = nullptr) const;
    void insertQueued(const QString& jobId);
    void startNext();
    void runJob(const QString& jobId,
            const StemSeparationRequest& request,
            std::shared_ptr<Cancellation> pCancellation);
    void publishState(
            const QString& jobId, StemSeparationState state);
    void publishProgress(const QString& jobId, float progress);
    void workerFinished(const QString& jobId,
            StemSeparationProcessor::Result result);
    void notifyJobAndQueue(const QString& jobId);
    int queuePosition(const QString& jobId) const;
    static bool isTerminal(StemSeparationState state);

    QString m_queueFilePath;
    std::shared_ptr<StemSeparationProcessor> m_pProcessor;
    QThreadPool m_workerPool;
    QHash<QString, JobRecord> m_jobs;
    QList<QString> m_queue;
    QString m_activeJobId;
    qint64 m_nextSequence = 0;
    bool m_initialized = false;
    bool m_paused = false;
    bool m_shuttingDown = false;
};

} // namespace mixxx::stems
