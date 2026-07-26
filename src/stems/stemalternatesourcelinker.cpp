#include "stems/stemalternatesourcelinker.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>

#include "util/logger.h"

namespace mixxx::stems {
namespace {

const Logger kLogger("StemAlternateSourceLinker");
const QString kIndexFileName =
        QStringLiteral(".stem-source-links.json");
const QRegularExpression kEntryIdPattern(
        QStringLiteral("^[0-9a-f]{64}$"));

bool fail(QString* pErrorMessage, QString message) {
    if (pErrorMessage) {
        *pErrorMessage = std::move(message);
    }
    return false;
}

qint64 jsonInteger(
        const QJsonObject& object, const QString& key) {
    bool ok = false;
    const auto value = object.value(key).toString().toLongLong(&ok);
    return ok ? value : -1;
}

} // namespace

StemAlternateSourceLinker::StemAlternateSourceLinker(
        QString cacheDirectoryPath)
        : m_cacheDirectoryPath(
                  QDir::cleanPath(std::move(cacheDirectoryPath))) {
}

bool StemAlternateSourceLinker::initialize(
        QString* pErrorMessage) {
    const QMutexLocker locker(&m_mutex);
    if (m_initialized) {
        return true;
    }
    if (m_cacheDirectoryPath.isEmpty() ||
            !QDir().mkpath(m_cacheDirectoryPath)) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Failed to create stem link directory"));
    }
    if (!loadIndex(pErrorMessage)) {
        return false;
    }
    m_initialized = true;
    return true;
}

bool StemAlternateSourceLinker::registerCompleted(
        const QString& sourceFilePath,
        const QString& entryId,
        QString* pErrorMessage) {
    const QMutexLocker locker(&m_mutex);
    if (!checkInitialized(pErrorMessage)) {
        return false;
    }
    const QFileInfo sourceInfo(sourceFilePath);
    const QFileInfo representationInfo(
            representationPath(entryId));
    const auto sourcePath =
            normalizedSourcePath(sourceFilePath);
    if (sourcePath.isEmpty() || !sourceInfo.isFile() ||
            sourceInfo.size() < 0 ||
            !kEntryIdPattern.match(entryId).hasMatch() ||
            !representationInfo.isFile() ||
            representationInfo.size() <= 0) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Cannot link invalid stem representation"));
    }
    m_links.insert(sourcePath,
            Link{
                    entryId,
                    sourceInfo.size(),
                    sourceInfo.lastModified()
                            .toMSecsSinceEpoch(),
            });
    return saveIndex(pErrorMessage);
}

std::optional<StemAlternateSourceLinker::Resolution>
StemAlternateSourceLinker::resolve(
        const QString& sourceFilePath,
        QString* pErrorMessage) {
    const QMutexLocker locker(&m_mutex);
    if (!checkInitialized(pErrorMessage)) {
        return std::nullopt;
    }
    const auto sourcePath =
            normalizedSourcePath(sourceFilePath);
    auto link = m_links.find(sourcePath);
    if (link == m_links.end()) {
        return std::nullopt;
    }
    const QFileInfo sourceInfo(sourceFilePath);
    const auto outputPath =
            representationPath(link->entryId);
    const QFileInfo outputInfo(outputPath);
    if (!sourceInfo.isFile() ||
            sourceInfo.size() != link->sourceSize ||
            sourceInfo.lastModified().toMSecsSinceEpoch() !=
                    link->sourceModifiedMilliseconds ||
            !outputInfo.isFile() || outputInfo.size() <= 0) {
        m_links.erase(link);
        if (!saveIndex(pErrorMessage)) {
            return std::nullopt;
        }
        return std::nullopt;
    }
    return Resolution{link->entryId, outputPath};
}

bool StemAlternateSourceLinker::removeEntry(
        const QString& entryId,
        QString* pErrorMessage) {
    const QMutexLocker locker(&m_mutex);
    if (!checkInitialized(pErrorMessage)) {
        return false;
    }
    bool changed = false;
    for (auto link = m_links.begin(); link != m_links.end();) {
        if (link->entryId == entryId) {
            link = m_links.erase(link);
            changed = true;
        } else {
            ++link;
        }
    }
    return !changed || saveIndex(pErrorMessage);
}

QString StemAlternateSourceLinker::normalizedSourcePath(
        const QString& sourceFilePath) const {
    const QFileInfo fileInfo(sourceFilePath);
    auto result = fileInfo.canonicalFilePath();
    if (result.isEmpty()) {
        result = fileInfo.absoluteFilePath();
    }
    result = QDir::cleanPath(result);
#if defined(Q_OS_WIN)
    result = result.toCaseFolded();
#endif
    return result;
}

QString StemAlternateSourceLinker::representationPath(
        const QString& entryId) const {
    return QDir(m_cacheDirectoryPath)
            .filePath(entryId + QStringLiteral(".stem.mp4"));
}

bool StemAlternateSourceLinker::loadIndex(
        QString* pErrorMessage) {
    QFile file(
            QDir(m_cacheDirectoryPath).filePath(kIndexFileName));
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to open stem link index"));
    }
    QJsonParseError parseError;
    const auto document =
            QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !document.isObject() ||
            document.object()
                            .value(QStringLiteral("version"))
                            .toInt() != 1) {
        kLogger.warning()
                << "Ignoring malformed stem source link index";
        return true;
    }
    const auto links =
            document.object().value(QStringLiteral("links")).toArray();
    for (const auto& value : links) {
        const auto object = value.toObject();
        const auto sourcePath =
                object.value(QStringLiteral("source_path")).toString();
        const auto entryId =
                object.value(QStringLiteral("entry_id")).toString();
        const auto sourceSize =
                jsonInteger(object, QStringLiteral("source_size"));
        const auto sourceModified = jsonInteger(
                object, QStringLiteral("source_modified_ms"));
        if (sourcePath.isEmpty() ||
                !kEntryIdPattern.match(entryId).hasMatch() ||
                sourceSize < 0 || sourceModified < 0) {
            continue;
        }
        m_links.insert(sourcePath,
                Link{entryId, sourceSize, sourceModified});
    }
    return true;
}

bool StemAlternateSourceLinker::saveIndex(
        QString* pErrorMessage) const {
    QJsonArray links;
    QStringList sourcePaths = m_links.keys();
    std::sort(sourcePaths.begin(), sourcePaths.end());
    for (const auto& sourcePath : sourcePaths) {
        const auto& link = m_links[sourcePath];
        links.append(QJsonObject{
                {QStringLiteral("source_path"), sourcePath},
                {QStringLiteral("entry_id"), link.entryId},
                {QStringLiteral("source_size"),
                        QString::number(link.sourceSize)},
                {QStringLiteral("source_modified_ms"),
                        QString::number(
                                link.sourceModifiedMilliseconds)},
        });
    }
    const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("links"), links},
    };
    QSaveFile file(
            QDir(m_cacheDirectoryPath).filePath(kIndexFileName));
    if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument(root).toJson(
                    QJsonDocument::Compact)) < 0 ||
            !file.commit()) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Failed to atomically save stem link index"));
    }
    return true;
}

bool StemAlternateSourceLinker::checkInitialized(
        QString* pErrorMessage) const {
    return m_initialized ||
            fail(pErrorMessage,
                    QStringLiteral(
                            "Stem source linker is not initialized"));
}

} // namespace mixxx::stems
