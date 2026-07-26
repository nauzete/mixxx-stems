#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QtTypes>

namespace mixxx::stems {

/// Immutable inputs that determine whether a separated representation can be
/// reused. Creating a key hashes the source file and must run on a background
/// worker.
struct StemCacheKey final {
    QByteArray sourceSha256;
    qint64 sourceSize = 0;
    qint64 sourceModifiedMilliseconds = 0;
    QString modelName;
    QString modelVersion;
    QByteArray modelSha256;
    QString codec = QStringLiteral("aac-lc");
    int bitRatePerStream = 128000;
    int containerVersion = 1;

    QString id() const;

    static std::optional<StemCacheKey> fromSourceFile(
            const QString& sourceFilePath,
            QString modelName,
            QString modelVersion,
            QByteArray modelSha256,
            QString codec,
            int bitRatePerStream,
            int containerVersion,
            QString* pErrorMessage = nullptr,
            const std::function<bool()>& cancelled = {});
};

/// Persistent, quota-bound cache for completed .stem.mp4 representations.
///
/// This class performs hashing, JSON and filesystem I/O. It is intentionally
/// not thread-safe and must be owned by the separation background worker.
class StemCache final {
  public:
    static constexpr qint64 kWindowsDefaultQuotaBytes =
            4LL * 1024LL * 1024LL * 1024LL;
    static constexpr qint64 kArmDefaultQuotaBytes =
            1LL * 1024LL * 1024LL * 1024LL;
    static constexpr qint64 kWindowsDefaultFreeSpaceReserveBytes =
            5LL * 1024LL * 1024LL * 1024LL;
    static constexpr qint64 kArmDefaultFreeSpaceReserveBytes =
            2LL * 1024LL * 1024LL * 1024LL;
    static constexpr qint64 kDefaultMaximumTrackBytes =
            256LL * 1024LL * 1024LL;

    struct Limits {
#if defined(Q_OS_WIN)
        qint64 quotaBytes = kWindowsDefaultQuotaBytes;
        qint64 freeSpaceReserveBytes =
                kWindowsDefaultFreeSpaceReserveBytes;
#else
        qint64 quotaBytes = kArmDefaultQuotaBytes;
        qint64 freeSpaceReserveBytes =
                kArmDefaultFreeSpaceReserveBytes;
#endif
        qint64 maximumTrackBytes = kDefaultMaximumTrackBytes;
    };

    explicit StemCache(QString directoryPath, Limits limits = {});

    bool initialize(QString* pErrorMessage = nullptr);

    std::optional<QString> lookup(
            const StemCacheKey& key,
            QString* pErrorMessage = nullptr);

    /// Reserves quota and returns the exact final path for a new entry.
    ///
    /// protectedEntryIds must include loaded, playing, processing, pinned,
    /// immediate-queue, and open entries. They will never be evicted.
    std::optional<QString> reserve(const StemCacheKey& key,
            qint64 maximumNewEntryBytes,
            const QSet<QString>& protectedEntryIds,
            QString* pErrorMessage = nullptr);

    bool registerCompleted(
            const StemCacheKey& key,
            const QSet<QString>& protectedEntryIds,
            QString* pErrorMessage = nullptr);

    bool remove(const QString& entryId,
            const QSet<QString>& protectedEntryIds,
            QString* pErrorMessage = nullptr);

    qint64 totalBytes() const noexcept;
    qsizetype entryCount() const noexcept;
    QString directoryPath() const;
    QString filePath(const QString& entryId) const;
    QString partialFilePath(const QString& entryId) const;
    Limits limits() const noexcept;

  private:
    struct Entry {
        qint64 sizeBytes = 0;
        qint64 lastAccessMilliseconds = 0;
    };

    bool loadIndex(QString* pErrorMessage);
    bool recover(QString* pErrorMessage);
    bool saveIndex(QString* pErrorMessage) const;
    bool ensureCapacity(qint64 requiredBytes,
            bool bytesAlreadyOnDisk,
            const QSet<QString>& protectedEntryIds,
            QString* pErrorMessage);
    bool evictOne(const QSet<QString>& protectedEntryIds);
    qint64 nextAccessTimestamp();
    bool checkInitialized(QString* pErrorMessage) const;

    QString m_directoryPath;
    Limits m_limits;
    QHash<QString, Entry> m_entries;
    qint64 m_totalBytes = 0;
    qint64 m_lastAccessTimestamp = 0;
    bool m_initialized = false;
};

} // namespace mixxx::stems
