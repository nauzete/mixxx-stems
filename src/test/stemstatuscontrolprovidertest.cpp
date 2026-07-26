#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <algorithm>
#include <functional>

#include "control/controlobject.h"
#include "mixer/basetrackplayer.h"
#include "stems/stemstatuscontrolprovider.h"
#include "test/mixxxtest.h"
#include "track/track.h"

namespace mixxx::stems {
namespace {

class FakeDeck final : public BaseTrackPlayer {
  public:
    explicit FakeDeck(QString group)
            : BaseTrackPlayer(nullptr, std::move(group)) {
    }

    TrackPointer getLoadedTrack() const final {
        return m_pTrack;
    }

    void setupEqControls() final {
    }

    void setAlternateAudioSourceResolver(
            AlternateAudioSourceResolver resolver) final {
        m_resolver = std::move(resolver);
    }

    void slotLoadTrack(TrackPointer pTrack,
            mixxx::StemChannelSelection,
            bool) final {
        const auto pOldTrack = m_pTrack;
        if (pTrack && m_resolver) {
            m_lastAlternateUrl = m_resolver(pTrack);
        }
        m_pTrack = std::move(pTrack);
        emit loadingTrack(m_pTrack, pOldTrack);
        if (m_pTrack) {
            emit newTrackLoaded(m_pTrack);
        }
    }

    void slotCloneFromGroup(const QString&) final {
    }

    void slotCloneDeck() final {
    }

    void slotEjectTrack(double value) final {
        if (value > 0.0) {
            slotLoadTrack({}, {}, false);
        }
    }

  private:
    TrackPointer m_pTrack;
    AlternateAudioSourceResolver m_resolver;
    QUrl m_lastAlternateUrl;
};

class SuccessfulProcessor final
        : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest&,
            const Callbacks& callbacks) final {
        callbacks.publishState(StemSeparationState::Preparing);
        callbacks.publishProgress(0.25F);
        callbacks.publishState(StemSeparationState::Separating);
        callbacks.publishProgress(0.75F);
        callbacks.publishState(StemSeparationState::Encoding);
        callbacks.publishState(StemSeparationState::Validating);
        return {Outcome::Completed, {}};
    }
};

class CancellableProcessor final
        : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest&,
            const Callbacks& callbacks) final {
        callbacks.publishState(StemSeparationState::Separating);
        while (!callbacks.cancelled()) {
            QThread::msleep(1);
        }
        return {Outcome::Cancelled, {}};
    }
};

class FailingProcessor final : public StemSeparationProcessor {
  public:
    Result process(const StemSeparationRequest&,
            const Callbacks&) final {
        return {
                Outcome::PermanentFailure,
                QStringLiteral("Synthetic processing failure"),
        };
    }
};

QString createSourceFile(QTemporaryDir* pDirectory) {
    const auto path =
            pDirectory->filePath(QStringLiteral("source.wav"));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write("test audio"), 10);
    return path;
}

double controlValue(
        const QString& group, const QString& item) {
    return ControlObject::get(ConfigKey(group, item));
}

bool waitUntil(const std::function<bool()>& predicate,
        int timeoutMilliseconds = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() &&
            timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

class StemStatusControlProviderTest : public MixxxTest {
};

TEST_F(StemStatusControlProviderTest,
        PublishesSuccessfulJobTransitions) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath = createSourceFile(&directory);
    auto pProcessor = std::make_shared<SuccessfulProcessor>();
    StemStatusControlProvider provider(
            {
                    directory.filePath(QStringLiteral("service")),
                    {},
                    2,
                    128000,
                    false,
            },
            pProcessor);
    ASSERT_TRUE(provider.initialize());
    FakeDeck deck(QStringLiteral("[Channel71]"));
    provider.registerDeck(&deck);
    deck.slotLoadTrack(
            Track::newTemporary(sourcePath), {}, false);

    EXPECT_TRUE(ControlObject::exists(ConfigKey(
            deck.getGroup(), QStringLiteral("separation_trigger"))));
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("separation_state")),
            static_cast<double>(StemSeparationState::Idle));
    EXPECT_DOUBLE_EQ(controlValue(QStringLiteral("[StemSeparation]"),
                             QStringLiteral("model_available")),
            1.0);

    QVector<double> observedProgress;
    auto* pPercentage = ControlObject::getControl(ConfigKey(
            deck.getGroup(),
            QStringLiteral("separation_percentage")));
    ASSERT_NE(pPercentage, nullptr);
    QObject::connect(pPercentage,
            &ControlObject::valueChanged,
            &provider,
            [&](double value) {
                observedProgress.push_back(value);
            });
    ControlObject::set(ConfigKey(deck.getGroup(),
                               QStringLiteral("separation_trigger")),
            1.0);

    ASSERT_TRUE(waitUntil([&] {
        return controlValue(deck.getGroup(),
                       QStringLiteral("separation_state")) ==
                static_cast<double>(StemSeparationState::Ready);
    }));
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("separation_percentage")),
            100.0);
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("stem_cache_status")),
            1.0);
    EXPECT_DOUBLE_EQ(controlValue(QStringLiteral("[StemSeparation]"),
                             QStringLiteral("queue_size")),
            0.0);
    ASSERT_FALSE(observedProgress.isEmpty());
    EXPECT_TRUE(std::is_sorted(
            observedProgress.cbegin(), observedProgress.cend()));
}

TEST_F(StemStatusControlProviderTest,
        PublishesPermanentFailureAndErrorCode) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath = createSourceFile(&directory);
    auto pProcessor = std::make_shared<FailingProcessor>();
    StemStatusControlProvider provider(
            {
                    directory.filePath(QStringLiteral("service")),
                    {},
                    2,
                    128000,
                    false,
            },
            pProcessor);
    ASSERT_TRUE(provider.initialize());
    FakeDeck deck(QStringLiteral("[Channel73]"));
    provider.registerDeck(&deck);
    deck.slotLoadTrack(
            Track::newTemporary(sourcePath), {}, false);

    ControlObject::set(ConfigKey(deck.getGroup(),
                               QStringLiteral("separation_trigger")),
            1.0);

    ASSERT_TRUE(waitUntil([&] {
        return controlValue(deck.getGroup(),
                       QStringLiteral("separation_state")) ==
                static_cast<double>(StemSeparationState::Failed);
    }));
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("separation_error")),
            4.0);
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("stem_cache_status")),
            0.0);
}

TEST_F(StemStatusControlProviderTest,
        CancelsActiveJobAndResetsOnTrackChange) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath = createSourceFile(&directory);
    auto pProcessor = std::make_shared<CancellableProcessor>();
    StemStatusControlProvider provider(
            {
                    directory.filePath(QStringLiteral("service")),
                    {},
                    2,
                    128000,
                    false,
            },
            pProcessor);
    ASSERT_TRUE(provider.initialize());
    FakeDeck deck(QStringLiteral("[Channel72]"));
    provider.registerDeck(&deck);
    deck.slotLoadTrack(
            Track::newTemporary(sourcePath), {}, false);

    ControlObject::set(ConfigKey(deck.getGroup(),
                               QStringLiteral("separation_trigger")),
            1.0);
    ASSERT_TRUE(waitUntil([&] {
        return controlValue(deck.getGroup(),
                       QStringLiteral("separation_state")) ==
                static_cast<double>(
                        StemSeparationState::Separating);
    }));
    ControlObject::set(ConfigKey(deck.getGroup(),
                               QStringLiteral("separation_cancel")),
            1.0);
    ASSERT_TRUE(waitUntil([&] {
        return controlValue(deck.getGroup(),
                       QStringLiteral("separation_state")) ==
                static_cast<double>(
                        StemSeparationState::Cancelled);
    }));

    const auto secondSourcePath =
            directory.filePath(QStringLiteral("second.wav"));
    QFile secondSource(secondSourcePath);
    ASSERT_TRUE(secondSource.open(QIODevice::WriteOnly));
    ASSERT_EQ(secondSource.write("second"), 6);
    secondSource.close();
    deck.slotLoadTrack(
            Track::newTemporary(secondSourcePath), {}, false);

    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("separation_state")),
            static_cast<double>(StemSeparationState::Idle));
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("separation_percentage")),
            0.0);
    EXPECT_DOUBLE_EQ(controlValue(deck.getGroup(),
                             QStringLiteral("separation_queue_position")),
            -1.0);
}

} // namespace
} // namespace mixxx::stems
