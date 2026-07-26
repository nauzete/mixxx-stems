#include "stems/stemseparationmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRunnable>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <exception>
#include <utility>

#include "moc_stemseparationmanager.cpp"
#include "util/logger.h"

namespace mixxx::stems {
namespace {

const Logger kLogger("StemSeparationManager");

bool fail(QString* pErrorMessage, QString message) {
    kLogger.warning().noquote() << message;
    if (pErrorMessage) {
        *pErrorMessage = std::move(message);
    }
    return false;
}

bool validPriority(int priority) {
    return priority >=
            static_cast<int>(
                    StemSeparationPriority::LoadedNotPlaying) &&
            priority <=
            static_cast<int>(StemSeparationPriority::Batch);
}

} // namespace

StemSeparationManager::StemSeparationManager(
        QString queueFilePath,
        std::shared_ptr<StemSeparationProcessor> pProcessor,
        QObject* pParent)
        : QObject(pParent),
          m_queueFilePath(QDir::cleanPath(std::move(queueFilePath))),
          m_pProcessor(std::move(pProcessor)) {
    m_workerPool.setMaxThreadCount(1);
    m_workerPool.setThreadPriority(QThread::LowPriority);
    m_workerPool.setExpiryTimeout(-1);
}

StemSeparationManager::~StemSeparationManager() {
    m_shuttingDown = true;
    for (auto& job : m_jobs) {
        if (job.pCancellation) {
            job.pCancellation->cancelled.store(
                    true, std::memory_order_release);
        }
    }
    saveQueue();
    m_workerPool.clear();
    m_workerPool.waitForDone();
}

bool StemSeparationManager::initialize(QString* pErrorMessage) {
    if (m_initialized) {
        return fail(pErrorMessage,
                QStringLiteral("Stem separation queue is already initialized"));
    }
    if (!m_pProcessor || m_queueFilePath.isEmpty() ||
            !QDir().mkpath(QFileInfo(m_queueFilePath).absolutePath())) {
        return fail(pErrorMessage,
                QStringLiteral("Invalid stem separation queue configuration"));
    }
    m_initialized = true;
    if (!loadQueue(pErrorMessage) ||
            !saveQueue(pErrorMessage)) {
        m_initialized = false;
        return false;
    }
    startNext();
    return true;
}

QString StemSeparationManager::enqueue(
        const StemSeparationRequest& request,
        QString* pErrorMessage) {
    if (!m_initialized || request.sourceFilePath.isEmpty() ||
            request.cacheEntryId.isEmpty() ||
            !validPriority(static_cast<int>(request.priority)) ||
            request.maximumRetries < 0 ||
            request.maximumRetries > 5) {
        fail(pErrorMessage,
                QStringLiteral("Invalid stem separation request"));
        return {};
    }
    for (auto job = m_jobs.cbegin(); job != m_jobs.cend(); ++job) {
        if (job->snapshot.request.cacheEntryId ==
                        request.cacheEntryId &&
                !isTerminal(job->snapshot.state)) {
            return job.key();
        }
    }

    const auto jobId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    JobRecord job;
    job.snapshot.jobId = jobId;
    job.snapshot.request = request;
    job.snapshot.state = m_paused
            ? StemSeparationState::Paused
            : StemSeparationState::Queued;
    job.sequence = m_nextSequence++;
    job.pCancellation = std::make_shared<Cancellation>();
    m_jobs.insert(jobId, std::move(job));
    if (!m_paused) {
        insertQueued(jobId);
    }
    if (!saveQueue(pErrorMessage)) {
        m_queue.removeAll(jobId);
        m_jobs.remove(jobId);
        return {};
    }
    notifyJobAndQueue(jobId);
    startNext();
    return jobId;
}

bool StemSeparationManager::cancel(const QString& jobId) {
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || isTerminal(job->snapshot.state)) {
        return false;
    }
    job->pCancellation->pauseRequested.store(
            false, std::memory_order_release);
    job->pCancellation->cancelled.store(
            true, std::memory_order_release);
    if (jobId != m_activeJobId) {
        m_queue.removeAll(jobId);
    }
    job->snapshot.state = StemSeparationState::Cancelled;
    job->snapshot.error.clear();
    saveQueue();
    notifyJobAndQueue(jobId);
    if (jobId != m_activeJobId) {
        startNext();
    }
    return true;
}

bool StemSeparationManager::setPriority(
        const QString& jobId, StemSeparationPriority priority) {
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || isTerminal(job->snapshot.state) ||
            !validPriority(static_cast<int>(priority))) {
        return false;
    }
    job->snapshot.request.priority = priority;
    if (job->snapshot.state == StemSeparationState::Queued) {
        m_queue.removeAll(jobId);
        insertQueued(jobId);
    }
    saveQueue();
    notifyJobAndQueue(jobId);
    return true;
}

void StemSeparationManager::setPaused(bool paused) {
    if (m_paused == paused) {
        return;
    }
    m_paused = paused;
    if (paused) {
        const auto queuedJobs =
                std::exchange(m_queue, QList<QString>{});
        for (const auto& jobId : queuedJobs) {
            auto job = m_jobs.find(jobId);
            if (job != m_jobs.end() &&
                    job->snapshot.state ==
                            StemSeparationState::Queued) {
                job->snapshot.state = StemSeparationState::Paused;
                emit jobChanged(jobId);
            }
        }
        auto active = m_jobs.find(m_activeJobId);
        if (active != m_jobs.end()) {
            active->pCancellation->pauseRequested.store(
                    true, std::memory_order_release);
        }
    } else {
        for (auto job = m_jobs.begin(); job != m_jobs.end(); ++job) {
            if (job->snapshot.state ==
                    StemSeparationState::Paused) {
                job->snapshot.state = StemSeparationState::Queued;
                job->pCancellation =
                        std::make_shared<Cancellation>();
                insertQueued(job.key());
                emit jobChanged(job.key());
            }
        }
    }
    saveQueue();
    emit workerPausedChanged(paused);
    emit queueChanged(queueSize(), activeJobCount());
    startNext();
}

bool StemSeparationManager::isPaused() const noexcept {
    return m_paused;
}

int StemSeparationManager::queueSize() const noexcept {
    int count = 0;
    for (const auto& job : m_jobs) {
        if (job.snapshot.state == StemSeparationState::Queued ||
                job.snapshot.state == StemSeparationState::Paused) {
            ++count;
        }
    }
    return count;
}

int StemSeparationManager::activeJobCount() const noexcept {
    return m_activeJobId.isEmpty() ? 0 : 1;
}

std::optional<StemSeparationSnapshot> StemSeparationManager::snapshot(
        const QString& jobId) const {
    const auto job = m_jobs.constFind(jobId);
    if (job == m_jobs.cend()) {
        return std::nullopt;
    }
    auto result = job->snapshot;
    result.queuePosition = queuePosition(jobId);
    return result;
}

QList<StemSeparationSnapshot> StemSeparationManager::snapshots() const {
    QList<StemSeparationSnapshot> result;
    result.reserve(m_jobs.size());
    for (auto job = m_jobs.cbegin(); job != m_jobs.cend(); ++job) {
        auto item = job->snapshot;
        item.queuePosition = queuePosition(job.key());
        result.push_back(std::move(item));
    }
    std::sort(result.begin(),
            result.end(),
            [&](const auto& left, const auto& right) {
                const auto leftJob = m_jobs.constFind(left.jobId);
                const auto rightJob = m_jobs.constFind(right.jobId);
                return leftJob->sequence < rightJob->sequence;
            });
    return result;
}

bool StemSeparationManager::loadQueue(QString* pErrorMessage) {
    QFile file(m_queueFilePath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to open persisted stem separation queue"));
    }
    QJsonParseError parseError;
    const auto document =
            QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !document.isObject() ||
            document.object().value(QStringLiteral("version")).toInt() != 1) {
        kLogger.warning()
                << "Ignoring malformed persisted stem separation queue";
        return true;
    }
    const auto jobs =
            document.object().value(QStringLiteral("jobs")).toArray();
    for (const auto& value : jobs) {
        const auto object = value.toObject();
        const auto jobId =
                object.value(QStringLiteral("job_id")).toString();
        const auto source =
                object.value(QStringLiteral("source")).toString();
        const auto cacheEntryId =
                object.value(QStringLiteral("cache_entry_id")).toString();
        const auto priority =
                object.value(QStringLiteral("priority")).toInt(-1);
        const auto maximumRetries =
                object.value(QStringLiteral("maximum_retries")).toInt(-1);
        const auto attemptCount =
                object.value(QStringLiteral("attempt_count")).toInt(0);
        const auto sequence =
                object.value(QStringLiteral("sequence"))
                        .toString()
                        .toLongLong();
        if (jobId.isEmpty() || source.isEmpty() ||
                cacheEntryId.isEmpty() || !validPriority(priority) ||
                maximumRetries < 0 || maximumRetries > 5 ||
                attemptCount < 0 || m_jobs.contains(jobId)) {
            continue;
        }
        JobRecord job;
        job.snapshot.jobId = jobId;
        job.snapshot.request = {
                source,
                cacheEntryId,
                object.value(QStringLiteral("display_name"))
                        .toString(),
                static_cast<StemSeparationPriority>(priority),
                maximumRetries,
        };
        job.snapshot.state = StemSeparationState::Queued;
        job.snapshot.attemptCount = attemptCount;
        job.sequence = sequence;
        job.pCancellation = std::make_shared<Cancellation>();
        m_nextSequence = std::max(m_nextSequence, sequence + 1);
        m_jobs.insert(jobId, std::move(job));
    }
    for (auto job = m_jobs.cbegin(); job != m_jobs.cend(); ++job) {
        if (job->snapshot.state == StemSeparationState::Queued) {
            insertQueued(job.key());
        }
    }
    return true;
}

bool StemSeparationManager::saveQueue(
        QString* pErrorMessage) const {
    if (!m_initialized) {
        return true;
    }
    QJsonArray jobs;
    for (auto job = m_jobs.cbegin(); job != m_jobs.cend(); ++job) {
        if (isTerminal(job->snapshot.state)) {
            continue;
        }
        jobs.append(QJsonObject{
                {QStringLiteral("job_id"), job.key()},
                {QStringLiteral("source"),
                        job->snapshot.request.sourceFilePath},
                {QStringLiteral("cache_entry_id"),
                        job->snapshot.request.cacheEntryId},
                {QStringLiteral("display_name"),
                        job->snapshot.request.displayName},
                {QStringLiteral("priority"),
                        static_cast<int>(
                                job->snapshot.request.priority)},
                {QStringLiteral("maximum_retries"),
                        job->snapshot.request.maximumRetries},
                {QStringLiteral("attempt_count"),
                        job->snapshot.attemptCount},
                {QStringLiteral("sequence"),
                        QString::number(job->sequence)},
                {QStringLiteral("state"),
                        static_cast<int>(job->snapshot.state)},
        });
    }
    const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("jobs"), jobs},
    };
    QSaveFile file(m_queueFilePath);
    if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) <
                    0 ||
            !file.commit()) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to atomically persist stem separation queue"));
    }
    return true;
}

void StemSeparationManager::insertQueued(const QString& jobId) {
    const auto job = m_jobs.constFind(jobId);
    if (job == m_jobs.cend() || m_queue.contains(jobId)) {
        return;
    }
    const auto position = std::find_if(m_queue.cbegin(),
            m_queue.cend(),
            [&](const QString& queuedId) {
                const auto queued = m_jobs.constFind(queuedId);
                if (queued->snapshot.request.priority !=
                        job->snapshot.request.priority) {
                    return queued->snapshot.request.priority >
                            job->snapshot.request.priority;
                }
                return queued->sequence > job->sequence;
            });
    m_queue.insert(position, jobId);
}

void StemSeparationManager::startNext() {
    if (!m_initialized || m_shuttingDown || m_paused ||
            !m_activeJobId.isEmpty() || m_queue.isEmpty()) {
        return;
    }
    const auto jobId = m_queue.takeFirst();
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() ||
            job->snapshot.state != StemSeparationState::Queued) {
        startNext();
        return;
    }
    m_activeJobId = jobId;
    job->snapshot.state = StemSeparationState::Preparing;
    job->snapshot.percentage = 0.0F;
    job->snapshot.error.clear();
    ++job->snapshot.attemptCount;
    const auto request = job->snapshot.request;
    const auto cancellation = job->pCancellation;
    saveQueue();
    notifyJobAndQueue(jobId);

    m_workerPool.start(QRunnable::create(
            [this, jobId, request, cancellation] {
                runJob(jobId, request, cancellation);
            }));
}

void StemSeparationManager::runJob(const QString& jobId,
        const StemSeparationRequest& request,
        std::shared_ptr<Cancellation> pCancellation) {
    const StemSeparationProcessor::Callbacks callbacks{
            [this, jobId](StemSeparationState state) {
                QMetaObject::invokeMethod(
                        this,
                        [this, jobId, state] { publishState(jobId, state); },
                        Qt::QueuedConnection);
            },
            [this, jobId](float progress) {
                QMetaObject::invokeMethod(
                        this,
                        [this, jobId, progress] {
                            publishProgress(jobId, progress);
                        },
                        Qt::QueuedConnection);
            },
            [pCancellation] {
                return pCancellation->cancelled.load(
                        std::memory_order_acquire);
            },
            [pCancellation] {
                return pCancellation->pauseRequested.load(
                        std::memory_order_acquire);
            },
    };
    StemSeparationProcessor::Result result;
    try {
        result = m_pProcessor->process(request, callbacks);
    } catch (const std::exception& exception) {
        result = {
                StemSeparationProcessor::Outcome::PermanentFailure,
                QStringLiteral("Stem processor exception: %1")
                        .arg(QString::fromUtf8(exception.what())),
        };
    } catch (...) {
        result = {
                StemSeparationProcessor::Outcome::PermanentFailure,
                QStringLiteral("Unknown stem processor exception"),
        };
    }
    QMetaObject::invokeMethod(
            this,
            [this, jobId, result = std::move(result)]() mutable {
                workerFinished(jobId, std::move(result));
            },
            Qt::QueuedConnection);
}

void StemSeparationManager::publishState(
        const QString& jobId, StemSeparationState state) {
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || jobId != m_activeJobId ||
            isTerminal(job->snapshot.state)) {
        return;
    }
    if (state != StemSeparationState::Preparing &&
            state != StemSeparationState::Separating &&
            state != StemSeparationState::Encoding &&
            state != StemSeparationState::Validating) {
        return;
    }
    if (static_cast<int>(state) <
            static_cast<int>(job->snapshot.state)) {
        return;
    }
    job->snapshot.state = state;
    saveQueue();
    emit jobChanged(jobId);
}

void StemSeparationManager::publishProgress(
        const QString& jobId, float progress) {
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || jobId != m_activeJobId ||
            isTerminal(job->snapshot.state)) {
        return;
    }
    const auto percentage =
            std::clamp(progress * 100.0F, 0.0F, 100.0F);
    if (percentage < job->snapshot.percentage) {
        return;
    }
    job->snapshot.percentage = percentage;
    emit jobChanged(jobId);
}

void StemSeparationManager::workerFinished(const QString& jobId,
        StemSeparationProcessor::Result result) {
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || jobId != m_activeJobId) {
        return;
    }
    m_activeJobId.clear();
    const auto cancelled = job->pCancellation->cancelled.load(
            std::memory_order_acquire);
    const auto pauseRequested =
            job->pCancellation->pauseRequested.load(
                    std::memory_order_acquire);
    if (cancelled ||
            result.outcome ==
                    StemSeparationProcessor::Outcome::Cancelled) {
        job->snapshot.state = StemSeparationState::Cancelled;
        job->snapshot.error.clear();
    } else if (m_paused || pauseRequested ||
            result.outcome ==
                    StemSeparationProcessor::Outcome::Paused) {
        job->snapshot.state = StemSeparationState::Paused;
        job->snapshot.error.clear();
        job->pCancellation = std::make_shared<Cancellation>();
    } else if (result.outcome ==
            StemSeparationProcessor::Outcome::Completed) {
        job->snapshot.state = StemSeparationState::Ready;
        job->snapshot.percentage = 100.0F;
        job->snapshot.error.clear();
    } else if (result.outcome ==
                    StemSeparationProcessor::Outcome::RetryableFailure &&
            job->snapshot.attemptCount <=
                    job->snapshot.request.maximumRetries) {
        job->snapshot.state = StemSeparationState::Queued;
        job->snapshot.percentage = 0.0F;
        job->snapshot.error = result.error;
        job->sequence = m_nextSequence++;
        job->pCancellation = std::make_shared<Cancellation>();
        insertQueued(jobId);
    } else {
        job->snapshot.state = StemSeparationState::Failed;
        job->snapshot.error = result.error.isEmpty()
                ? QStringLiteral("Stem separation failed")
                : std::move(result.error);
    }
    saveQueue();
    notifyJobAndQueue(jobId);
    startNext();
}

void StemSeparationManager::notifyJobAndQueue(
        const QString& jobId) {
    emit jobChanged(jobId);
    emit queueChanged(queueSize(), activeJobCount());
}

int StemSeparationManager::queuePosition(
        const QString& jobId) const {
    if (jobId == m_activeJobId) {
        return 0;
    }
    const auto position = m_queue.indexOf(jobId);
    return position < 0 ? -1 : position + 1;
}

// static
bool StemSeparationManager::isTerminal(
        StemSeparationState state) {
    return state == StemSeparationState::Ready ||
            state == StemSeparationState::Cancelled ||
            state == StemSeparationState::Failed ||
            state == StemSeparationState::Unavailable;
}

} // namespace mixxx::stems
