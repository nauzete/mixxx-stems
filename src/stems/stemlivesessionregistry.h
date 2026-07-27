#pragma once

#include <QMutex>
#include <QString>
#include <QUrl>
#include <memory>
#include <vector>

#include "stems/stemtemporarystore.h"
#include "track/track_decl.h"

namespace mixxx::stems {

class StemLiveSession final {
  public:
    StemLiveSession(QString id,
            QString temporaryDirectoryPath,
            TrackPointer pTrack);
    ~StemLiveSession();

    QString id() const {
        return m_id;
    }
    QUrl url() const;
    QString temporaryDirectoryPath() const {
        return m_temporaryDirectoryPath;
    }
    TrackPointer track() const {
        return m_pTrack;
    }

    void publishTemporarySession(
            std::shared_ptr<StemTemporarySession> pSession);
    std::shared_ptr<StemTemporarySession>
    temporarySession() const;
    void attachTrack(const TrackPointer& pTrack);
    void detachTrack(const TrackPointer& pTrack);

  private:
    struct AttachedTrack {
        TrackPointer pTrack;
        int referenceCount = 0;
        bool ownsTemporaryStemInfo = false;
    };

    QString m_id;
    QString m_temporaryDirectoryPath;
    TrackPointer m_pTrack;
    mutable QMutex m_mutex;
    std::shared_ptr<StemTemporarySession> m_pTemporarySession;
    std::vector<AttachedTrack> m_attachedTracks;
};

/// Process-local registry connecting generated chunks to SoundSourceStemLive.
///
/// All methods are called outside the real-time audio thread.
class StemLiveSessionRegistry final {
  public:
    static std::shared_ptr<StemLiveSession> acquire(
            const QString& temporaryDirectoryPath,
            const TrackPointer& pTrack);
    static std::shared_ptr<StemLiveSession> find(
            const QString& sessionId);
    static void release(const QString& sessionId,
            const TrackPointer& pTrack = {});
    static void removeStaleFiles(
            const QString& temporaryDirectoryPath);
};

} // namespace mixxx::stems
