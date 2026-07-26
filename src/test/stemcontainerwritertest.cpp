#include <gtest/gtest.h>

#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "sources/soundsourcestem.h"
#include "stems/stemcontainerwriter.h"
#include "track/steminfoimporter.h"

namespace mixxx::stems {
namespace {

std::vector<float> slicePlanes(const std::vector<float>& source,
        std::size_t sourceFrameCount,
        std::size_t offset,
        std::size_t frameCount) {
    constexpr auto planeCount =
            StemContainerWriter::kStemCount *
            StemContainerWriter::kChannelCount;
    std::vector<float> result(planeCount * frameCount);
    for (int plane = 0; plane < planeCount; ++plane) {
        std::copy_n(source.begin() + plane * sourceFrameCount + offset,
                frameCount,
                result.begin() + plane * frameCount);
    }
    return result;
}

TEST(StemContainerWriterTest, WritesValidIncrementalStemMp4) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto outputPath =
            directory.filePath(QStringLiteral("separated.stem.mp4"));
    constexpr std::size_t frameCount = 4103;
    constexpr std::size_t firstChunk = 1500;
    std::vector<float> stems(
            StemContainerWriter::kStemCount *
            StemContainerWriter::kChannelCount * frameCount);
    for (int stem = 0; stem < StemContainerWriter::kStemCount; ++stem) {
        for (int channel = 0;
                channel < StemContainerWriter::kChannelCount;
                ++channel) {
            for (std::size_t frame = 0; frame < frameCount; ++frame) {
                stems[(stem * StemContainerWriter::kChannelCount + channel) *
                                frameCount +
                        frame] = static_cast<float>(0.02 * (stem + 1) *
                        std::sin(2.0 * std::numbers::pi *
                                (110.0 + 20.0 * channel) *
                                static_cast<double>(frame) /
                                StemContainerWriter::kSampleRate));
            }
        }
    }

    StemContainerWriter writer(outputPath);
    QString error;
    const auto first = slicePlanes(stems, frameCount, 0, firstChunk);
    ASSERT_TRUE(writer.open(&error)) << error.toStdString();
    ASSERT_TRUE(writer.write(0, firstChunk, first, &error))
            << error.toStdString();

    const auto secondChunk = slicePlanes(stems,
            frameCount,
            firstChunk,
            frameCount - firstChunk);
    ASSERT_TRUE(writer.write(firstChunk,
            frameCount - firstChunk,
            secondChunk,
            &error))
            << error.toStdString();
    ASSERT_TRUE(writer.finish(&error)) << error.toStdString();

    EXPECT_TRUE(QFileInfo::exists(outputPath));
    EXPECT_FALSE(QFileInfo::exists(writer.partialFilePath()));
    EXPECT_GT(QFileInfo(outputPath).size(), 0);
    EXPECT_TRUE(StemInfoImporter::hasStemAtom(outputPath));
    const auto stemInfos = StemInfoImporter::importStemInfos(outputPath);
    ASSERT_EQ(stemInfos.size(), StemContainerWriter::kStemCount);
    EXPECT_EQ(stemInfos[0].getLabel(), QStringLiteral("Drums"));
    EXPECT_EQ(stemInfos[1].getLabel(), QStringLiteral("Bass"));
    EXPECT_EQ(stemInfos[2].getLabel(), QStringLiteral("Other"));
    EXPECT_EQ(stemInfos[3].getLabel(), QStringLiteral("Vocals"));

    SoundSourceSTEM source(QUrl::fromLocalFile(outputPath));
    AudioSource::OpenParams params;
    params.setChannelCount(audio::ChannelCount::stem());
    EXPECT_EQ(source.open(AudioSource::OpenMode::Strict, params),
            AudioSource::OpenResult::Succeeded);
}

TEST(StemContainerWriterTest, CancellationRemovesPartialOutput) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto outputPath =
            directory.filePath(QStringLiteral("cancelled.stem.mp4"));
    StemContainerWriter writer(outputPath);
    QString error;

    ASSERT_TRUE(writer.open(&error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(writer.partialFilePath()));
    writer.cancel();

    EXPECT_FALSE(QFileInfo::exists(writer.partialFilePath()));
    EXPECT_FALSE(QFileInfo::exists(outputPath));
}

TEST(StemContainerWriterTest, FourMinuteEstimateFitsStorageTarget) {
    constexpr qint64 frameCount =
            4LL * 60LL * StemContainerWriter::kSampleRate;
    EXPECT_LE(StemContainerWriter::estimatedEncodedBytes(frameCount),
            32LL * 1024LL * 1024LL);
}

} // namespace
} // namespace mixxx::stems
