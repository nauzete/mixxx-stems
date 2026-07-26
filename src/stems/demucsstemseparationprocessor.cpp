#include "stems/demucsstemseparationprocessor.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "stems/demucsonnxrunner.h"
#include "stems/stemaudiosourcereader.h"
#include "stems/stemchunkpipeline.h"
#include "stems/stemcontainerwriter.h"
#include "track/track.h"

namespace mixxx::stems {
namespace {

constexpr qsizetype kHashBlockSize = 1024 * 1024;

std::filesystem::path filesystemPath(const QString& path) {
#if defined(Q_OS_WIN)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

bool stopped(
        const StemSeparationProcessor::Callbacks& callbacks) {
    return (callbacks.cancelled && callbacks.cancelled()) ||
            (callbacks.pauseRequested &&
                    callbacks.pauseRequested());
}

} // namespace

DemucsStemSeparationProcessor::DemucsStemSeparationProcessor(
        Settings settings,
        ProtectedEntryIdsProvider protectedEntryIdsProvider)
        : m_settings(std::move(settings)),
          m_protectedEntryIdsProvider(
                  std::move(protectedEntryIdsProvider)),
          m_cache(m_settings.cacheDirectoryPath,
                  m_settings.cacheLimits),
          m_alternateSourceLinker(
                  m_settings.cacheDirectoryPath) {
}

DemucsStemSeparationProcessor::~DemucsStemSeparationProcessor() = default;

StemSeparationProcessor::Result
DemucsStemSeparationProcessor::process(
        const StemSeparationRequest& request,
        const Callbacks& callbacks) {
    if (callbacks.publishState) {
        callbacks.publishState(StemSeparationState::Preparing);
    }
    if (stopped(callbacks)) {
        return cancelledOrPaused(callbacks);
    }
    if (!QFileInfo(request.sourceFilePath).isFile()) {
        return {
                Outcome::PermanentFailure,
                QStringLiteral("Source track is unavailable"),
        };
    }

    QString error;
    if (!prepareRuntime(callbacks, &error)) {
        if (stopped(callbacks)) {
            return cancelledOrPaused(callbacks);
        }
        return {
                Outcome::PermanentFailure,
                std::move(error),
        };
    }

    error.clear();
    if (m_cache.lookup(request.cacheEntryId, &error).has_value()) {
        if (m_alternateSourceLinker.registerCompleted(
                    request.sourceFilePath,
                    request.cacheEntryId,
                    &error)) {
            return {Outcome::Completed, {}};
        }
        return {
                Outcome::RetryableFailure,
                std::move(error),
        };
    }
    if (!error.isEmpty()) {
        return {
                Outcome::RetryableFailure,
                std::move(error),
        };
    }
    const auto protectedIds =
            protectedEntryIds(request.cacheEntryId);
    const auto outputPath = m_cache.reserve(request.cacheEntryId,
            m_settings.cacheLimits.maximumTrackBytes,
            protectedIds,
            &error);
    if (!outputPath) {
        return {
                Outcome::RetryableFailure,
                std::move(error),
        };
    }

    try {
        const auto pTrack =
                Track::newTemporary(request.sourceFilePath);
        auto pReader = StemAudioSourceReader::open(pTrack);
        StemContainerWriter writer(*outputPath,
                {
                        m_settings.bitRatePerStream,
                        m_settings.cacheLimits.maximumTrackBytes,
                });
        if (!writer.open(&error)) {
            return {
                    Outcome::RetryableFailure,
                    std::move(error),
            };
        }
        StemChunkPipeline pipeline(*m_pRunner);
        bool encodingPublished = false;
        if (callbacks.publishState) {
            callbacks.publishState(StemSeparationState::Separating);
        }
        const auto pipelineResult = pipeline.run(
                pReader->frameCount(),
                [&](std::size_t frameOffset,
                        std::span<float> interleavedStereo) {
                    pReader->read(frameOffset, interleavedStereo);
                },
                [&](std::size_t frameOffset,
                        std::size_t frameCount,
                        std::span<const float> planarStems) {
                    if (!encodingPublished &&
                            callbacks.publishState) {
                        callbacks.publishState(
                                StemSeparationState::Encoding);
                        encodingPublished = true;
                    }
                    if (!writer.write(frameOffset,
                                frameCount,
                                planarStems,
                                &error)) {
                        throw std::runtime_error(
                                error.toStdString());
                    }
                },
                [&](float progress) {
                    if (callbacks.publishProgress) {
                        callbacks.publishProgress(progress);
                    }
                },
                [&] { return stopped(callbacks); });
        if (pipelineResult == StemChunkPipeline::Result::Cancelled) {
            writer.cancel();
            return cancelledOrPaused(callbacks);
        }
        if (stopped(callbacks)) {
            writer.cancel();
            return cancelledOrPaused(callbacks);
        }
        if (callbacks.publishState) {
            callbacks.publishState(StemSeparationState::Validating);
        }
        if (!writer.finish(&error)) {
            return {
                    Outcome::RetryableFailure,
                    std::move(error),
            };
        }
        if (!m_cache.registerCompleted(request.cacheEntryId,
                    protectedEntryIds(request.cacheEntryId),
                    &error)) {
            QFile::remove(*outputPath);
            return {
                    Outcome::RetryableFailure,
                    std::move(error),
            };
        }
        if (!m_alternateSourceLinker.registerCompleted(
                    request.sourceFilePath,
                    request.cacheEntryId,
                    &error)) {
            m_cache.remove(request.cacheEntryId, {}, nullptr);
            return {
                    Outcome::RetryableFailure,
                    std::move(error),
            };
        }
        return {Outcome::Completed, {}};
    } catch (const std::exception& exception) {
        if (stopped(callbacks)) {
            return cancelledOrPaused(callbacks);
        }
        QFile::remove(m_cache.partialFilePath(
                request.cacheEntryId));
        return {
                Outcome::PermanentFailure,
                QStringLiteral("Stem pipeline failed: %1")
                        .arg(QString::fromUtf8(exception.what())),
        };
    }
}

StemSeparationProcessor::Result
DemucsStemSeparationProcessor::cancelledOrPaused(
        const Callbacks& callbacks) const {
    if (callbacks.pauseRequested &&
            callbacks.pauseRequested()) {
        return {Outcome::Paused, {}};
    }
    return {Outcome::Cancelled, {}};
}

bool DemucsStemSeparationProcessor::prepareRuntime(
        const Callbacks& callbacks, QString* pErrorMessage) {
    if (!m_cacheInitialized) {
        if (!m_cache.initialize(pErrorMessage) ||
                !m_alternateSourceLinker.initialize(
                        pErrorMessage)) {
            return false;
        }
        m_cacheInitialized = true;
    }
    if (!m_modelVerified) {
        const auto expected =
                m_settings.expectedModelSha256.toLower();
        QFile model(m_settings.modelFilePath);
        if (expected.size() != 64 || !model.open(QIODevice::ReadOnly)) {
            *pErrorMessage =
                    QStringLiteral("HTDemucs model is unavailable");
            return false;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray block(kHashBlockSize, '\0');
        while (!model.atEnd()) {
            if (stopped(callbacks)) {
                return false;
            }
            const auto read =
                    model.read(block.data(), block.size());
            if (read < 0) {
                *pErrorMessage =
                        QStringLiteral("Failed to hash HTDemucs model");
                return false;
            }
            if (read > 0) {
                hash.addData(QByteArrayView(
                        block.constData(), read));
            }
        }
        if (hash.result().toHex() != expected) {
            *pErrorMessage =
                    QStringLiteral("HTDemucs model SHA-256 mismatch");
            return false;
        }
        m_modelVerified = true;
    }
    if (!m_pRunner) {
        if (m_settings.inferenceThreadCount <= 0) {
            *pErrorMessage =
                    QStringLiteral("Invalid HTDemucs thread count");
            return false;
        }
        m_pRunner = std::make_unique<DemucsOnnxRunner>(
                filesystemPath(m_settings.modelFilePath),
                m_settings.inferenceThreadCount);
    }
    return true;
}

QSet<QString> DemucsStemSeparationProcessor::protectedEntryIds(
        const QString& activeEntryId) const {
    auto result = m_protectedEntryIdsProvider
            ? m_protectedEntryIdsProvider()
            : QSet<QString>{};
    result.insert(activeEntryId);
    return result;
}

} // namespace mixxx::stems
