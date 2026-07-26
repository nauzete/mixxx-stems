#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

#include "stems/stemseparationmanager.h"

namespace mixxx::stems {
namespace {

bool waitUntil(const std::function<bool()>& predicate,
        int timeoutMilliseconds = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return predicate();
}

StemSeparationRequest request(
        const QString& name,
        StemSeparationPriority priority) {
    return {
            QStringLiteral("/music/") + name + QStringLiteral(".wav"),
            name.repeated(64).left(64),
            name,
            priority,
            2,
    };
}

class RecordingProcessor final : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest& request,
            const Callbacks& callbacks) override {
        callbacks.publishState(StemSeparationState::Separating);
        callbacks.publishProgress(0.5F);
        callbacks.publishProgress(0.4F);
        callbacks.publishProgress(0.8F);
        {
            QMutexLocker lock(&m_mutex);
            m_processed.push_back(request.displayName);
        }
        return {Outcome::Completed, {}};
    }

    QList<QString> processed() const {
        QMutexLocker lock(&m_mutex);
        return m_processed;
    }

  private:
    mutable QMutex m_mutex;
    QList<QString> m_processed;
};

class CancellableProcessor final : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest&,
            const Callbacks& callbacks) override {
        m_started.store(true, std::memory_order_release);
        while (!callbacks.cancelled() &&
                !callbacks.pauseRequested()) {
            QThread::msleep(1);
        }
        return callbacks.pauseRequested()
                ? Result{Outcome::Paused, {}}
                : Result{Outcome::Cancelled, {}};
    }

    std::atomic_bool m_started{false};
};

class RetryingProcessor final : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest&,
            const Callbacks&) override {
        const auto attempt =
                m_attempts.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (attempt < 3) {
            return {
                    Outcome::RetryableFailure,
                    QStringLiteral("temporary"),
            };
        }
        return {Outcome::Completed, {}};
    }

    std::atomic_int m_attempts{0};
};

class FailingProcessor final : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest&,
            const Callbacks&) override {
        return {
                Outcome::PermanentFailure,
                QStringLiteral("model unavailable"),
        };
    }
};

TEST(StemSeparationManagerTest, RunsPriorityThenFifoOnOneWorker) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto processor = std::make_shared<RecordingProcessor>();
    StemSeparationManager manager(
            directory.filePath(QStringLiteral("queue.json")),
            processor);
    manager.setPaused(true);
    QString error;
    ASSERT_TRUE(manager.initialize(&error)) << error.toStdString();

    const auto batch = manager.enqueue(
            request(QStringLiteral("b"), StemSeparationPriority::Batch),
            &error);
    const auto manual = manager.enqueue(
            request(QStringLiteral("m"), StemSeparationPriority::Manual),
            &error);
    const auto next = manager.enqueue(
            request(QStringLiteral("n"), StemSeparationPriority::NextSelected),
            &error);
    ASSERT_FALSE(batch.isEmpty());
    ASSERT_FALSE(manual.isEmpty());
    ASSERT_FALSE(next.isEmpty());
    manager.setPaused(false);

    ASSERT_TRUE(waitUntil([&] {
        return manager.activeJobCount() == 0 &&
                processor->processed().size() == 3;
    }));
    EXPECT_EQ(processor->processed(),
            (QList<QString>{
                    QStringLiteral("n"),
                    QStringLiteral("m"),
                    QStringLiteral("b"),
            }));
    EXPECT_EQ(manager.snapshot(next)->state,
            StemSeparationState::Ready);
    EXPECT_FLOAT_EQ(manager.snapshot(next)->percentage, 100.0F);
}

TEST(StemSeparationManagerTest, CancelsActiveJobCooperatively) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto processor = std::make_shared<CancellableProcessor>();
    StemSeparationManager manager(
            directory.filePath(QStringLiteral("queue.json")),
            processor);
    QString error;
    ASSERT_TRUE(manager.initialize(&error)) << error.toStdString();
    const auto jobId = manager.enqueue(
            request(QStringLiteral("c"), StemSeparationPriority::Manual),
            &error);
    ASSERT_TRUE(waitUntil([&] {
        return processor->m_started.load(std::memory_order_acquire);
    }));

    ASSERT_TRUE(manager.cancel(jobId));
    ASSERT_TRUE(waitUntil([&] {
        return manager.activeJobCount() == 0;
    }));
    EXPECT_EQ(manager.snapshot(jobId)->state,
            StemSeparationState::Cancelled);
}

TEST(StemSeparationManagerTest, PausesAndResumesActiveJob) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto processor = std::make_shared<CancellableProcessor>();
    StemSeparationManager manager(
            directory.filePath(QStringLiteral("queue.json")),
            processor);
    QString error;
    ASSERT_TRUE(manager.initialize(&error)) << error.toStdString();
    const auto jobId = manager.enqueue(
            request(QStringLiteral("p"), StemSeparationPriority::Manual),
            &error);
    ASSERT_TRUE(waitUntil([&] {
        return processor->m_started.load(std::memory_order_acquire);
    }));

    manager.setPaused(true);
    ASSERT_TRUE(waitUntil([&] {
        return manager.activeJobCount() == 0;
    }));
    EXPECT_EQ(manager.snapshot(jobId)->state,
            StemSeparationState::Paused);
    manager.cancel(jobId);
}

TEST(StemSeparationManagerTest, RetriesOnlyToConfiguredLimit) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto processor = std::make_shared<RetryingProcessor>();
    StemSeparationManager manager(
            directory.filePath(QStringLiteral("queue.json")),
            processor);
    QString error;
    ASSERT_TRUE(manager.initialize(&error)) << error.toStdString();
    const auto jobId = manager.enqueue(
            request(QStringLiteral("r"), StemSeparationPriority::Manual),
            &error);

    ASSERT_TRUE(waitUntil([&] {
        const auto value = manager.snapshot(jobId);
        return value &&
                value->state == StemSeparationState::Ready;
    }));
    EXPECT_EQ(processor->m_attempts.load(), 3);
    EXPECT_EQ(manager.snapshot(jobId)->attemptCount, 3);
}

TEST(StemSeparationManagerTest, PublishesPermanentError) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto processor = std::make_shared<FailingProcessor>();
    StemSeparationManager manager(
            directory.filePath(QStringLiteral("queue.json")),
            processor);
    QString error;
    ASSERT_TRUE(manager.initialize(&error)) << error.toStdString();
    const auto jobId = manager.enqueue(
            request(QStringLiteral("f"), StemSeparationPriority::Manual),
            &error);

    ASSERT_TRUE(waitUntil([&] {
        const auto value = manager.snapshot(jobId);
        return value &&
                value->state == StemSeparationState::Failed;
    }));
    EXPECT_EQ(manager.snapshot(jobId)->error,
            QStringLiteral("model unavailable"));
}

TEST(StemSeparationManagerTest, RestoresPersistedQueueAfterRestart) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto queuePath =
            directory.filePath(QStringLiteral("queue.json"));
    QString error;
    QString jobId;
    {
        auto processor = std::make_shared<RecordingProcessor>();
        StemSeparationManager manager(queuePath, processor);
        manager.setPaused(true);
        ASSERT_TRUE(manager.initialize(&error)) << error.toStdString();
        jobId = manager.enqueue(
                request(QStringLiteral("q"), StemSeparationPriority::Batch),
                &error);
        ASSERT_FALSE(jobId.isEmpty());
    }

    auto processor = std::make_shared<RecordingProcessor>();
    StemSeparationManager restored(queuePath, processor);
    ASSERT_TRUE(restored.initialize(&error)) << error.toStdString();
    ASSERT_TRUE(waitUntil([&] {
        const auto value = restored.snapshot(jobId);
        return value &&
                value->state == StemSeparationState::Ready;
    }));
    EXPECT_EQ(processor->processed(),
            QList<QString>{QStringLiteral("q")});
}

} // namespace
} // namespace mixxx::stems
