#include "stems/stemaudiosourcereader.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include "audio/types.h"
#include "sources/soundsourceproxy.h"
#include "stems/stemchunkpipeline.h"
#include "util/indexrange.h"
#include "util/samplebuffer.h"

namespace mixxx::stems {

std::unique_ptr<StemAudioSourceReader> StemAudioSourceReader::open(
        const TrackPointer& pTrack) {
    if (!pTrack) {
        throw std::invalid_argument(
                "Cannot open a stem audio source without a track");
    }

    AudioSource::OpenParams params;
    params.setChannelCount(audio::ChannelCount::stereo());
    params.setSampleRate(
            audio::SampleRate(StemChunkPipeline::kSampleRate));
    auto pAudioSource =
            SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pAudioSource) {
        throw std::runtime_error(
                "Mixxx could not open the track for stem separation");
    }
    const auto& signalInfo = pAudioSource->getSignalInfo();
    if (signalInfo.getChannelCount() != audio::ChannelCount::stereo() ||
            signalInfo.getSampleRate() !=
                    audio::SampleRate(StemChunkPipeline::kSampleRate)) {
        pAudioSource->close();
        throw std::runtime_error(
                "The selected decoder did not provide stereo 44.1 kHz audio");
    }
    if (pAudioSource->frameIndexRange().empty()) {
        pAudioSource->close();
        throw std::runtime_error(
                "Cannot separate an empty audio source");
    }
    return std::unique_ptr<StemAudioSourceReader>(
            new StemAudioSourceReader(std::move(pAudioSource)));
}

StemAudioSourceReader::StemAudioSourceReader(
        AudioSourcePointer pAudioSource)
        : m_pAudioSource(std::move(pAudioSource)) {
}

StemAudioSourceReader::~StemAudioSourceReader() {
    if (m_pAudioSource) {
        m_pAudioSource->close();
    }
}

std::size_t StemAudioSourceReader::frameCount() const noexcept {
    return static_cast<std::size_t>(m_pAudioSource->frameLength());
}

void StemAudioSourceReader::read(
        std::size_t frameOffset,
        std::span<float> interleavedStereo) {
    constexpr std::size_t kChannelCount = 2;
    if (interleavedStereo.size() % kChannelCount != 0) {
        throw std::invalid_argument(
                "Stem audio read buffer is not interleaved stereo");
    }
    const auto requestedFrameCount =
            interleavedStereo.size() / kChannelCount;
    if (frameOffset > frameCount() ||
            requestedFrameCount > frameCount() - frameOffset ||
            frameOffset >
                    static_cast<std::size_t>(
                            std::numeric_limits<SINT>::max()) ||
            requestedFrameCount >
                    static_cast<std::size_t>(
                            std::numeric_limits<SINT>::max())) {
        throw std::out_of_range(
                "Stem audio read exceeds the source frame range");
    }

    const auto absoluteFrameOffset =
            m_pAudioSource->frameIndexMin() +
            static_cast<SINT>(frameOffset);
    const auto requestedRange = IndexRange::forward(
            absoluteFrameOffset,
            static_cast<SINT>(requestedFrameCount));
    const auto result = m_pAudioSource->readSampleFrames(
            WritableSampleFrames(requestedRange,
                    SampleBuffer::WritableSlice(
                            interleavedStereo.data(),
                            static_cast<SINT>(
                                    interleavedStereo.size()))));
    if (result.frameIndexRange() != requestedRange ||
            result.readableLength() !=
                    static_cast<SINT>(interleavedStereo.size())) {
        throw std::runtime_error(
                "Mixxx decoder returned an incomplete stem audio block");
    }
}

} // namespace mixxx::stems
