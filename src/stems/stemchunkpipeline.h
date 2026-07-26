#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "stems/steminferencerunner.h"

namespace mixxx::stems {

/// Applies HTDemucs to a stereo 44.1 kHz source using bounded, overlapping
/// segments.
///
/// run() performs decoding callbacks, model inference, and allocations. It is
/// intended exclusively for a low-priority background worker and must never be
/// called from Mixxx's real-time audio thread.
class StemChunkPipeline final {
  public:
    static constexpr std::size_t kSampleRate = 44100;
    static constexpr float kOverlap = 0.25F;
    static constexpr std::size_t kStrideSampleCount =
            static_cast<std::size_t>(
                    StemInferenceRunner::kSegmentSampleCount *
                    (1.0F - kOverlap));
    static constexpr std::size_t kStatisticsBlockSampleCount = 65536;

    enum class Result {
        Completed,
        Cancelled,
    };

    /// Reads interleaved stereo frames at an absolute frame offset.
    ///
    /// The callback must fill the complete destination or throw. It may perform
    /// file I/O and decoding because run() is restricted to a background
    /// worker.
    using ReadCallback = std::function<void(
            std::size_t frameOffset,
            std::span<float> interleavedStereo)>;

    /// Consumes a packed [source, channel, sample] output chunk synchronously.
    ///
    /// The span remains valid only until the callback returns.
    using WriteCallback = std::function<void(
            std::size_t frameOffset,
            std::size_t frameCount,
            std::span<const float> planarStems)>;

    using ProgressCallback = std::function<void(float progress)>;
    using CancelCallback = std::function<bool()>;

    explicit StemChunkPipeline(const StemInferenceRunner& runner);

    Result run(std::size_t totalFrameCount,
            const ReadCallback& read,
            const WriteCallback& write,
            const ProgressCallback& progress = {},
            const CancelCallback& cancelled = {});

    std::size_t allocatedSampleCapacity() const noexcept;

  private:
    struct Statistics {
        double mean = 0.0;
        double standardDeviation = 1.0;
    };

    Statistics calculateStatistics(std::size_t totalFrameCount,
            const ReadCallback& read,
            const ProgressCallback& progress,
            const CancelCallback& cancelled,
            bool* pCancelled);
    void prepareInput(std::size_t segmentOffset,
            std::size_t chunkFrameCount,
            std::size_t totalFrameCount,
            const Statistics& statistics,
            const ReadCallback& read);
    void addSegment(std::span<const float> output,
            std::size_t outputOffset,
            std::size_t chunkFrameCount,
            const Statistics& statistics);
    void emit(std::size_t frameOffset,
            std::size_t frameCount,
            const WriteCallback& write);
    void shiftAccumulator();

    const StemInferenceRunner& m_runner;
    std::vector<float> m_interleavedInput;
    std::vector<float> m_planarInput;
    std::vector<float> m_accumulator;
    std::vector<float> m_weightSum;
    std::vector<float> m_emitBuffer;
    std::vector<float> m_weight;
};

} // namespace mixxx::stems
