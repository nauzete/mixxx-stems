#include "stems/stemtemporarystore.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "util/logger.h"

namespace mixxx::stems {
namespace {

const Logger kLogger("StemTemporaryStore");
const QRegularExpression kSessionIdPattern(
        QStringLiteral("^[0-9a-f]{64}$"));

} // namespace

StemTemporaryStore::StemTemporaryStore(
        QString directoryPath, qint64 maximumBytes)
        : m_directoryPath(
                  QDir::cleanPath(std::move(directoryPath))),
          m_maximumBytes(maximumBytes) {
}

StemTemporaryStore::~StemTemporaryStore() {
    m_file.close();
}

bool StemTemporaryStore::initialize(
        const QString& sessionId,
        std::size_t totalFrameCount,
        QString* pErrorMessage) {
    if (m_pSession || m_directoryPath.isEmpty() ||
            m_maximumBytes <= 0 || totalFrameCount == 0 ||
            !kSessionIdPattern.match(sessionId).hasMatch()) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Invalid temporary stem store configuration"));
    }
    constexpr auto kBytesPerFrame =
            kStemChannelCount * sizeof(qint16);
    if (totalFrameCount >
            static_cast<std::size_t>(
                    std::numeric_limits<qint64>::max() /
                    kBytesPerFrame)) {
        return fail(pErrorMessage,
                QStringLiteral("Temporary stem store size overflow"));
    }
    const auto expectedBytes =
            static_cast<qint64>(totalFrameCount * kBytesPerFrame);
    if (expectedBytes > m_maximumBytes ||
            !QDir().mkpath(m_directoryPath)) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Temporary stem store exceeds its storage guard"));
    }
    const auto filePath = QDir(m_directoryPath)
                                  .filePath(sessionId +
                                          QStringLiteral(
                                                  ".stemlive.pcm"));
    m_file.setFileName(filePath);
    if (QFileInfo::exists(filePath) &&
            !QFile::remove(filePath)) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Failed to remove stale temporary stems"));
    }
    if (!m_file.open(QIODevice::WriteOnly)) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Failed to create temporary stem store"));
    }
    m_pSession = std::make_shared<StemTemporarySession>();
    m_pSession->id = sessionId;
    m_pSession->filePath = filePath;
    m_pSession->totalFrameCount = totalFrameCount;
    return true;
}

bool StemTemporaryStore::append(
        std::size_t frameOffset,
        std::size_t frameCount,
        std::span<const float> planarStems,
        QString* pErrorMessage) {
    if (!m_pSession || !m_file.isOpen() || frameCount == 0 ||
            frameOffset != m_pSession->readyFrameCount.load(
                                   std::memory_order_acquire) ||
            frameOffset > m_pSession->totalFrameCount ||
            frameCount >
                    m_pSession->totalFrameCount - frameOffset ||
            planarStems.size() !=
                    kStemChannelCount * frameCount) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Invalid temporary stem chunk"));
    }
    m_interleavedPcm.resize(
            frameCount * kStemChannelCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t channel = 0;
                channel < kStemChannelCount;
                ++channel) {
            const auto sample = std::clamp(
                    planarStems[channel * frameCount + frame],
                    -1.0F,
                    1.0F);
            const auto pcm = static_cast<qint16>(
                    std::lround(sample * 32767.0F));
            m_interleavedPcm[frame * kStemChannelCount + channel] =
                    qToLittleEndian(pcm);
        }
    }
    const auto byteCount = static_cast<qint64>(
            m_interleavedPcm.size() * sizeof(qint16));
    if (m_file.write(
                reinterpret_cast<const char*>(
                        m_interleavedPcm.data()),
                byteCount) != byteCount ||
            !m_file.flush()) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Failed to publish temporary stem chunk"));
    }
    m_pSession->readyFrameCount.store(
            frameOffset + frameCount,
            std::memory_order_release);
    return true;
}

bool StemTemporaryStore::finish(QString* pErrorMessage) {
    if (!m_pSession ||
            m_pSession->readyFrameCount.load(
                    std::memory_order_acquire) !=
                    m_pSession->totalFrameCount ||
            !m_file.flush()) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Temporary stem store is incomplete"));
    }
    m_pSession->complete.store(true, std::memory_order_release);
    m_file.close();
    return true;
}

bool StemTemporaryStore::remove(QString* pErrorMessage) {
    if (!m_pSession) {
        return true;
    }
    m_file.close();
    m_pSession->complete.store(false, std::memory_order_release);
    m_pSession->readyFrameCount.store(0, std::memory_order_release);
    if (QFileInfo::exists(m_pSession->filePath) &&
            !QFile::remove(m_pSession->filePath)) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "Failed to remove temporary stems"));
    }
    m_pSession.reset();
    m_interleavedPcm.clear();
    return true;
}

bool StemTemporaryStore::fail(
        QString* pErrorMessage, QString message) const {
    kLogger.warning().noquote() << message;
    if (pErrorMessage) {
        *pErrorMessage = std::move(message);
    }
    return false;
}

} // namespace mixxx::stems
