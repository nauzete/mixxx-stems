#include "stems/stemlivesessionregistry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <algorithm>
#include <utility>

#include "track/track.h"

namespace mixxx::stems {
namespace {

struct RegisteredSession {
    std::shared_ptr<StemLiveSession> pSession;
    std::weak_ptr<StemLiveSession> pWeakSession;
    int deckReferenceCount = 0;
};

QMutex s_registryMutex;
QHash<QString, RegisteredSession> s_sessions;

QString sessionIdForTrack(const TrackPointer& pTrack) {
    const QFileInfo fileInfo(pTrack->getLocation());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(fileInfo.absoluteFilePath().toUtf8());
    hash.addData(QByteArray::number(fileInfo.size()));
    hash.addData(QByteArray::number(
            fileInfo.lastModified().toMSecsSinceEpoch()));
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace

StemLiveSession::StemLiveSession(QString id,
        QString temporaryDirectoryPath,
        TrackPointer pTrack)
        : m_id(std::move(id)),
          m_temporaryDirectoryPath(
                  QDir::cleanPath(
                          std::move(temporaryDirectoryPath))),
          m_pTrack(std::move(pTrack)) {
}

StemLiveSession::~StemLiveSession() {
    const QMutexLocker locker(&m_mutex);
    if (m_pTemporarySession &&
            QFileInfo::exists(
                    m_pTemporarySession->filePath)) {
        QFile::remove(m_pTemporarySession->filePath);
    }
    for (const auto& attachedTrack : m_attachedTracks) {
        if (attachedTrack.ownsTemporaryStemInfo &&
                attachedTrack.pTrack) {
            attachedTrack.pTrack
                    ->clearStemInfosForTemporarySource();
        }
    }
}

QUrl StemLiveSession::url() const {
    return QUrl::fromLocalFile(
            QDir(m_temporaryDirectoryPath)
                    .filePath(m_id +
                            QStringLiteral(".stemlive")));
}

void StemLiveSession::publishTemporarySession(
        std::shared_ptr<StemTemporarySession> pSession) {
    const QMutexLocker locker(&m_mutex);
    m_pTemporarySession = std::move(pSession);
}

std::shared_ptr<StemTemporarySession>
StemLiveSession::temporarySession() const {
    const QMutexLocker locker(&m_mutex);
    return m_pTemporarySession;
}

void StemLiveSession::attachTrack(
        const TrackPointer& pTrack) {
    if (!pTrack) {
        return;
    }
    const QMutexLocker locker(&m_mutex);
    const auto attachedTrack = std::find_if(
            m_attachedTracks.begin(),
            m_attachedTracks.end(),
            [&](const auto& candidate) {
                return candidate.pTrack == pTrack;
            });
    if (attachedTrack != m_attachedTracks.end()) {
        ++attachedTrack->referenceCount;
        return;
    }
    m_attachedTracks.push_back({
            pTrack,
            1,
            pTrack->setStemInfosForTemporarySource({
                    StemInfo(QStringLiteral("DRUMS"),
                            QColor(QStringLiteral("#48d273"))),
                    StemInfo(QStringLiteral("BASS"),
                            QColor(QStringLiteral("#ef5a5a"))),
                    StemInfo(QStringLiteral("OTHER"),
                            QColor(QStringLiteral("#b573ef"))),
                    StemInfo(QStringLiteral("VOCALS"),
                            QColor(QStringLiteral("#4aa9ff"))),
            }),
    });
}

void StemLiveSession::detachTrack(
        const TrackPointer& pTrack) {
    if (!pTrack) {
        return;
    }
    const QMutexLocker locker(&m_mutex);
    const auto attachedTrack = std::find_if(
            m_attachedTracks.begin(),
            m_attachedTracks.end(),
            [&](const auto& candidate) {
                return candidate.pTrack == pTrack;
            });
    if (attachedTrack == m_attachedTracks.end() ||
            --attachedTrack->referenceCount > 0) {
        return;
    }
    if (attachedTrack->ownsTemporaryStemInfo) {
        attachedTrack->pTrack
                ->clearStemInfosForTemporarySource();
    }
    m_attachedTracks.erase(attachedTrack);
}

void StemLiveSession::requestThrough(
        std::size_t frameCount) noexcept {
    auto requested = m_requestedFrameCount.load(
            std::memory_order_relaxed);
    while (requested < frameCount &&
            !m_requestedFrameCount.compare_exchange_weak(
                    requested,
                    frameCount,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
    }
}

std::size_t StemLiveSession::requestedFrameCount() const noexcept {
    return m_requestedFrameCount.load(
            std::memory_order_acquire);
}

std::shared_ptr<StemLiveSession>
StemLiveSessionRegistry::acquire(
        const QString& temporaryDirectoryPath,
        const TrackPointer& pTrack) {
    if (!pTrack || temporaryDirectoryPath.isEmpty()) {
        return {};
    }
    const auto sessionId = sessionIdForTrack(pTrack);
    const QMutexLocker locker(&s_registryMutex);
    auto session = s_sessions.find(sessionId);
    if (session == s_sessions.end()) {
        auto pSession =
                std::make_shared<StemLiveSession>(
                        sessionId,
                        temporaryDirectoryPath,
                        pTrack);
        session = s_sessions.insert(sessionId,
                RegisteredSession{
                        pSession,
                        pSession,
                        0,
                });
    } else if (!session->pSession) {
        session->pSession =
                session->pWeakSession.lock();
        if (!session->pSession) {
            session->pSession =
                    std::make_shared<StemLiveSession>(
                            sessionId,
                            temporaryDirectoryPath,
                            pTrack);
            session->pWeakSession =
                    session->pSession;
        }
    }
    session->pSession->attachTrack(pTrack);
    ++session->deckReferenceCount;
    return session->pSession;
}

std::shared_ptr<StemLiveSession>
StemLiveSessionRegistry::find(const QString& sessionId) {
    const QMutexLocker locker(&s_registryMutex);
    const auto session = s_sessions.constFind(sessionId);
    return session == s_sessions.cend()
            ? nullptr
            : session->pSession
            ? session->pSession
            : session->pWeakSession.lock();
}

void StemLiveSessionRegistry::release(
        const QString& sessionId,
        const TrackPointer& pTrack) {
    const QMutexLocker locker(&s_registryMutex);
    auto session = s_sessions.find(sessionId);
    if (session == s_sessions.end()) {
        return;
    }
    const auto pSession = session->pSession
            ? session->pSession
            : session->pWeakSession.lock();
    if (pSession) {
        pSession->detachTrack(pTrack);
    }
    if (session->deckReferenceCount > 0 &&
            --session->deckReferenceCount == 0) {
        session->pWeakSession = session->pSession;
        session->pSession.reset();
    }
}

void StemLiveSessionRegistry::removeStaleFiles(
        const QString& temporaryDirectoryPath) {
    const QDir directory(temporaryDirectoryPath);
    const QMutexLocker locker(&s_registryMutex);
    for (auto session = s_sessions.begin();
            session != s_sessions.end();) {
        if (session->deckReferenceCount == 0 &&
                session->pWeakSession.expired()) {
            session = s_sessions.erase(session);
        } else {
            ++session;
        }
    }
    const auto fileNames = directory.entryList(
            {QStringLiteral("*.stemlive.pcm")},
            QDir::Files);
    for (const auto& fileName : fileNames) {
        auto sessionId = fileName;
        sessionId.chop(
                QStringLiteral(".stemlive.pcm").size());
        const auto session = s_sessions.constFind(sessionId);
        const bool active =
                session != s_sessions.cend() &&
                (session->pSession ||
                        !session->pWeakSession.expired());
        if (!active) {
            QFile::remove(directory.filePath(fileName));
        }
    }
}

} // namespace mixxx::stems
