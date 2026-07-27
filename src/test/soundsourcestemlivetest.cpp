#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <array>

#include "sources/soundsourcestemlive.h"
#include "stems/stemlivesessionregistry.h"
#include "stems/stemtemporarystore.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace mixxx {
namespace {

class SoundSourceStemLiveTest
        : public MixxxTest,
          ::SoundSourceProviderRegistration {
};

TEST_F(SoundSourceStemLiveTest,
        ReadsPublishedPcmAsEightChannels) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto pTrack = Track::newTemporary(
            getTestDir().filePath(
                    QStringLiteral("sine-30.wav")));
    auto pLiveSession =
            stems::StemLiveSessionRegistry::acquire(
                    directory.path(), pTrack);
    ASSERT_TRUE(pLiveSession);
    const auto pSecondTrack = Track::newTemporary(
            pTrack->getLocation());
    auto pSecondLiveSession =
            stems::StemLiveSessionRegistry::acquire(
                    directory.path(), pSecondTrack);
    ASSERT_EQ(pSecondLiveSession, pLiveSession);
    EXPECT_TRUE(pTrack->hasStem());
    EXPECT_TRUE(pSecondTrack->hasStem());

    stems::StemTemporaryStore store(
            directory.path(), 4096);
    QString error;
    ASSERT_TRUE(store.initialize(
            pLiveSession->id(), 1, &error))
            << error.toStdString();
    const std::array<float, 8> planar = {
            -1.0F,
            -0.75F,
            -0.5F,
            -0.25F,
            0.0F,
            0.25F,
            0.5F,
            1.0F,
    };
    ASSERT_TRUE(store.append(0, 1, planar, &error))
            << error.toStdString();
    pLiveSession->publishTemporarySession(
            store.session());

    SoundSourceStemLive source(pLiveSession->url());
    ASSERT_EQ(source.open(SoundSource::OpenMode::Strict),
            SoundSource::OpenResult::Succeeded);
    ASSERT_EQ(source.getSignalInfo().getChannelCount(),
            audio::ChannelCount::stem());
    std::array<CSAMPLE, 8> output{};
    const auto read = source.readSampleFrames(
            WritableSampleFrames(
                    IndexRange::forward(
                            source.frameIndexMin(), 1),
                    SampleBuffer::WritableSlice(
                            output.data(), output.size())));
    ASSERT_EQ(read.readableLength(), 8);
    for (std::size_t index = 0;
            index < output.size();
            ++index) {
        EXPECT_NEAR(output[index],
                planar[index],
                1.0F / 32767.0F);
    }
    source.close();
    stems::StemLiveSessionRegistry::release(
            pSecondLiveSession->id(), pSecondTrack);
    pSecondLiveSession.reset();
    EXPECT_TRUE(pTrack->hasStem());
    EXPECT_FALSE(pSecondTrack->hasStem());
    stems::StemLiveSessionRegistry::release(
            pLiveSession->id(), pTrack);
    pLiveSession.reset();
    EXPECT_FALSE(pTrack->hasStem());
    ASSERT_TRUE(store.remove(&error))
            << error.toStdString();
}

TEST_F(SoundSourceStemLiveTest,
        ReplacesFallbackWithoutReopeningTheSource) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto pTrack = Track::newTemporary(
            getTestDir().filePath(
                    QStringLiteral("sine-30.wav")));
    auto pLiveSession =
            stems::StemLiveSessionRegistry::acquire(
                    directory.path(), pTrack);
    ASSERT_TRUE(pLiveSession);

    SoundSourceStemLive source(pLiveSession->url());
    ASSERT_EQ(source.open(SoundSource::OpenMode::Strict),
            SoundSource::OpenResult::Succeeded);
    std::array<CSAMPLE, 8> fallback{};
    ASSERT_EQ(source.readSampleFrames(
                            WritableSampleFrames(
                                    IndexRange::forward(
                                            source.frameIndexMin(),
                                            1),
                                    SampleBuffer::WritableSlice(
                                            fallback.data(),
                                            fallback.size())))
                      .readableLength(),
            8);

    stems::StemTemporaryStore store(
            directory.path(), 4096);
    QString error;
    ASSERT_TRUE(store.initialize(
            pLiveSession->id(), 1, &error));
    const std::array<float, 8> separated = {
            -0.8F,
            -0.6F,
            -0.4F,
            -0.2F,
            0.2F,
            0.4F,
            0.6F,
            0.8F,
    };
    ASSERT_TRUE(store.append(
            0, 1, separated, &error));
    pLiveSession->publishTemporarySession(
            store.session());

    std::array<CSAMPLE, 8> hotOutput{};
    ASSERT_EQ(source.readSampleFrames(
                            WritableSampleFrames(
                                    IndexRange::forward(
                                            source.frameIndexMin(),
                                            1),
                                    SampleBuffer::WritableSlice(
                                            hotOutput.data(),
                                            hotOutput.size())))
                      .readableLength(),
            8);
    for (std::size_t index = 0;
            index < hotOutput.size();
            ++index) {
        EXPECT_NEAR(hotOutput[index],
                separated[index],
                1.0F / 32767.0F);
    }
    EXPECT_NE(fallback, hotOutput);

    source.close();
    stems::StemLiveSessionRegistry::release(
            pLiveSession->id(), pTrack);
    pLiveSession.reset();
    ASSERT_TRUE(store.remove(&error));
}

TEST_F(SoundSourceStemLiveTest,
        RemovesCrashLeftoversButPreservesActiveSessions) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto stalePath = directory.filePath(
            QString(64, QChar('d')) +
            QStringLiteral(".stemlive.pcm"));
    QFile staleFile(stalePath);
    ASSERT_TRUE(staleFile.open(QIODevice::WriteOnly));
    staleFile.write("stale");
    staleFile.close();

    const auto pTrack = Track::newTemporary(
            getTestDir().filePath(
                    QStringLiteral("sine-30.wav")));
    auto pLiveSession =
            stems::StemLiveSessionRegistry::acquire(
                    directory.path(), pTrack);
    ASSERT_TRUE(pLiveSession);
    const auto activePath = directory.filePath(
            pLiveSession->id() +
            QStringLiteral(".stemlive.pcm"));
    QFile activeFile(activePath);
    ASSERT_TRUE(activeFile.open(QIODevice::WriteOnly));
    activeFile.write("active");
    activeFile.close();

    stems::StemLiveSessionRegistry::removeStaleFiles(
            directory.path());
    EXPECT_FALSE(QFile::exists(stalePath));
    EXPECT_TRUE(QFile::exists(activePath));

    stems::StemLiveSessionRegistry::release(
            pLiveSession->id(), pTrack);
    pLiveSession.reset();
    stems::StemLiveSessionRegistry::removeStaleFiles(
            directory.path());
    EXPECT_FALSE(QFile::exists(activePath));
}

} // namespace
} // namespace mixxx
