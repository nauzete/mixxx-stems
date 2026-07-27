#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace mixxx::stems {

class StemInferenceRunner {
  public:
    static constexpr std::size_t kBatchSize = 1;
    static constexpr std::size_t kAudioChannelCount = 2;
    static constexpr std::size_t kSourceCount = 4;
    static constexpr std::size_t kDefaultSegmentSampleCount = 343980;
    static constexpr std::size_t kSegmentSampleCount =
            kDefaultSegmentSampleCount;
    static constexpr std::size_t kInputElementCount =
            kBatchSize * kAudioChannelCount * kDefaultSegmentSampleCount;
    static constexpr std::size_t kOutputElementCount =
            kBatchSize * kSourceCount * kAudioChannelCount *
            kDefaultSegmentSampleCount;

    virtual ~StemInferenceRunner() = default;

    virtual std::size_t segmentSampleCount() const noexcept {
        return kDefaultSegmentSampleCount;
    }

    std::size_t inputElementCount() const noexcept {
        return kBatchSize * kAudioChannelCount * segmentSampleCount();
    }

    std::size_t outputElementCount() const noexcept {
        return kBatchSize * kSourceCount * kAudioChannelCount *
                segmentSampleCount();
    }

    virtual std::vector<float> run(std::span<const float> input) const = 0;
};

} // namespace mixxx::stems
