#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <optional>

namespace mixxx::stems {

/// Associates a generated stem representation with its original logical track.
///
/// The mapping is resolved only while loading a deck. The original Track
/// remains the metadata owner, so cues, beatgrid, history, and library identity
/// are never duplicated into a synthetic Track.
///
/// This class performs JSON and filesystem I/O and must never be called from
/// the audio thread.
class StemAlternateSourceLinker final {
  public:
    struct Resolution {
        QString entryId;
        QString filePath;
    };

    explicit StemAlternateSourceLinker(QString cacheDirectoryPath);

    bool initialize(QString* pErrorMessage = nullptr);

    bool registerCompleted(const QString& sourceFilePath,
            const QString& entryId,
            QString* pErrorMessage = nullptr);

    /// Returns a completed representation only while the source fingerprint
    /// still matches. Stale links are removed atomically.
    std::optional<Resolution> resolve(
            const QString& sourceFilePath,
            QString* pErrorMessage = nullptr);

    bool removeEntry(
            const QString& entryId,
            QString* pErrorMessage = nullptr);

  private:
    struct Link {
        QString entryId;
        qint64 sourceSize = 0;
        qint64 sourceModifiedMilliseconds = 0;
    };

    QString normalizedSourcePath(const QString& sourceFilePath) const;
    QString representationPath(const QString& entryId) const;
    bool loadIndex(QString* pErrorMessage);
    bool saveIndex(QString* pErrorMessage) const;
    bool checkInitialized(QString* pErrorMessage) const;

    QString m_cacheDirectoryPath;
    QHash<QString, Link> m_links;
    bool m_initialized = false;
    mutable QMutex m_mutex;
};

} // namespace mixxx::stems
