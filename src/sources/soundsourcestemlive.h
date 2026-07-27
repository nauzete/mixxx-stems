#pragma once

#include <QByteArray>
#include <QFile>
#include <memory>

#include "sources/soundsourceprovider.h"
#include "stems/stemlivesessionregistry.h"
#include "util/samplebuffer.h"

namespace mixxx {

class SoundSourceProxy;

/// Progressive eight-channel stem source with an original-track fallback.
///
/// It is read only by CachingReader's worker. Missing generated frames are
/// represented as four quarter-gain copies of the original stereo signal, so
/// the default sum remains bit-for-bit close to the loaded track until a
/// separated chunk has been published.
class SoundSourceStemLive final : public SoundSource {
  public:
    explicit SoundSourceStemLive(const QUrl& url);
    ~SoundSourceStemLive() override;

    void close() override;

  protected:
    OpenResult tryOpen(
            OpenMode mode,
            const OpenParams& params) override;
    ReadableSampleFrames readSampleFramesClamped(
            const WritableSampleFrames& sampleFrames) override;

  private:
    bool copyPublishedFrames(
            SINT relativeFrameOffset,
            SINT frameCount,
            CSAMPLE* pOutput);

    std::shared_ptr<stems::StemLiveSession> m_pLiveSession;
    std::unique_ptr<SoundSourceProxy> m_pOriginalProxy;
    AudioSourcePointer m_pOriginalSource;
    audio::ChannelCount m_requestedChannelCount;
    SampleBuffer m_originalBuffer;
    QFile m_stemFile;
    QByteArray m_stemBytes;
};

class SoundSourceProviderStemLive final
        : public SoundSourceProvider {
  public:
    QString getDisplayName() const override {
        return QStringLiteral("Mixxx temporary live stems");
    }
    QStringList getSupportedFileTypes() const override {
        return {QStringLiteral("stemlive")};
    }
    SoundSourceProviderPriority getPriorityHint(
            const QString&) const override {
        return SoundSourceProviderPriority::Highest;
    }
    SoundSourcePointer newSoundSource(
            const QUrl& url) override {
        return newSoundSourceFromUrl<SoundSourceStemLive>(url);
    }
};

} // namespace mixxx
