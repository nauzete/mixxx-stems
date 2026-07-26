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
    static constexpr std::size_t kSegmentSampleCount = 343980;
    static constexpr std::size_t kInputElementCount =
            kBatchSize * kAudioChannelCount * kSegmentSampleCount;
    static constexpr std::size_t kOutputElementCount =
            kBatchSize * kSourceCount * kAudioChannelCount *
            kSegmentSampleCount;

    virtual ~StemInferenceRunner() = default;

    virtual std::vector<float> run(std::span<const float> input) const = 0;
};

} // namespace mixxx::stems
