#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <QByteArray>
#include <QString>
#include <QtTypes>

namespace mixxx::stems {

/// Incrementally encodes four stereo stems into a Native Instruments STEM MP4.
///
/// All methods perform file I/O, allocations, and codec work. Instances must
/// only be used from a background worker, never from Mixxx's audio thread.
class StemContainerWriter final {
  public:
    static constexpr int kSampleRate = 44100;
    static constexpr int kStemCount = 4;
    static constexpr int kChannelCount = 2;
    static constexpr int kAudioStreamCount = kStemCount + 1;
    static constexpr int kDefaultBitRatePerStream = 128000;
    static constexpr qint64 kDefaultMaximumOutputBytes =
            256LL * 1024LL * 1024LL;

    struct Settings {
        int bitRatePerStream = kDefaultBitRatePerStream;
        qint64 maximumOutputBytes = kDefaultMaximumOutputBytes;
    };

    explicit StemContainerWriter(
            QString outputFilePath,
            Settings settings = {});
    ~StemContainerWriter();

    StemContainerWriter(const StemContainerWriter&) = delete;
    StemContainerWriter& operator=(const StemContainerWriter&) = delete;

    bool open(QString* pErrorMessage = nullptr);

    /// Writes packed [stem, channel, frame] floating-point samples.
    ///
    /// frameOffset must be contiguous with all previous writes. The input span
    /// remains owned by the caller and is fully consumed before this method
    /// returns.
    bool write(std::size_t frameOffset,
            std::size_t frameCount,
            std::span<const float> planarStems,
            QString* pErrorMessage = nullptr);

    /// Flushes, writes the STEM manifest, validates, and atomically publishes.
    bool finish(QString* pErrorMessage = nullptr);

    /// Abandons the encode and removes the exact .partial output.
    void cancel();

    QString outputFilePath() const;
    QString partialFilePath() const;
    qint64 writtenFrameCount() const noexcept;

    static QByteArray manifest();
    static qint64 estimatedEncodedBytes(
            qint64 frameCount,
            int bitRatePerStream = kDefaultBitRatePerStream);

  private:
    class Impl;
    std::unique_ptr<Impl> m_pImpl;
};

} // namespace mixxx::stems
