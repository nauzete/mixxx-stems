#include "stems/stemcache.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QtEndian>

#include "util/logger.h"

namespace mixxx::stems {
namespace {

const Logger kLogger("StemCache");
constexpr qsizetype kHashBlockSize = 1024 * 1024;
const QString kIndexFileName = QStringLiteral(".stem-cache-index.json");
const QRegularExpression kEntryIdPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
const QRegularExpression kCompletedFilePattern(
        QStringLiteral("^([0-9a-f]{64})\\.stem\\.mp4$"));
const QRegularExpression kPartialFilePattern(
        QStringLiteral("^([0-9a-f]{64})\\.stem\\.mp4\\.partial$"));

bool fail(QString* pErrorMessage, QString message) {
    kLogger.warning().noquote() << message;
    if (pErrorMessage) {
        *pErrorMessage = std::move(message);
    }
    return false;
}

void addHashField(
        QCryptographicHash* pHash, const QByteArray& field) {
    std::array<uchar, sizeof(quint64)> size{};
    qToBigEndian(static_cast<quint64>(field.size()), size.data());
    pHash->addData(QByteArrayView(
            reinterpret_cast<const char*>(size.data()), size.size()));
    pHash->addData(field);
}

qint64 jsonInteger(
        const QJsonObject& object, const QString& name, qint64 fallback) {
    bool ok = false;
    const auto value = object.value(name).toString().toLongLong(&ok);
    return ok ? value : fallback;
}

} // namespace

QString StemCacheKey::id() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addHashField(&hash, sourceSha256);
    addHashField(&hash, QByteArray::number(sourceSize));
    addHashField(
            &hash, QByteArray::number(sourceModifiedMilliseconds));
    addHashField(&hash, modelName.toUtf8());
    addHashField(&hash, modelVersion.toUtf8());
    addHashField(&hash, modelSha256);
    addHashField(&hash, codec.toUtf8());
    addHashField(&hash, QByteArray::number(bitRatePerStream));
    addHashField(&hash, QByteArray::number(containerVersion));
    return QString::fromLatin1(hash.result().toHex());
}

// static
std::optional<StemCacheKey> StemCacheKey::fromSourceFile(
        const QString& sourceFilePath,
        QString modelName,
        QString modelVersion,
        QByteArray modelSha256,
        QString codec,
        int bitRatePerStream,
        int containerVersion,
        QString* pErrorMessage,
        const std::function<bool()>& cancelled) {
    QFile file(sourceFilePath);
    const QFileInfo info(file);
    if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
        fail(pErrorMessage,
                QStringLiteral("Failed to open source file for cache identity"));
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray block(kHashBlockSize, '\0');
    while (!file.atEnd()) {
        if (cancelled && cancelled()) {
            fail(pErrorMessage,
                    QStringLiteral("Source hashing was cancelled"));
            return std::nullopt;
        }
        const auto read = file.read(block.data(), block.size());
        if (read < 0) {
            fail(pErrorMessage,
                    QStringLiteral("Failed to hash source file for cache identity"));
            return std::nullopt;
        }
        if (read > 0) {
            hash.addData(QByteArrayView(block.constData(), read));
        }
    }
    if (bitRatePerStream <= 0 || containerVersion <= 0 ||
            modelName.isEmpty() || modelVersion.isEmpty() ||
            modelSha256.isEmpty() || codec.isEmpty()) {
        fail(pErrorMessage,
                QStringLiteral("Incomplete cache identity settings"));
        return std::nullopt;
    }
    return StemCacheKey{
            hash.result(),
            info.size(),
            info.lastModified().toMSecsSinceEpoch(),
            std::move(modelName),
            std::move(modelVersion),
            std::move(modelSha256),
            std::move(codec),
            bitRatePerStream,
            containerVersion,
    };
}

StemCache::StemCache(QString directoryPath, Limits limits)
        : m_directoryPath(
                  QDir::cleanPath(std::move(directoryPath))),
          m_limits(limits) {
}

bool StemCache::initialize(QString* pErrorMessage) {
    if (m_limits.quotaBytes <= 0 ||
            m_limits.freeSpaceReserveBytes < 0 ||
            m_limits.maximumTrackBytes <= 0 ||
            m_limits.maximumTrackBytes > m_limits.quotaBytes) {
        return fail(pErrorMessage,
                QStringLiteral("Invalid stem cache limits"));
    }
    if (!QDir().mkpath(m_directoryPath)) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to create stem cache directory"));
    }
    const QFileInfo directoryInfo(m_directoryPath);
    if (!directoryInfo.isDir() || !directoryInfo.isWritable()) {
        return fail(pErrorMessage,
                QStringLiteral("Stem cache directory is not writable"));
    }
    m_entries.clear();
    m_totalBytes = 0;
    m_lastAccessTimestamp = 0;
    m_initialized = true;
    if (!loadIndex(pErrorMessage) ||
            !recover(pErrorMessage) ||
            !ensureCapacity(0, false, {}, pErrorMessage) ||
            !saveIndex(pErrorMessage)) {
        m_initialized = false;
        return false;
    }
    return true;
}

std::optional<QString> StemCache::lookup(
        const StemCacheKey& key, QString* pErrorMessage) {
    if (!checkInitialized(pErrorMessage)) {
        return std::nullopt;
    }
    const auto entryId = key.id();
    auto entry = m_entries.find(entryId);
    if (entry == m_entries.end()) {
        return std::nullopt;
    }
    const QFileInfo fileInfo(filePath(entryId));
    if (!fileInfo.isFile() || fileInfo.size() != entry->sizeBytes) {
        m_totalBytes -= entry->sizeBytes;
        m_entries.erase(entry);
        saveIndex(pErrorMessage);
        return std::nullopt;
    }
    entry->lastAccessMilliseconds = nextAccessTimestamp();
    if (!saveIndex(pErrorMessage)) {
        return std::nullopt;
    }
    return fileInfo.absoluteFilePath();
}

std::optional<QString> StemCache::reserve(const StemCacheKey& key,
        qint64 maximumNewEntryBytes,
        const QSet<QString>& protectedEntryIds,
        QString* pErrorMessage) {
    if (!checkInitialized(pErrorMessage)) {
        return std::nullopt;
    }
    const auto entryId = key.id();
    if (m_entries.contains(entryId)) {
        fail(pErrorMessage,
                QStringLiteral("Stem cache entry already exists"));
        return std::nullopt;
    }
    if (maximumNewEntryBytes <= 0 ||
            maximumNewEntryBytes > m_limits.maximumTrackBytes) {
        fail(pErrorMessage,
                QStringLiteral("Stem cache entry exceeds the per-track guard"));
        return std::nullopt;
    }
    const auto partialPath = partialFilePath(entryId);
    if (QFileInfo::exists(partialPath) &&
            !QFile::remove(partialPath)) {
        fail(pErrorMessage,
                QStringLiteral("Failed to remove stale cache partial"));
        return std::nullopt;
    }
    if (!ensureCapacity(maximumNewEntryBytes,
                false,
                protectedEntryIds,
                pErrorMessage) ||
            !saveIndex(pErrorMessage)) {
        return std::nullopt;
    }
    return filePath(entryId);
}

bool StemCache::registerCompleted(const StemCacheKey& key,
        const QSet<QString>& protectedEntryIds,
        QString* pErrorMessage) {
    if (!checkInitialized(pErrorMessage)) {
        return false;
    }
    const auto entryId = key.id();
    if (m_entries.contains(entryId)) {
        return fail(pErrorMessage,
                QStringLiteral("Stem cache entry is already registered"));
    }
    const QFileInfo fileInfo(filePath(entryId));
    if (!fileInfo.isFile() || fileInfo.size() <= 0 ||
            fileInfo.size() > m_limits.maximumTrackBytes) {
        return fail(pErrorMessage,
                QStringLiteral("Completed stem cache file is invalid or too large"));
    }
    auto protectedWithNewEntry = protectedEntryIds;
    protectedWithNewEntry.insert(entryId);
    if (!ensureCapacity(fileInfo.size(),
                true,
                protectedWithNewEntry,
                pErrorMessage)) {
        return false;
    }
    m_entries.insert(entryId,
            Entry{
                    fileInfo.size(),
                    nextAccessTimestamp(),
            });
    m_totalBytes += fileInfo.size();
    if (!saveIndex(pErrorMessage)) {
        m_totalBytes -= fileInfo.size();
        m_entries.remove(entryId);
        return false;
    }
    return true;
}

bool StemCache::remove(const QString& entryId,
        const QSet<QString>& protectedEntryIds,
        QString* pErrorMessage) {
    if (!checkInitialized(pErrorMessage)) {
        return false;
    }
    if (protectedEntryIds.contains(entryId)) {
        return fail(pErrorMessage,
                QStringLiteral("Refusing to remove a protected stem cache entry"));
    }
    auto entry = m_entries.find(entryId);
    if (entry == m_entries.end()) {
        return true;
    }
    QFile file(filePath(entryId));
    if (file.exists() && !file.remove()) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to remove stem cache entry"));
    }
    m_totalBytes -= entry->sizeBytes;
    m_entries.erase(entry);
    return saveIndex(pErrorMessage);
}

qint64 StemCache::totalBytes() const noexcept {
    return m_totalBytes;
}

qsizetype StemCache::entryCount() const noexcept {
    return m_entries.size();
}

QString StemCache::directoryPath() const {
    return m_directoryPath;
}

QString StemCache::filePath(const QString& entryId) const {
    return QDir(m_directoryPath)
            .filePath(entryId + QStringLiteral(".stem.mp4"));
}

QString StemCache::partialFilePath(const QString& entryId) const {
    return filePath(entryId) + QStringLiteral(".partial");
}

StemCache::Limits StemCache::limits() const noexcept {
    return m_limits;
}

bool StemCache::loadIndex(QString* pErrorMessage) {
    QFile file(QDir(m_directoryPath).filePath(kIndexFileName));
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to open stem cache index"));
    }
    QJsonParseError parseError;
    const auto document =
            QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !document.isObject() ||
            document.object().value(QStringLiteral("version")).toInt() != 1) {
        kLogger.warning()
                << "Ignoring malformed stem cache index and rebuilding it";
        return true;
    }
    const auto entries =
            document.object().value(QStringLiteral("entries")).toArray();
    for (const auto& value : entries) {
        const auto object = value.toObject();
        const auto entryId =
                object.value(QStringLiteral("id")).toString();
        const auto size =
                jsonInteger(object, QStringLiteral("size_bytes"), -1);
        const auto lastAccess = jsonInteger(
                object, QStringLiteral("last_access_ms"), -1);
        if (!kEntryIdPattern.match(entryId).hasMatch() ||
                size <= 0 || lastAccess < 0 ||
                m_entries.contains(entryId)) {
            continue;
        }
        m_entries.insert(entryId, Entry{size, lastAccess});
        m_totalBytes += size;
        m_lastAccessTimestamp =
                std::max(m_lastAccessTimestamp, lastAccess);
    }
    return true;
}

bool StemCache::recover(QString* pErrorMessage) {
    for (auto entry = m_entries.begin(); entry != m_entries.end();) {
        const QFileInfo fileInfo(filePath(entry.key()));
        if (!fileInfo.isFile() || fileInfo.size() != entry->sizeBytes) {
            m_totalBytes -= entry->sizeBytes;
            entry = m_entries.erase(entry);
        } else {
            ++entry;
        }
    }

    const QDir directory(m_directoryPath);
    const auto files = directory.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot);
    for (const auto& fileInfo : files) {
        const auto partialMatch =
                kPartialFilePattern.match(fileInfo.fileName());
        if (partialMatch.hasMatch()) {
            QFile partial(fileInfo.absoluteFilePath());
            if (!partial.remove()) {
                return fail(pErrorMessage,
                        QStringLiteral("Failed to clean stale stem cache partial"));
            }
            continue;
        }
        const auto completedMatch =
                kCompletedFilePattern.match(fileInfo.fileName());
        if (!completedMatch.hasMatch()) {
            continue;
        }
        const auto entryId = completedMatch.captured(1);
        if (!m_entries.contains(entryId) && fileInfo.size() > 0 &&
                fileInfo.size() <= m_limits.maximumTrackBytes) {
            const auto access = std::max(
                    fileInfo.lastModified().toMSecsSinceEpoch(),
                    nextAccessTimestamp());
            m_entries.insert(entryId,
                    Entry{
                            fileInfo.size(),
                            access,
                    });
            m_totalBytes += fileInfo.size();
        }
    }
    return true;
}

bool StemCache::saveIndex(QString* pErrorMessage) const {
    QJsonArray entries;
    QStringList entryIds = m_entries.keys();
    std::sort(entryIds.begin(), entryIds.end());
    for (const auto& entryId : entryIds) {
        const auto& entry = m_entries[entryId];
        entries.append(QJsonObject{
                {QStringLiteral("id"), entryId},
                {QStringLiteral("size_bytes"),
                        QString::number(entry.sizeBytes)},
                {QStringLiteral("last_access_ms"),
                        QString::number(entry.lastAccessMilliseconds)},
        });
    }
    const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("entries"), entries},
    };
    QSaveFile file(QDir(m_directoryPath).filePath(kIndexFileName));
    if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) <
                    0 ||
            !file.commit()) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to atomically save stem cache index"));
    }
    return true;
}

bool StemCache::ensureCapacity(qint64 requiredBytes,
        bool bytesAlreadyOnDisk,
        const QSet<QString>& protectedEntryIds,
        QString* pErrorMessage) {
    if (requiredBytes < 0 ||
            requiredBytes > m_limits.maximumTrackBytes) {
        return fail(pErrorMessage,
                QStringLiteral("Invalid stem cache capacity request"));
    }
    QStorageInfo storage(m_directoryPath);
    storage.refresh();
    if (!storage.isValid() || !storage.isReady()) {
        return fail(pErrorMessage,
                QStringLiteral("Stem cache storage is unavailable"));
    }
    auto available = storage.bytesAvailable();
    const auto additionalDiskBytes =
            bytesAlreadyOnDisk ? 0 : requiredBytes;
    while (m_totalBytes + requiredBytes > m_limits.quotaBytes ||
            available - additionalDiskBytes <
                    m_limits.freeSpaceReserveBytes) {
        const auto previousBytes = m_totalBytes;
        if (!evictOne(protectedEntryIds)) {
            return fail(pErrorMessage,
                    QStringLiteral("Insufficient unprotected stem cache capacity"));
        }
        available += previousBytes - m_totalBytes;
    }
    return true;
}

bool StemCache::evictOne(
        const QSet<QString>& protectedEntryIds) {
    auto candidate = m_entries.end();
    for (auto entry = m_entries.begin(); entry != m_entries.end(); ++entry) {
        if (protectedEntryIds.contains(entry.key())) {
            continue;
        }
        if (candidate == m_entries.end() ||
                entry->lastAccessMilliseconds <
                        candidate->lastAccessMilliseconds) {
            candidate = entry;
        }
    }
    if (candidate == m_entries.end()) {
        return false;
    }
    QFile file(filePath(candidate.key()));
    if (file.exists() && !file.remove()) {
        kLogger.warning()
                << "Failed to evict stem cache entry"
                << candidate.key();
        return false;
    }
    m_totalBytes -= candidate->sizeBytes;
    m_entries.erase(candidate);
    return true;
}

qint64 StemCache::nextAccessTimestamp() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    m_lastAccessTimestamp =
            std::max(now, m_lastAccessTimestamp + 1);
    return m_lastAccessTimestamp;
}

bool StemCache::checkInitialized(QString* pErrorMessage) const {
    if (!m_initialized) {
        return fail(pErrorMessage,
                QStringLiteral("Stem cache is not initialized"));
    }
    return true;
}

} // namespace mixxx::stems
