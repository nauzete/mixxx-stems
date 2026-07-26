#include "stems/stemcontainerwriter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include "track/steminfoimporter.h"
#include "util/logger.h"

namespace mixxx::stems {
namespace {

const Logger kLogger("StemContainerWriter");
constexpr qsizetype kFileMoveBlockSize = 1024 * 1024;

QString ffmpegError(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return QString::fromUtf8(buffer.data());
}

bool fail(QString* pErrorMessage, QString message) {
    kLogger.warning().noquote() << message;
    if (pErrorMessage) {
        *pErrorMessage = std::move(message);
    }
    return false;
}

struct Mp4Box {
    quint64 offset = 0;
    quint64 size = 0;
    quint64 headerSize = 0;
    QByteArray type;

    quint64 end() const {
        return offset + size;
    }
};

bool readBox(QFile* pFile, quint64 offset, quint64 parentEnd, Mp4Box* pBox) {
    if (offset + 8 > parentEnd || !pFile->seek(static_cast<qint64>(offset))) {
        return false;
    }
    const auto header = pFile->read(8);
    if (header.size() != 8) {
        return false;
    }
    const auto* data =
            reinterpret_cast<const uchar*>(header.constData());
    quint64 size = qFromBigEndian<quint32>(data);
    quint64 headerSize = 8;
    if (size == 1) {
        const auto extended = pFile->read(8);
        if (extended.size() != 8) {
            return false;
        }
        size = qFromBigEndian<quint64>(
                reinterpret_cast<const uchar*>(extended.constData()));
        headerSize = 16;
    } else if (size == 0) {
        size = parentEnd - offset;
    }
    if (size < headerSize || size > parentEnd - offset) {
        return false;
    }
    *pBox = {
            offset,
            size,
            headerSize,
            header.sliced(4, 4),
    };
    return true;
}

bool findChild(QFile* pFile,
        quint64 begin,
        quint64 end,
        const QByteArray& type,
        Mp4Box* pResult) {
    auto offset = begin;
    while (offset + 8 <= end) {
        Mp4Box box;
        if (!readBox(pFile, offset, end, &box)) {
            return false;
        }
        if (box.type == type) {
            *pResult = box;
            return true;
        }
        offset = box.end();
    }
    return false;
}

QByteArray makeBox(const QByteArray& type, const QByteArray& contents) {
    const auto size = 8LL + contents.size();
    if (type.size() != 4 ||
            size > std::numeric_limits<quint32>::max()) {
        return {};
    }
    QByteArray box(static_cast<qsizetype>(size), '\0');
    qToBigEndian(
            static_cast<quint32>(size),
            reinterpret_cast<uchar*>(box.data()));
    std::copy(type.cbegin(), type.cend(), box.begin() + 4);
    std::copy(contents.cbegin(), contents.cend(), box.begin() + 8);
    return box;
}

bool patchBoxSize(
        QFile* pFile, const Mp4Box& box, quint64 growth, QString* pErrorMessage) {
    if (box.headerSize != 8 ||
            box.size + growth > std::numeric_limits<quint32>::max()) {
        return fail(pErrorMessage,
                QStringLiteral("Unsupported extended MP4 box while writing STEM manifest"));
    }
    std::array<uchar, sizeof(quint32)> size{};
    qToBigEndian(static_cast<quint32>(box.size + growth), size.data());
    if (!pFile->seek(static_cast<qint64>(box.offset)) ||
            pFile->write(reinterpret_cast<const char*>(size.data()),
                    size.size()) !=
                    static_cast<qint64>(size.size())) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to update MP4 box size"));
    }
    return true;
}

bool insertBytes(QFile* pFile,
        quint64 offset,
        const QByteArray& bytes,
        QString* pErrorMessage) {
    const auto oldSize = static_cast<quint64>(pFile->size());
    const auto growth = static_cast<quint64>(bytes.size());
    if (offset > oldSize ||
            oldSize + growth >
                    static_cast<quint64>(
                            std::numeric_limits<qint64>::max()) ||
            !pFile->resize(static_cast<qint64>(oldSize + growth))) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to extend partial STEM container"));
    }

    auto remaining = oldSize - offset;
    while (remaining > 0) {
        const auto blockSize =
                static_cast<qsizetype>(
                        std::min<quint64>(remaining, kFileMoveBlockSize));
        const auto sourceOffset = offset + remaining - blockSize;
        if (!pFile->seek(static_cast<qint64>(sourceOffset))) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to seek while extending MP4 metadata"));
        }
        const auto block = pFile->read(blockSize);
        if (block.size() != blockSize ||
                !pFile->seek(
                        static_cast<qint64>(sourceOffset + growth)) ||
                pFile->write(block) != blockSize) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to move MP4 data while inserting STEM manifest"));
        }
        remaining -= static_cast<quint64>(blockSize);
    }
    if (!pFile->seek(static_cast<qint64>(offset)) ||
            pFile->write(bytes) != bytes.size()) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to write STEM manifest atom"));
    }
    return true;
}

bool injectManifest(const QString& filePath,
        const QByteArray& manifest,
        QString* pErrorMessage) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to open partial container for STEM metadata: %1")
                        .arg(file.errorString()));
    }

    Mp4Box moov;
    if (!findChild(&file,
                0,
                static_cast<quint64>(file.size()),
                QByteArrayLiteral("moov"),
                &moov)) {
        return fail(pErrorMessage,
                QStringLiteral("Encoded MP4 has no moov box"));
    }

    const auto stem = makeBox(QByteArrayLiteral("stem"), manifest);
    if (stem.isEmpty()) {
        return fail(pErrorMessage,
                QStringLiteral("STEM manifest is too large"));
    }

    Mp4Box udta;
    const bool hasUdta = findChild(&file,
            moov.offset + moov.headerSize,
            moov.end(),
            QByteArrayLiteral("udta"),
            &udta);
    const auto bytes = hasUdta
            ? stem
            : makeBox(QByteArrayLiteral("udta"), stem);
    const auto insertionOffset = hasUdta ? udta.end() : moov.end();
    if (bytes.isEmpty() ||
            !insertBytes(&file, insertionOffset, bytes, pErrorMessage) ||
            (hasUdta &&
                    !patchBoxSize(
                            &file, udta, bytes.size(), pErrorMessage)) ||
            !patchBoxSize(&file, moov, bytes.size(), pErrorMessage)) {
        return false;
    }
    if (!file.flush()) {
        return fail(pErrorMessage,
                QStringLiteral("Failed to flush STEM manifest"));
    }
    file.close();
    return true;
}

bool validateContainer(const QString& filePath,
        qint64 maximumOutputBytes,
        QString* pErrorMessage) {
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.isFile() || fileInfo.size() <= 0) {
        return fail(pErrorMessage,
                QStringLiteral("Encoded STEM container is empty"));
    }
    if (fileInfo.size() > maximumOutputBytes) {
        return fail(pErrorMessage,
                QStringLiteral("Encoded STEM container exceeds the per-track size guard"));
    }

    AVFormatContext* pInput = nullptr;
    const auto encodedPath = QFile::encodeName(filePath);
    auto result =
            avformat_open_input(&pInput, encodedPath.constData(), nullptr, nullptr);
    if (result < 0) {
        return fail(pErrorMessage,
                QStringLiteral("FFmpeg could not reopen encoded STEM container: %1")
                        .arg(ffmpegError(result)));
    }
    const auto closeInput = [&] {
        avformat_close_input(&pInput);
    };
    result = avformat_find_stream_info(pInput, nullptr);
    if (result < 0) {
        closeInput();
        return fail(pErrorMessage,
                QStringLiteral("FFmpeg could not inspect encoded STEM streams: %1")
                        .arg(ffmpegError(result)));
    }

    int audioStreamCount = 0;
    AVCodecID stemCodec = AV_CODEC_ID_NONE;
    int stemSampleRate = 0;
    for (unsigned int index = 0; index < pInput->nb_streams; ++index) {
        const auto* pParameters = pInput->streams[index]->codecpar;
        if (pParameters->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        ++audioStreamCount;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        const auto channelCount = pParameters->ch_layout.nb_channels;
#else
        const auto channelCount = pParameters->channels;
#endif
        if (channelCount != StemContainerWriter::kChannelCount) {
            closeInput();
            return fail(pErrorMessage,
                    QStringLiteral("Encoded STEM stream is not stereo"));
        }
        if (audioStreamCount == 1) {
            continue;
        }
        if (stemCodec == AV_CODEC_ID_NONE) {
            stemCodec = pParameters->codec_id;
            stemSampleRate = pParameters->sample_rate;
        } else if (pParameters->codec_id != stemCodec ||
                pParameters->sample_rate != stemSampleRate) {
            closeInput();
            return fail(pErrorMessage,
                    QStringLiteral("Encoded stem streams have inconsistent formats"));
        }
    }
    closeInput();
    if (audioStreamCount != StemContainerWriter::kAudioStreamCount ||
            stemCodec != AV_CODEC_ID_AAC ||
            stemSampleRate != StemContainerWriter::kSampleRate) {
        return fail(pErrorMessage,
                QStringLiteral("Encoded container is not a five-stream 44.1 kHz AAC STEM file"));
    }
    if (!StemInfoImporter::hasStemAtom(filePath) ||
            StemInfoImporter::importStemInfos(filePath).size() !=
                    StemContainerWriter::kStemCount) {
        return fail(pErrorMessage,
                QStringLiteral("Mixxx could not read the encoded STEM manifest"));
    }
    return true;
}

} // namespace

class StemContainerWriter::Impl {
  public:
    struct Stream {
        AVStream* pStream = nullptr;
        AVCodecContext* pCodec = nullptr;
        SwrContext* pResampler = nullptr;
        AVFrame* pFrame = nullptr;
        std::vector<float> pending;
        qint64 nextPts = 0;
    };

    explicit Impl(QString outputFilePath, Settings settings)
            : outputFilePath(std::move(outputFilePath)),
              partialFilePath(this->outputFilePath + QStringLiteral(".partial")),
              settings(settings) {
    }

    ~Impl() {
        cancel();
    }

    bool open(QString* pErrorMessage) {
        if (opened || committed) {
            return fail(pErrorMessage,
                    QStringLiteral("STEM container writer is already active"));
        }
        if (settings.bitRatePerStream <= 0 ||
                settings.maximumOutputBytes <= 0 ||
                !outputFilePath.endsWith(
                        QStringLiteral(".stem.mp4"),
                        Qt::CaseInsensitive)) {
            return fail(pErrorMessage,
                    QStringLiteral("Invalid STEM container output settings"));
        }
        const QFileInfo outputInfo(outputFilePath);
        if (!QFileInfo(outputInfo.absolutePath()).isWritable() ||
                QFileInfo::exists(outputFilePath)) {
            return fail(pErrorMessage,
                    QStringLiteral("STEM output path is not available"));
        }
        if (QFileInfo::exists(partialFilePath) &&
                !QFile::remove(partialFilePath)) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to remove stale partial STEM output"));
        }

        const auto encodedPath = QFile::encodeName(partialFilePath);
        auto result = avformat_alloc_output_context2(
                &pFormat, nullptr, "mp4", encodedPath.constData());
        if (result < 0 || !pFormat) {
            cleanup();
            return fail(pErrorMessage,
                    QStringLiteral("Failed to create MP4 muxer: %1")
                            .arg(ffmpegError(result)));
        }
        av_dict_set(&pFormat->metadata, "title", "Mixxx separated stems", 0);

        const AVCodec* pEncoder =
                avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!pEncoder) {
            cleanup();
            return fail(pErrorMessage,
                    QStringLiteral("FFmpeg AAC encoder is unavailable"));
        }
        for (auto& stream : streams) {
            if (!openStream(&stream, pEncoder, pErrorMessage)) {
                cleanup();
                return false;
            }
        }
        if (!(pFormat->oformat->flags & AVFMT_NOFILE)) {
            result = avio_open(
                    &pFormat->pb, encodedPath.constData(), AVIO_FLAG_WRITE);
            if (result < 0) {
                cleanup();
                return fail(pErrorMessage,
                        QStringLiteral("Failed to open partial STEM output: %1")
                                .arg(ffmpegError(result)));
            }
        }
        result = avformat_write_header(pFormat, nullptr);
        if (result < 0) {
            closeOutput();
            cleanup();
            return fail(pErrorMessage,
                    QStringLiteral("Failed to write MP4 header: %1")
                            .arg(ffmpegError(result)));
        }
        opened = true;
        return true;
    }

    bool write(std::size_t frameOffset,
            std::size_t frameCount,
            std::span<const float> planarStems,
            QString* pErrorMessage) {
        constexpr auto planeCount = kStemCount * kChannelCount;
        if (!opened || frameOffset != static_cast<std::size_t>(framesWritten) ||
                planarStems.size() != frameCount * planeCount) {
            return fail(pErrorMessage,
                    QStringLiteral("Non-contiguous or malformed STEM audio chunk"));
        }

        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            std::array<float, kChannelCount> premix{};
            for (int stem = 0; stem < kStemCount; ++stem) {
                auto& pending = streams[stem + 1].pending;
                for (int channel = 0; channel < kChannelCount; ++channel) {
                    const auto sample =
                            planarStems[(stem * kChannelCount + channel) *
                                            frameCount +
                                    frame];
                    pending.push_back(sample);
                    premix[channel] += sample;
                }
            }
            streams[0].pending.insert(
                    streams[0].pending.end(), premix.cbegin(), premix.cend());

            if (streams[0].pending.size() ==
                    static_cast<std::size_t>(
                            streams[0].pCodec->frame_size * kChannelCount)) {
                for (auto& stream : streams) {
                    if (!encodePending(
                                &stream, stream.pCodec->frame_size, pErrorMessage)) {
                        return false;
                    }
                }
            }
        }
        framesWritten += static_cast<qint64>(frameCount);
        if (pFormat->pb && avio_tell(pFormat->pb) > settings.maximumOutputBytes) {
            return fail(pErrorMessage,
                    QStringLiteral("Partial STEM output exceeds the per-track size guard"));
        }
        return true;
    }

    bool finish(QString* pErrorMessage) {
        if (!opened) {
            return fail(pErrorMessage,
                    QStringLiteral("STEM container writer is not open"));
        }
        const auto remainingFrames =
                streams[0].pending.size() / kChannelCount;
        if (remainingFrames > 0) {
            for (auto& stream : streams) {
                if (!encodePending(
                            &stream,
                            static_cast<int>(remainingFrames),
                            pErrorMessage)) {
                    cancel();
                    return false;
                }
            }
        }
        for (auto& stream : streams) {
            if (!sendFrame(&stream, nullptr, pErrorMessage)) {
                cancel();
                return false;
            }
        }
        auto result = av_write_trailer(pFormat);
        if (result < 0) {
            cancel();
            return fail(pErrorMessage,
                    QStringLiteral("Failed to finalize STEM MP4: %1")
                            .arg(ffmpegError(result)));
        }
        closeOutput();
        cleanup();
        opened = false;

        if (!injectManifest(
                    partialFilePath,
                    StemContainerWriter::manifest(),
                    pErrorMessage) ||
                !validateContainer(partialFilePath,
                        settings.maximumOutputBytes,
                        pErrorMessage)) {
            QFile::remove(partialFilePath);
            return false;
        }
        if (!QFile::rename(partialFilePath, outputFilePath)) {
            QFile::remove(partialFilePath);
            return fail(pErrorMessage,
                    QStringLiteral("Failed to atomically publish STEM container"));
        }
        committed = true;
        return true;
    }

    void cancel() {
        if (opened) {
            closeOutput();
        }
        cleanup();
        opened = false;
        if (!committed && QFileInfo::exists(partialFilePath) &&
                !QFile::remove(partialFilePath)) {
            kLogger.warning()
                    << "Failed to remove partial STEM output"
                    << partialFilePath;
        }
    }

    bool openStream(Stream* pStream,
            const AVCodec* pEncoder,
            QString* pErrorMessage) {
        pStream->pStream = avformat_new_stream(pFormat, nullptr);
        pStream->pCodec = avcodec_alloc_context3(pEncoder);
        if (!pStream->pStream || !pStream->pCodec) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to allocate AAC stream"));
        }
        pStream->pCodec->codec_id = AV_CODEC_ID_AAC;
        pStream->pCodec->codec_type = AVMEDIA_TYPE_AUDIO;
        pStream->pCodec->sample_fmt = pEncoder->sample_fmts
                ? pEncoder->sample_fmts[0]
                : AV_SAMPLE_FMT_FLTP;
        pStream->pCodec->sample_rate = kSampleRate;
        pStream->pCodec->bit_rate = settings.bitRatePerStream;
        pStream->pCodec->time_base = {1, kSampleRate};
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_default(
                &pStream->pCodec->ch_layout, kChannelCount);
#else
        pStream->pCodec->channel_layout = AV_CH_LAYOUT_STEREO;
        pStream->pCodec->channels = kChannelCount;
#endif
        if (pFormat->oformat->flags & AVFMT_GLOBALHEADER) {
            pStream->pCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        auto result =
                avcodec_open2(pStream->pCodec, pEncoder, nullptr);
        if (result < 0) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to open AAC encoder: %1")
                            .arg(ffmpegError(result)));
        }
        if (pStream->pCodec->frame_size <= 0) {
            return fail(pErrorMessage,
                    QStringLiteral("AAC encoder returned an invalid frame size"));
        }
        pStream->pStream->time_base = pStream->pCodec->time_base;
        result = avcodec_parameters_from_context(
                pStream->pStream->codecpar, pStream->pCodec);
        if (result < 0) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to copy AAC stream parameters: %1")
                            .arg(ffmpegError(result)));
        }

        pStream->pFrame = av_frame_alloc();
        if (!pStream->pFrame) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to allocate AAC frame"));
        }
        pStream->pFrame->format = pStream->pCodec->sample_fmt;
        pStream->pFrame->sample_rate = pStream->pCodec->sample_rate;
        pStream->pFrame->nb_samples = pStream->pCodec->frame_size;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        result = av_channel_layout_copy(
                &pStream->pFrame->ch_layout, &pStream->pCodec->ch_layout);
#else
        pStream->pFrame->channel_layout =
                pStream->pCodec->channel_layout;
        pStream->pFrame->channels = pStream->pCodec->channels;
        result = 0;
#endif
        if (result < 0 ||
                (result = av_frame_get_buffer(pStream->pFrame, 0)) < 0) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to allocate AAC sample buffer: %1")
                            .arg(ffmpegError(result)));
        }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        AVChannelLayout inputLayout;
        av_channel_layout_default(&inputLayout, kChannelCount);
        result = swr_alloc_set_opts2(&pStream->pResampler,
                &pStream->pCodec->ch_layout,
                pStream->pCodec->sample_fmt,
                kSampleRate,
                &inputLayout,
                AV_SAMPLE_FMT_FLT,
                kSampleRate,
                0,
                nullptr);
        av_channel_layout_uninit(&inputLayout);
#else
        pStream->pResampler = swr_alloc_set_opts(nullptr,
                pStream->pCodec->channel_layout,
                pStream->pCodec->sample_fmt,
                kSampleRate,
                AV_CH_LAYOUT_STEREO,
                AV_SAMPLE_FMT_FLT,
                kSampleRate,
                0,
                nullptr);
        result = pStream->pResampler ? 0 : AVERROR(ENOMEM);
#endif
        if (result < 0 || !pStream->pResampler ||
                (result = swr_init(pStream->pResampler)) < 0) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to initialize AAC sample converter: %1")
                            .arg(ffmpegError(result)));
        }
        pStream->pending.reserve(
                pStream->pCodec->frame_size * kChannelCount);
        return true;
    }

    bool encodePending(
            Stream* pStream, int frameCount, QString* pErrorMessage) {
        if (frameCount <= 0 ||
                pStream->pending.size() !=
                        static_cast<std::size_t>(
                                frameCount * kChannelCount)) {
            return fail(pErrorMessage,
                    QStringLiteral("AAC input buffering became inconsistent"));
        }
        auto result = av_frame_make_writable(pStream->pFrame);
        if (result < 0) {
            return fail(pErrorMessage,
                    QStringLiteral("AAC frame is not writable: %1")
                            .arg(ffmpegError(result)));
        }
        pStream->pFrame->nb_samples = frameCount;
        const uint8_t* input[] = {
                reinterpret_cast<const uint8_t*>(
                        pStream->pending.data()),
        };
        result = swr_convert(pStream->pResampler,
                pStream->pFrame->data,
                frameCount,
                input,
                frameCount);
        if (result != frameCount) {
            return fail(pErrorMessage,
                    QStringLiteral("AAC sample conversion failed: %1")
                            .arg(result < 0
                                            ? ffmpegError(result)
                                            : QStringLiteral("short output")));
        }
        pStream->pFrame->pts = pStream->nextPts;
        pStream->nextPts += frameCount;
        pStream->pending.clear();
        return sendFrame(pStream, pStream->pFrame, pErrorMessage);
    }

    bool sendFrame(
            Stream* pStream, AVFrame* pFrame, QString* pErrorMessage) {
        auto result = avcodec_send_frame(pStream->pCodec, pFrame);
        if (result < 0) {
            return fail(pErrorMessage,
                    QStringLiteral("AAC encoder rejected a frame: %1")
                            .arg(ffmpegError(result)));
        }
        AVPacket* pPacket = av_packet_alloc();
        if (!pPacket) {
            return fail(pErrorMessage,
                    QStringLiteral("Failed to allocate AAC packet"));
        }
        while ((result =
                               avcodec_receive_packet(pStream->pCodec, pPacket)) >=
                0) {
            av_packet_rescale_ts(pPacket,
                    pStream->pCodec->time_base,
                    pStream->pStream->time_base);
            pPacket->stream_index = pStream->pStream->index;
            result = av_interleaved_write_frame(pFormat, pPacket);
            av_packet_unref(pPacket);
            if (result < 0) {
                av_packet_free(&pPacket);
                return fail(pErrorMessage,
                        QStringLiteral("Failed to mux AAC packet: %1")
                                .arg(ffmpegError(result)));
            }
        }
        av_packet_free(&pPacket);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            return fail(pErrorMessage,
                    QStringLiteral("AAC encoder failed while draining packets: %1")
                            .arg(ffmpegError(result)));
        }
        return true;
    }

    void closeOutput() {
        if (pFormat && pFormat->pb &&
                !(pFormat->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&pFormat->pb);
        }
    }

    void cleanup() {
        for (auto& stream : streams) {
            swr_free(&stream.pResampler);
            av_frame_free(&stream.pFrame);
            avcodec_free_context(&stream.pCodec);
            stream.pStream = nullptr;
            stream.pending.clear();
        }
        if (pFormat) {
            avformat_free_context(pFormat);
            pFormat = nullptr;
        }
    }

    QString outputFilePath;
    QString partialFilePath;
    Settings settings;
    AVFormatContext* pFormat = nullptr;
    std::array<Stream, kAudioStreamCount> streams;
    qint64 framesWritten = 0;
    bool opened = false;
    bool committed = false;
};

StemContainerWriter::StemContainerWriter(
        QString outputFilePath, Settings settings)
        : m_pImpl(std::make_unique<Impl>(
                  std::move(outputFilePath), settings)) {
}

StemContainerWriter::~StemContainerWriter() = default;

bool StemContainerWriter::open(QString* pErrorMessage) {
    return m_pImpl->open(pErrorMessage);
}

bool StemContainerWriter::write(std::size_t frameOffset,
        std::size_t frameCount,
        std::span<const float> planarStems,
        QString* pErrorMessage) {
    return m_pImpl->write(
            frameOffset, frameCount, planarStems, pErrorMessage);
}

bool StemContainerWriter::finish(QString* pErrorMessage) {
    return m_pImpl->finish(pErrorMessage);
}

void StemContainerWriter::cancel() {
    m_pImpl->cancel();
}

QString StemContainerWriter::outputFilePath() const {
    return m_pImpl->outputFilePath;
}

QString StemContainerWriter::partialFilePath() const {
    return m_pImpl->partialFilePath;
}

qint64 StemContainerWriter::writtenFrameCount() const noexcept {
    return m_pImpl->framesWritten;
}

QByteArray StemContainerWriter::manifest() {
    QJsonArray stems;
    const std::array<std::pair<QString, QString>, kStemCount> details = {{
            {QStringLiteral("Drums"), QStringLiteral("#009e73")},
            {QStringLiteral("Bass"), QStringLiteral("#d55e00")},
            {QStringLiteral("Other"), QStringLiteral("#cc79a7")},
            {QStringLiteral("Vocals"), QStringLiteral("#56b4e9")},
    }};
    for (const auto& [name, color] : details) {
        stems.append(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("color"), color},
        });
    }
    const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("stems"), stems},
            {QStringLiteral("mastering_dsp"),
                    QJsonObject{
                            {QStringLiteral("compressor"),
                                    QJsonObject{
                                            {QStringLiteral("enabled"), false},
                                    }},
                            {QStringLiteral("limiter"),
                                    QJsonObject{
                                            {QStringLiteral("enabled"), false},
                                    }},
                    }},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

qint64 StemContainerWriter::estimatedEncodedBytes(
        qint64 frameCount, int bitRatePerStream) {
    if (frameCount <= 0 || bitRatePerStream <= 0) {
        return 0;
    }
    const auto bits = static_cast<long double>(frameCount) *
            static_cast<long double>(bitRatePerStream) *
            static_cast<long double>(kAudioStreamCount) /
            static_cast<long double>(kSampleRate);
    return static_cast<qint64>(bits / 8.0L);
}

} // namespace mixxx::stems
