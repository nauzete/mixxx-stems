#include "stems/stemcache.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace mixxx::stems {
namespace {

StemCache::Limits testLimits(
        qint64 quotaBytes = 1024,
        qint64 maximumTrackBytes = 512) {
    return {
            quotaBytes,
            0,
            maximumTrackBytes,
    };
}

StemCacheKey makeKey(char discriminator) {
    return {
            QByteArray(32, discriminator),
            100,
            123456,
            QStringLiteral("htdemucs"),
            QStringLiteral("955717e8"),
            QByteArray(32, 'm'),
            QStringLiteral("aac-lc"),
            128000,
            1,
    };
}

bool createSizedFile(const QString& path, qint64 size) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.resize(size);
}

TEST(StemCacheKeyTest, IncludesSourceModelAndOutputSettings) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath =
            directory.filePath(QStringLiteral("source.raw"));
    QFile source(sourcePath);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write("source audio"), 12);
    source.close();

    QString error;
    const auto base = StemCacheKey::fromSourceFile(sourcePath,
            QStringLiteral("htdemucs"),
            QStringLiteral("955717e8"),
            QByteArray(32, 'm'),
            QStringLiteral("aac-lc"),
            128000,
            1,
            &error);
    ASSERT_TRUE(base.has_value()) << error.toStdString();
    const auto repeated = StemCacheKey::fromSourceFile(sourcePath,
            QStringLiteral("htdemucs"),
            QStringLiteral("955717e8"),
            QByteArray(32, 'm'),
            QStringLiteral("aac-lc"),
            128000,
            1,
            &error);
    ASSERT_TRUE(repeated.has_value()) << error.toStdString();
    EXPECT_EQ(base->id(), repeated->id());

    auto changed = *base;
    changed.bitRatePerStream = 96000;
    EXPECT_NE(base->id(), changed.id());
    changed = *base;
    changed.modelSha256[0] = 'x';
    EXPECT_NE(base->id(), changed.id());
    changed = *base;
    changed.sourceModifiedMilliseconds++;
    EXPECT_NE(base->id(), changed.id());
}

TEST(StemCacheKeyTest, SourceHashingIsCooperativelyCancellable) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sourcePath =
            directory.filePath(QStringLiteral("source.raw"));
    ASSERT_TRUE(createSizedFile(sourcePath, 32));
    QString error;

    const auto key = StemCacheKey::fromSourceFile(sourcePath,
            QStringLiteral("htdemucs"),
            QStringLiteral("955717e8"),
            QByteArray(32, 'm'),
            QStringLiteral("aac-lc"),
            128000,
            1,
            &error,
            [] {
                return true;
            });

    EXPECT_FALSE(key.has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(StemCacheTest, PersistsEntriesAndCleansPartialFiles) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto key = makeKey('a');
    QString error;
    {
        StemCache cache(directory.path(), testLimits());
        ASSERT_TRUE(cache.initialize(&error)) << error.toStdString();
        const auto path = cache.reserve(key, 100, {}, &error);
        ASSERT_TRUE(path.has_value()) << error.toStdString();
        ASSERT_TRUE(createSizedFile(*path, 80));
        ASSERT_TRUE(cache.registerCompleted(key, {}, &error))
                << error.toStdString();
        ASSERT_TRUE(createSizedFile(cache.partialFilePath(makeKey('b').id()), 5));
    }

    StemCache restored(directory.path(), testLimits());
    ASSERT_TRUE(restored.initialize(&error)) << error.toStdString();
    EXPECT_EQ(restored.entryCount(), 1);
    EXPECT_EQ(restored.totalBytes(), 80);
    EXPECT_TRUE(restored.lookup(key, &error).has_value())
            << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
            restored.partialFilePath(makeKey('b').id())));
}

TEST(StemCacheTest, EvictsLeastRecentlyUsedUnprotectedEntry) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StemCache cache(directory.path(), testLimits(25, 20));
    QString error;
    ASSERT_TRUE(cache.initialize(&error)) << error.toStdString();
    const auto first = makeKey('a');
    const auto second = makeKey('b');
    const auto third = makeKey('c');

    for (const auto& key : {first, second}) {
        const auto path = cache.reserve(key, 10, {}, &error);
        ASSERT_TRUE(path.has_value()) << error.toStdString();
        ASSERT_TRUE(createSizedFile(*path, 10));
        ASSERT_TRUE(cache.registerCompleted(key, {}, &error))
                << error.toStdString();
    }
    ASSERT_TRUE(cache.lookup(first, &error).has_value())
            << error.toStdString();

    const auto thirdPath = cache.reserve(third, 10, {}, &error);
    ASSERT_TRUE(thirdPath.has_value()) << error.toStdString();
    EXPECT_TRUE(cache.lookup(first, &error).has_value());
    EXPECT_FALSE(cache.lookup(second, &error).has_value());
}

TEST(StemCacheTest, NeverEvictsProtectedEntries) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StemCache cache(directory.path(), testLimits(20, 20));
    QString error;
    ASSERT_TRUE(cache.initialize(&error)) << error.toStdString();
    const auto first = makeKey('a');
    const auto second = makeKey('b');

    const auto path = cache.reserve(first, 20, {}, &error);
    ASSERT_TRUE(path.has_value()) << error.toStdString();
    ASSERT_TRUE(createSizedFile(*path, 20));
    ASSERT_TRUE(cache.registerCompleted(first, {}, &error))
            << error.toStdString();

    EXPECT_FALSE(cache.reserve(
                              second, 10, QSet<QString>{first.id()}, &error)
                         .has_value());
    EXPECT_TRUE(QFileInfo::exists(cache.filePath(first.id())));
    EXPECT_FALSE(cache.remove(first.id(),
            QSet<QString>{first.id()},
            &error));
}

TEST(StemCacheTest, RecoversUnindexedCompletedEntry) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto key = makeKey('r');
    const auto path = QDir(directory.path())
                              .filePath(key.id() + QStringLiteral(".stem.mp4"));
    ASSERT_TRUE(createSizedFile(path, 17));

    StemCache cache(directory.path(), testLimits());
    QString error;
    ASSERT_TRUE(cache.initialize(&error)) << error.toStdString();
    EXPECT_EQ(cache.entryCount(), 1);
    EXPECT_EQ(cache.totalBytes(), 17);
    EXPECT_TRUE(cache.lookup(key, &error).has_value())
            << error.toStdString();
}

} // namespace
} // namespace mixxx::stems
