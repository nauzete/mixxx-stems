#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "sources/audiosource.h"
#include "track/track_decl.h"

namespace mixxx::stems {

/// Opens a Mixxx decoder as stereo 44.1 kHz and exposes bounded random reads
/// for StemChunkPipeline.
///
/// Construction and read() perform file I/O and decoding. Instances belong on
/// the stem background worker and must never be used from the audio thread.
class StemAudioSourceReader final {
  public:
    static std::unique_ptr<StemAudioSourceReader> open(
            const TrackPointer& pTrack);

    ~StemAudioSourceReader();

    StemAudioSourceReader(const StemAudioSourceReader&) = delete;
    StemAudioSourceReader& operator=(const StemAudioSourceReader&) = delete;

    std::size_t frameCount() const noexcept;
    void read(std::size_t frameOffset, std::span<float> interleavedStereo);

  private:
    explicit StemAudioSourceReader(AudioSourcePointer pAudioSource);

    AudioSourcePointer m_pAudioSource;
};

} // namespace mixxx::stems
