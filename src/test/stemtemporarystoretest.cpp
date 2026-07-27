#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <cmath>

#include "stems/stemtemporarystore.h"

namespace mixxx::stems {
namespace {

TEST(StemTemporaryStoreTest,
        PublishesOnlyFlushedSequentialFramesAndRemovesFile) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StemTemporaryStore store(directory.path(), 4096);
    const auto sessionId = QString(64, QChar('a'));
    QString error;
    ASSERT_TRUE(store.initialize(sessionId, 2, &error))
            << error.toStdString();
    const auto pSession = store.session();
    ASSERT_TRUE(pSession);
    EXPECT_EQ(pSession->readyFrameCount.load(), 0U);

    const std::array<float, 16> planar = {
            -1.0F,
            1.0F,
            -0.75F,
            0.75F,
            -0.5F,
            0.5F,
            -0.25F,
            0.25F,
            0.0F,
            0.1F,
            0.2F,
            0.3F,
            0.4F,
            0.5F,
            0.6F,
            0.7F,
    };
    ASSERT_TRUE(store.append(0, 2, planar, &error))
            << error.toStdString();
    EXPECT_EQ(pSession->readyFrameCount.load(), 2U);
    ASSERT_TRUE(store.finish(&error)) << error.toStdString();
    EXPECT_TRUE(pSession->complete.load());

    QFile file(pSession->filePath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    ASSERT_EQ(bytes.size(), 32);
    const auto* pcm =
            reinterpret_cast<const uchar*>(bytes.constData());
    EXPECT_EQ(qFromLittleEndian<qint16>(pcm), -32767);
    EXPECT_EQ(qFromLittleEndian<qint16>(
                      pcm + sizeof(qint16)),
            static_cast<qint16>(
                    std::lround(-0.75F * 32767.0F)));
    EXPECT_EQ(qFromLittleEndian<qint16>(
                      pcm + 8 * sizeof(qint16)),
            32767);
    file.close();

    ASSERT_TRUE(store.remove(&error)) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(pSession->filePath));
}

TEST(StemTemporaryStoreTest, RejectsOversizedOrNonSequentialData) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QString error;
    StemTemporaryStore oversized(directory.path(), 15);
    EXPECT_FALSE(oversized.initialize(
            QString(64, QChar('b')), 1, &error));

    StemTemporaryStore store(directory.path(), 4096);
    ASSERT_TRUE(store.initialize(
            QString(64, QChar('c')), 2, &error));
    const std::array<float, 8> oneFrame{};
    EXPECT_FALSE(store.append(1, 1, oneFrame, &error));
}

} // namespace
} // namespace mixxx::stems
