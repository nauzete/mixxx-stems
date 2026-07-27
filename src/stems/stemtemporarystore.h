#pragma once

#include <QFile>
#include <QString>
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace mixxx::stems {

struct StemTemporarySession final {
    QString id;
    QString filePath;
    std::size_t totalFrameCount = 0;
    std::atomic_size_t readyFrameCount{0};
    std::atomic_bool complete{false};
};

/// Sequential temporary PCM store for progressively separated stems.
///
/// The worker publishes readyFrameCount with release semantics only after a
/// complete chunk has been flushed. Readers with a separate file handle may
/// safely read frames below that boundary without locking the inference
/// worker. The store is never accessed from Mixxx's real-time audio thread.
class StemTemporaryStore final {
  public:
    static constexpr std::size_t kStemChannelCount = 8;
    static constexpr qint64 kDefaultMaximumBytes =
            512LL * 1024LL * 1024LL;

    explicit StemTemporaryStore(QString directoryPath,
            qint64 maximumBytes = kDefaultMaximumBytes);
    ~StemTemporaryStore();

    bool initialize(const QString& sessionId,
            std::size_t totalFrameCount,
            QString* pErrorMessage = nullptr);

    bool append(std::size_t frameOffset,
            std::size_t frameCount,
            std::span<const float> planarStems,
            QString* pErrorMessage = nullptr);

    bool finish(QString* pErrorMessage = nullptr);
    bool remove(QString* pErrorMessage = nullptr);

    std::shared_ptr<StemTemporarySession> session() const {
        return m_pSession;
    }

  private:
    bool fail(QString* pErrorMessage, QString message) const;

    QString m_directoryPath;
    qint64 m_maximumBytes;
    QFile m_file;
    std::shared_ptr<StemTemporarySession> m_pSession;
    std::vector<qint16> m_interleavedPcm;
};

} // namespace mixxx::stems
