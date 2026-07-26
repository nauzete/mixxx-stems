#include "stems/stemchunkpipeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mixxx::stems {
namespace {

constexpr std::size_t kChannelCount =
        StemInferenceRunner::kAudioChannelCount;
constexpr std::size_t kSourceCount = StemInferenceRunner::kSourceCount;
constexpr std::size_t kSegmentFrameCount =
        StemInferenceRunner::kSegmentSampleCount;
constexpr std::size_t kPlaneCount = kSourceCount * kChannelCount;
constexpr double kNormalizationEpsilon = 1e-8;

bool isCancelled(const StemChunkPipeline::CancelCallback& cancelled) {
    return cancelled && cancelled();
}

void reportProgress(
        const StemChunkPipeline::ProgressCallback& callback,
        float value) {
    if (callback) {
        callback(std::clamp(value, 0.0F, 1.0F));
    }
}

} // namespace

StemChunkPipeline::StemChunkPipeline(const StemInferenceRunner& runner)
        : m_runner(runner),
          m_interleavedInput(kChannelCount * kSegmentFrameCount),
          m_planarInput(StemInferenceRunner::kInputElementCount),
          m_accumulator(kPlaneCount * kSegmentFrameCount),
          m_weightSum(kSegmentFrameCount),
          m_emitBuffer(kPlaneCount * kStrideSampleCount),
          m_weight(kSegmentFrameCount) {
    const auto midpoint = kSegmentFrameCount / 2;
    const auto maximum = static_cast<float>(midpoint);
    for (std::size_t frame = 0; frame < kSegmentFrameCount; ++frame) {
        const auto rising = frame < midpoint;
        const auto distance = rising ? frame + 1 : kSegmentFrameCount - frame;
        m_weight[frame] = static_cast<float>(distance) / maximum;
    }
}

StemChunkPipeline::Result StemChunkPipeline::run(
        std::size_t totalFrameCount,
        const ReadCallback& read,
        const WriteCallback& write,
        const ProgressCallback& progress,
        const CancelCallback& cancelled) {
    if (totalFrameCount == 0) {
        reportProgress(progress, 1.0F);
        return Result::Completed;
    }
    if (!read || !write) {
        throw std::invalid_argument(
                "Stem chunk pipeline requires read and write callbacks");
    }

    std::fill(m_accumulator.begin(), m_accumulator.end(), 0.0F);
    std::fill(m_weightSum.begin(), m_weightSum.end(), 0.0F);
    bool statisticsCancelled = false;
    const auto statistics = calculateStatistics(totalFrameCount,
            read,
            progress,
            cancelled,
            &statisticsCancelled);
    if (statisticsCancelled) {
        return Result::Cancelled;
    }

    const auto segmentCount =
            (totalFrameCount + kStrideSampleCount - 1) /
            kStrideSampleCount;
    std::size_t emittedFrameCount = 0;
    for (std::size_t segmentIndex = 0; segmentIndex < segmentCount;
            ++segmentIndex) {
        if (isCancelled(cancelled)) {
            return Result::Cancelled;
        }

        const auto segmentOffset = segmentIndex * kStrideSampleCount;
        const auto chunkFrameCount =
                std::min(kSegmentFrameCount,
                        totalFrameCount - segmentOffset);
        prepareInput(segmentOffset,
                chunkFrameCount,
                totalFrameCount,
                statistics,
                read);
        const auto output = m_runner.run(m_planarInput);
        if (output.size() != StemInferenceRunner::kOutputElementCount) {
            throw std::runtime_error(
                    "Stem inference returned an unexpected output size");
        }
        if (isCancelled(cancelled)) {
            return Result::Cancelled;
        }

        const auto trimOffset =
                (kSegmentFrameCount - chunkFrameCount) / 2;
        addSegment(output, trimOffset, chunkFrameCount, statistics);

        const auto finalSegment =
                segmentOffset + kStrideSampleCount >= totalFrameCount;
        const auto emitFrameCount =
                finalSegment ? totalFrameCount - emittedFrameCount
                             : kStrideSampleCount;
        emit(emittedFrameCount, emitFrameCount, write);
        emittedFrameCount += emitFrameCount;
        if (!finalSegment) {
            shiftAccumulator();
        }

        const auto inferenceFraction =
                static_cast<float>(segmentIndex + 1) /
                static_cast<float>(segmentCount);
        reportProgress(progress, 0.1F + 0.9F * inferenceFraction);
    }
    return Result::Completed;
}

StemChunkPipeline::Statistics StemChunkPipeline::calculateStatistics(
        std::size_t totalFrameCount,
        const ReadCallback& read,
        const ProgressCallback& progress,
        const CancelCallback& cancelled,
        bool* pCancelled) {
    double mean = 0.0;
    double squaredDistanceSum = 0.0;
    std::size_t sampleCount = 0;
    for (std::size_t offset = 0; offset < totalFrameCount;
            offset += kStatisticsBlockSampleCount) {
        if (isCancelled(cancelled)) {
            *pCancelled = true;
            return {};
        }
        const auto frameCount =
                std::min(kStatisticsBlockSampleCount,
                        totalFrameCount - offset);
        auto block = std::span(m_interleavedInput)
                             .first(frameCount * kChannelCount);
        read(offset, block);
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            const auto monoReference =
                    (static_cast<double>(block[frame * kChannelCount]) +
                            static_cast<double>(
                                    block[frame * kChannelCount + 1])) /
                    2.0;
            ++sampleCount;
            const auto delta = monoReference - mean;
            mean += delta / static_cast<double>(sampleCount);
            const auto adjustedDelta = monoReference - mean;
            squaredDistanceSum += delta * adjustedDelta;
        }
        reportProgress(progress,
                0.1F * static_cast<float>(offset + frameCount) /
                        static_cast<float>(totalFrameCount));
    }

    const auto variance = sampleCount > 1
            ? squaredDistanceSum / static_cast<double>(sampleCount - 1)
            : 0.0;
    return {
            mean,
            std::sqrt(std::max(variance, 0.0)) + kNormalizationEpsilon,
    };
}

void StemChunkPipeline::prepareInput(
        std::size_t segmentOffset,
        std::size_t chunkFrameCount,
        std::size_t totalFrameCount,
        const Statistics& statistics,
        const ReadCallback& read) {
    std::fill(m_interleavedInput.begin(), m_interleavedInput.end(), 0.0F);
    std::fill(m_planarInput.begin(), m_planarInput.end(), 0.0F);

    const auto padding = kSegmentFrameCount - chunkFrameCount;
    const auto desiredStart =
            static_cast<std::ptrdiff_t>(segmentOffset) -
            static_cast<std::ptrdiff_t>(padding / 2);
    const auto sourceStart =
            static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, desiredStart));
    const auto destinationStart =
            static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(sourceStart) - desiredStart);
    const auto readableFrameCount =
            std::min(kSegmentFrameCount - destinationStart,
                    totalFrameCount - sourceStart);
    auto destination =
            std::span(m_interleavedInput)
                    .subspan(destinationStart * kChannelCount,
                            readableFrameCount * kChannelCount);
    read(sourceStart, destination);

    const auto scale = static_cast<float>(statistics.standardDeviation);
    const auto mean = static_cast<float>(statistics.mean);
    for (std::size_t frame = destinationStart;
            frame < destinationStart + readableFrameCount;
            ++frame) {
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            m_planarInput[channel * kSegmentFrameCount + frame] =
                    (m_interleavedInput[frame * kChannelCount + channel] -
                            mean) /
                    scale;
        }
    }
}

void StemChunkPipeline::addSegment(
        std::span<const float> output,
        std::size_t outputOffset,
        std::size_t chunkFrameCount,
        const Statistics& statistics) {
    const auto scale = static_cast<float>(statistics.standardDeviation);
    const auto mean = static_cast<float>(statistics.mean);
    for (std::size_t frame = 0; frame < chunkFrameCount; ++frame) {
        const auto weight = m_weight[frame];
        m_weightSum[frame] += weight;
        for (std::size_t plane = 0; plane < kPlaneCount; ++plane) {
            const auto sample =
                    output[plane * kSegmentFrameCount + outputOffset + frame] *
                            scale +
                    mean;
            m_accumulator[plane * kSegmentFrameCount + frame] +=
                    weight * sample;
        }
    }
}

void StemChunkPipeline::emit(
        std::size_t frameOffset,
        std::size_t frameCount,
        const WriteCallback& write) {
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if (!(m_weightSum[frame] >
                    std::numeric_limits<float>::epsilon())) {
            throw std::runtime_error(
                    "Stem overlap-add produced an uncovered sample");
        }
        for (std::size_t plane = 0; plane < kPlaneCount; ++plane) {
            m_emitBuffer[plane * frameCount + frame] =
                    m_accumulator[plane * kSegmentFrameCount + frame] /
                    m_weightSum[frame];
        }
    }
    write(frameOffset,
            frameCount,
            std::span(m_emitBuffer).first(kPlaneCount * frameCount));
}

void StemChunkPipeline::shiftAccumulator() {
    constexpr auto retainedFrameCount =
            kSegmentFrameCount - kStrideSampleCount;
    for (std::size_t plane = 0; plane < kPlaneCount; ++plane) {
        auto planeData =
                std::span(m_accumulator)
                        .subspan(plane * kSegmentFrameCount,
                                kSegmentFrameCount);
        std::move(planeData.begin() + kStrideSampleCount,
                planeData.end(),
                planeData.begin());
        std::fill(planeData.begin() + retainedFrameCount,
                planeData.end(),
                0.0F);
    }
    std::move(m_weightSum.begin() + kStrideSampleCount,
            m_weightSum.end(),
            m_weightSum.begin());
    std::fill(m_weightSum.begin() + retainedFrameCount,
            m_weightSum.end(),
            0.0F);
}

std::size_t StemChunkPipeline::allocatedSampleCapacity() const noexcept {
    return m_interleavedInput.size() + m_planarInput.size() +
            m_accumulator.size() + m_weightSum.size() +
            m_emitBuffer.size() + m_weight.size();
}

} // namespace mixxx::stems
