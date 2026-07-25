#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mixxx::stems {

/// Owns a CPU ONNX Runtime session for the exported HTDemucs model.
///
/// Construction and run() perform blocking work and allocate memory. They must
/// only be called from a background worker, never from the real-time audio
/// thread.
class DemucsOnnxRunner final {
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

    struct TensorContract {
        std::string inputName;
        std::string outputName;
        std::vector<int64_t> inputShape;
        std::vector<int64_t> declaredOutputShape;
    };

    explicit DemucsOnnxRunner(
            const std::filesystem::path& modelPath,
            int intraOpThreadCount = 1);
    ~DemucsOnnxRunner();

    DemucsOnnxRunner(const DemucsOnnxRunner&) = delete;
    DemucsOnnxRunner& operator=(const DemucsOnnxRunner&) = delete;
    DemucsOnnxRunner(DemucsOnnxRunner&&) = delete;
    DemucsOnnxRunner& operator=(DemucsOnnxRunner&&) = delete;

    const TensorContract& contract() const noexcept;

    /// Runs one normalized stereo HTDemucs segment.
    ///
    /// Input layout is [batch, channel, sample]. Output layout is
    /// [batch, source, channel, sample], with source order:
    /// drums, bass, other, vocals.
    std::vector<float> run(std::span<const float> input) const;

    static std::string runtimeVersion();

  private:
    class Impl;
    std::unique_ptr<Impl> m_pImpl;
};

} // namespace mixxx::stems
