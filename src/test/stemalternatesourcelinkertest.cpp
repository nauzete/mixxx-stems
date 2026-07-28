#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "stems/stemalternatesourcelinker.h"

namespace mixxx::stems {
namespace {

const QString kEntryId(64, QChar('a'));

void writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(contents), contents.size());
}

TEST(StemAlternateSourceLinkerTest,
        PersistsCompletedRepresentationForOriginalSource) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath =
            QDir(directory.path()).filePath(QStringLiteral("song.wav"));
    const auto outputPath = QDir(directory.path())
                                    .filePath(kEntryId +
                                            QStringLiteral(".stem.mp4"));
    writeFile(sourcePath, QByteArray("source"));
    writeFile(outputPath, QByteArray("stem"));

    {
        StemAlternateSourceLinker linker(directory.path());
        ASSERT_TRUE(linker.initialize());
        ASSERT_TRUE(linker.registerCompleted(sourcePath, kEntryId));
    }

    StemAlternateSourceLinker restored(directory.path());
    ASSERT_TRUE(restored.initialize());
    const auto resolution = restored.resolve(sourcePath);
    ASSERT_TRUE(resolution);
    EXPECT_EQ(resolution->entryId, kEntryId);
    EXPECT_EQ(resolution->filePath, outputPath);
}

TEST(StemAlternateSourceLinkerTest,
        InvalidatesLinkWhenOriginalSourceChanges) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath =
            QDir(directory.path()).filePath(QStringLiteral("song.wav"));
    const auto outputPath = QDir(directory.path())
                                    .filePath(kEntryId +
                                            QStringLiteral(".stem.mp4"));
    writeFile(sourcePath, QByteArray("source"));
    writeFile(outputPath, QByteArray("stem"));

    StemAlternateSourceLinker linker(directory.path());
    ASSERT_TRUE(linker.initialize());
    ASSERT_TRUE(linker.registerCompleted(sourcePath, kEntryId));

    writeFile(sourcePath, QByteArray("changed source"));
    EXPECT_FALSE(linker.resolve(sourcePath));

    StemAlternateSourceLinker restored(directory.path());
    ASSERT_TRUE(restored.initialize());
    EXPECT_FALSE(restored.resolve(sourcePath));
}

TEST(StemAlternateSourceLinkerTest,
        InvalidatesLinkWhenRepresentationIsEvicted) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath =
            QDir(directory.path()).filePath(QStringLiteral("song.wav"));
    const auto outputPath = QDir(directory.path())
                                    .filePath(kEntryId +
                                            QStringLiteral(".stem.mp4"));
    writeFile(sourcePath, QByteArray("source"));
    writeFile(outputPath, QByteArray("stem"));

    StemAlternateSourceLinker linker(directory.path());
    ASSERT_TRUE(linker.initialize());
    ASSERT_TRUE(linker.registerCompleted(sourcePath, kEntryId));
    ASSERT_TRUE(QFile::remove(outputPath));

    EXPECT_FALSE(linker.resolve(sourcePath));
}

} // namespace
} // namespace mixxx::stems
