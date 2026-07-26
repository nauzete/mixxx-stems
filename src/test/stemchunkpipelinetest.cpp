#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "stems/stemchunkpipeline.h"

namespace mixxx::stems {
namespace {

constexpr std::size_t kChannelCount =
        StemInferenceRunner::kAudioChannelCount;
constexpr std::size_t kSourceCount = StemInferenceRunner::kSourceCount;
constexpr std::size_t kPlaneCount = kChannelCount * kSourceCount;
constexpr std::size_t kSegmentFrameCount =
        StemInferenceRunner::kSegmentSampleCount;

class IdentityRunner final : public StemInferenceRunner {
  public:
    std::vector<float> run(std::span<const float> input) const override {
        ++m_runCount;
        std::vector<float> output(kOutputElementCount);
        for (std::size_t source = 0; source < kSourceCount; ++source) {
            for (std::size_t channel = 0; channel < kChannelCount;
                    ++channel) {
                const auto inputOffset = channel * kSegmentFrameCount;
                const auto outputOffset =
                        (source * kChannelCount + channel) *
                        kSegmentFrameCount;
                std::copy_n(input.begin() + inputOffset,
                        kSegmentFrameCount,
                        output.begin() + outputOffset);
            }
        }
        return output;
    }

    mutable std::size_t m_runCount = 0;
};

std::vector<float> makeStereo(std::size_t frameCount) {
    std::vector<float> audio(frameCount * kChannelCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const auto time =
                static_cast<double>(frame) /
                static_cast<double>(StemChunkPipeline::kSampleRate);
        audio[frame * kChannelCount] = static_cast<float>(
                0.2 * std::sin(2.0 * std::numbers::pi * 220.0 * time) +
                0.03);
        audio[frame * kChannelCount + 1] = static_cast<float>(
                0.15 * std::sin(2.0 * std::numbers::pi * 330.0 * time) -
                0.02);
    }
    return audio;
}

TEST(StemChunkPipelineTest, ReconstructsAcrossOverlapAndFinalPadding) {
    const auto frameCount =
            StemChunkPipeline::kStrideSampleCount + 2001;
    const auto input = makeStereo(frameCount);
    std::vector<float> output(kPlaneCount * frameCount);
    std::vector<float> progressValues;
    std::size_t expectedWriteOffset = 0;
    std::size_t maximumReadFrameCount = 0;
    IdentityRunner runner;
    StemChunkPipeline pipeline(runner);
    const auto capacityBefore = pipeline.allocatedSampleCapacity();

    const auto result = pipeline.run(
            frameCount,
            [&](std::size_t offset, std::span<float> destination) {
                ASSERT_EQ(destination.size() % kChannelCount, 0U);
                const auto readFrameCount =
                        destination.size() / kChannelCount;
                maximumReadFrameCount =
                        std::max(maximumReadFrameCount, readFrameCount);
                ASSERT_LE(offset + readFrameCount, frameCount);
                std::copy_n(input.begin() + offset * kChannelCount,
                        destination.size(),
                        destination.begin());
            },
            [&](std::size_t offset,
                    std::size_t writeFrameCount,
                    std::span<const float> stems) {
                ASSERT_EQ(offset, expectedWriteOffset);
                ASSERT_EQ(stems.size(), kPlaneCount * writeFrameCount);
                for (std::size_t plane = 0; plane < kPlaneCount; ++plane) {
                    std::copy_n(stems.begin() + plane * writeFrameCount,
                            writeFrameCount,
                            output.begin() + plane * frameCount + offset);
                }
                expectedWriteOffset += writeFrameCount;
            },
            [&](float progress) { progressValues.push_back(progress); });

    EXPECT_EQ(result, StemChunkPipeline::Result::Completed);
    EXPECT_EQ(expectedWriteOffset, frameCount);
    EXPECT_EQ(runner.m_runCount, 2U);
    EXPECT_LE(maximumReadFrameCount, kSegmentFrameCount);
    EXPECT_EQ(pipeline.allocatedSampleCapacity(), capacityBefore);
    ASSERT_FALSE(progressValues.empty());
    EXPECT_TRUE(std::is_sorted(progressValues.begin(), progressValues.end()));
    EXPECT_FLOAT_EQ(progressValues.back(), 1.0F);

    float maximumError = 0.0F;
    for (std::size_t source = 0; source < kSourceCount; ++source) {
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            const auto plane = source * kChannelCount + channel;
            for (std::size_t frame = 0; frame < frameCount; ++frame) {
                maximumError =
                        std::max(maximumError,
                                std::abs(output[plane * frameCount + frame] -
                                        input[frame * kChannelCount + channel]));
            }
        }
    }
    EXPECT_LE(maximumError, 1e-5F);
}

TEST(StemChunkPipelineTest, CancelsBeforeInference) {
    const auto frameCount = StemChunkPipeline::kStatisticsBlockSampleCount;
    const auto input = makeStereo(frameCount);
    IdentityRunner runner;
    StemChunkPipeline pipeline(runner);
    bool cancel = false;

    const auto result = pipeline.run(
            frameCount,
            [&](std::size_t offset, std::span<float> destination) {
                std::copy_n(input.begin() + offset * kChannelCount,
                        destination.size(),
                        destination.begin());
            },
            [](std::size_t, std::size_t, std::span<const float>) {
                throw std::logic_error("Cancelled pipeline wrote output");
            },
            [&](float progress) { cancel = progress >= 0.1F; },
            [&] { return cancel; });

    EXPECT_EQ(result, StemChunkPipeline::Result::Cancelled);
    EXPECT_EQ(runner.m_runCount, 0U);
}

TEST(StemChunkPipelineTest, EmptyInputCompletesWithoutCallbacks) {
    IdentityRunner runner;
    StemChunkPipeline pipeline(runner);
    float progress = 0.0F;

    const auto result =
            pipeline.run(0, {}, {}, [&](float value) {
                progress = value;
            });

    EXPECT_EQ(result, StemChunkPipeline::Result::Completed);
    EXPECT_FLOAT_EQ(progress, 1.0F);
    EXPECT_EQ(runner.m_runCount, 0U);
}

} // namespace
} // namespace mixxx::stems
