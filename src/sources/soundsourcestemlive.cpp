#include "sources/soundsourcestemlive.h"

#include <QFileInfo>
#include <QtEndian>
#include <algorithm>

#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/sample.h"

namespace mixxx {
namespace {

constexpr auto kStemChannelCount =
        stems::StemTemporaryStore::kStemChannelCount;
constexpr auto kBytesPerStemFrame =
        kStemChannelCount * sizeof(qint16);

} // namespace

SoundSourceStemLive::SoundSourceStemLive(const QUrl& url)
        : SoundSource(url) {
}

SoundSourceStemLive::~SoundSourceStemLive() = default;

SoundSource::OpenResult SoundSourceStemLive::tryOpen(
        OpenMode,
        const OpenParams& params) {
    const auto sessionId =
            QFileInfo(getLocalFileName()).completeBaseName();
    m_pLiveSession =
            stems::StemLiveSessionRegistry::find(sessionId);
    if (!m_pLiveSession || !m_pLiveSession->track()) {
        return OpenResult::Failed;
    }
    m_pOriginalProxy = std::make_unique<::SoundSourceProxy>(
            m_pLiveSession->track());
    m_pOriginalSource = m_pOriginalProxy->openAudioSource(
            OpenParams(audio::ChannelCount::stereo(),
                    audio::SampleRate::fromDouble(44100.0)));
    if (!m_pOriginalSource) {
        close();
        return OpenResult::Failed;
    }

    m_requestedChannelCount =
            params.getSignalInfo().getChannelCount() ==
                    audio::ChannelCount::stereo()
            ? audio::ChannelCount::stereo()
            : audio::ChannelCount::stem();
    initChannelCountOnce(m_requestedChannelCount);
    initSampleRateOnce(
            m_pOriginalSource->getSignalInfo().getSampleRate());
    initBitrateOnce(m_pOriginalSource->getBitrate());
    initFrameIndexRangeOnce(
            m_pOriginalSource->frameIndexRange());
    return OpenResult::Succeeded;
}

void SoundSourceStemLive::close() {
    m_stemFile.close();
    if (m_pOriginalSource) {
        m_pOriginalSource->close();
    }
    m_pOriginalSource.reset();
    m_pOriginalProxy.reset();
    m_pLiveSession.reset();
    m_requestedChannelCount = audio::ChannelCount();
}

ReadableSampleFrames
SoundSourceStemLive::readSampleFramesClamped(
        const WritableSampleFrames& sampleFrames) {
    if (!m_pOriginalSource ||
            !m_requestedChannelCount.isValid()) {
        return {};
    }
    const auto frameCount = sampleFrames.frameLength();
    if (frameCount <= 0) {
        return {};
    }
    if (m_requestedChannelCount ==
            audio::ChannelCount::stereo()) {
        return m_pOriginalSource->readSampleFrames(
                sampleFrames);
    }
    if (sampleFrames.writableLength() !=
            frameCount *
                    static_cast<SINT>(kStemChannelCount)) {
        return {};
    }

    const auto originalSampleCount =
            frameCount *
            audio::ChannelCount::stereo();
    if (m_originalBuffer.size() < originalSampleCount) {
        m_originalBuffer =
                SampleBuffer(originalSampleCount);
    }
    WritableSampleFrames originalFrames(
            sampleFrames.frameIndexRange(),
            SampleBuffer::WritableSlice(
                    m_originalBuffer.data(),
                    originalSampleCount));
    const auto original =
            m_pOriginalSource->readSampleFrames(
                    originalFrames);
    auto* const pOutput = sampleFrames.writableData();
    SampleUtil::clear(
            pOutput, sampleFrames.writableLength());
    const auto readableFrames =
            original.readableLength() /
            audio::ChannelCount::stereo();
    for (SINT frame = 0; frame < readableFrames; ++frame) {
        for (std::size_t stem = 0; stem < 4; ++stem) {
            pOutput[frame * kStemChannelCount +
                    stem * 2] =
                    original.readableData()[frame * 2] *
                    0.25F;
            pOutput[frame * kStemChannelCount +
                    stem * 2 + 1] =
                    original.readableData()[frame * 2 + 1] *
                    0.25F;
        }
    }
    const auto relativeFrameOffset =
            sampleFrames.frameIndexRange().start() -
            frameIndexMin();
    copyPublishedFrames(
            relativeFrameOffset,
            readableFrames,
            pOutput);
    return ReadableSampleFrames(
            IndexRange::forward(
                    sampleFrames.frameIndexRange().start(),
                    readableFrames),
            SampleBuffer::ReadableSlice(
                    pOutput,
                    readableFrames *
                            static_cast<SINT>(
                                    kStemChannelCount)));
}

bool SoundSourceStemLive::copyPublishedFrames(
        SINT relativeFrameOffset,
        SINT frameCount,
        CSAMPLE* pOutput) {
    if (relativeFrameOffset < 0 || frameCount <= 0) {
        return false;
    }
    const auto pTemporary =
            m_pLiveSession->temporarySession();
    if (!pTemporary) {
        return false;
    }
    const auto readyFrameCount =
            pTemporary->readyFrameCount.load(
                    std::memory_order_acquire);
    const auto start =
            static_cast<std::size_t>(relativeFrameOffset);
    if (start >= readyFrameCount) {
        return false;
    }
    const auto readableFrameCount = std::min(
            static_cast<std::size_t>(frameCount),
            readyFrameCount - start);
    if (m_stemFile.fileName() !=
            pTemporary->filePath) {
        m_stemFile.close();
        m_stemFile.setFileName(
                pTemporary->filePath);
    }
    if ((!m_stemFile.isOpen() &&
                !m_stemFile.open(QIODevice::ReadOnly)) ||
            !m_stemFile.seek(
                    static_cast<qint64>(
                            start * kBytesPerStemFrame))) {
        return false;
    }
    const auto byteCount = static_cast<qsizetype>(
            readableFrameCount *
            kBytesPerStemFrame);
    m_stemBytes.resize(byteCount);
    if (m_stemFile.read(
                m_stemBytes.data(), byteCount) !=
            byteCount) {
        return false;
    }
    const auto* const pcm =
            reinterpret_cast<const uchar*>(
                    m_stemBytes.constData());
    for (std::size_t sample = 0;
            sample <
            readableFrameCount *
                    kStemChannelCount;
            ++sample) {
        pOutput[sample] =
                static_cast<float>(
                        qFromLittleEndian<qint16>(
                                pcm +
                                sample *
                                        sizeof(qint16))) /
                32767.0F;
    }
    return true;
}

} // namespace mixxx
