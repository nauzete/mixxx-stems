#include "stems/stemmodelmanager.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRunnable>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <functional>
#include <utility>

#include "moc_stemmodelmanager.cpp"

namespace mixxx::stems {
namespace {

constexpr qsizetype kHashBlockSize = 1024 * 1024;
const QString kManifestFileName =
        QStringLiteral("model-manifest.json");
const QString kModelFileName = QStringLiteral("htdemucs.onnx");

bool fail(QString* pErrorMessage, QString message) {
    if (pErrorMessage) {
        *pErrorMessage = std::move(message);
    }
    return false;
}

QByteArray hashFile(
        const QString& path, const std::function<bool()>& cancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray block(kHashBlockSize, '\0');
    while (!file.atEnd()) {
        if (cancelled && cancelled()) {
            return {};
        }
        const auto read = file.read(block.data(), block.size());
        if (read < 0) {
            return {};
        }
        if (read > 0) {
            hash.addData(QByteArrayView(block.constData(), read));
        }
    }
    return hash.result().toHex();
}

bool stringArrayEquals(
        const QJsonValue& value, const QStringList& expected) {
    const auto array = value.toArray();
    if (array.size() != expected.size()) {
        return false;
    }
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (array[index].toString() != expected[index]) {
            return false;
        }
    }
    return true;
}

bool integerArrayEquals(
        const QJsonValue& value, const QList<qint64>& expected) {
    const auto array = value.toArray();
    if (array.size() != expected.size()) {
        return false;
    }
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (array[index].toInteger(-1) != expected[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

StemModelManager::Settings StemModelManager::pinnedSettings(
        const QString& directoryPath) {
    const QUrl releaseBase(QStringLiteral(
            "https://github.com/nauzete/mixxx-stems/releases/download/"
            "htdemucs-955717e8-onnx-v1/"));
    return {
            directoryPath,
            releaseBase.resolved(QUrl(kManifestFileName)),
            releaseBase.resolved(QUrl(kModelFileName)),
            QByteArrayLiteral(
                    "04fa5cf8a4fd3674a1b7d8d6d021c1cf"
                    "108d4dfaac138e8786270115c1e9e41d"),
            QByteArrayLiteral(
                    "db37d1314ac1e1051e7978d25ef45b3f"
                    "1d3f43c837678752f592c0f2deca752d"),
            304413278,
    };
}

StemModelManager::StemModelManager(Settings settings,
        QNetworkAccessManager* pNetworkAccessManager,
        QObject* pParent)
        : QObject(pParent),
          m_settings(std::move(settings)),
          m_pNetworkAccessManager(pNetworkAccessManager) {
    if (!m_pNetworkAccessManager) {
        m_pOwnedNetworkAccessManager =
                std::make_unique<QNetworkAccessManager>();
        m_pNetworkAccessManager =
                m_pOwnedNetworkAccessManager.get();
    }
    m_workerPool.setMaxThreadCount(1);
    m_workerPool.setThreadPriority(QThread::LowPriority);
}

StemModelManager::~StemModelManager() {
    m_shuttingDown = true;
    cancel();
    m_workerPool.clear();
    m_workerPool.waitForDone();
}

void StemModelManager::initialize() {
    if (m_initialized) {
        return;
    }
    m_initialized = true;
    if (m_settings.directoryPath.isEmpty() ||
            m_settings.expectedManifestSha256.size() != 64 ||
            m_settings.expectedModelSha256.size() != 64 ||
            m_settings.expectedModelSizeBytes <= 0 ||
            !QDir().mkpath(m_settings.directoryPath)) {
        setError(QStringLiteral(
                "Invalid HTDemucs model configuration"));
        setState(State::Error);
        return;
    }
    setState(State::Checking);
    verifyExistingFiles();
}

void StemModelManager::requestAvailable() {
    m_downloadRequested = true;
    if (!m_initialized) {
        initialize();
        return;
    }
    if (m_state == State::Ready ||
            m_state == State::Downloading ||
            m_state == State::Checking) {
        return;
    }
    startManifestDownload();
}

void StemModelManager::cancel() {
    m_downloadRequested = false;
    if (m_pReply) {
        m_pReply->abort();
    }
}

StemModelManager::State StemModelManager::state() const noexcept {
    return m_state;
}

bool StemModelManager::isAvailable() const noexcept {
    return m_state == State::Ready;
}

double StemModelManager::downloadPercentage() const noexcept {
    return m_downloadPercentage;
}

QString StemModelManager::error() const {
    return m_error;
}

QString StemModelManager::modelFilePath() const {
    return QDir(m_settings.directoryPath).filePath(kModelFileName);
}

QByteArray StemModelManager::modelSha256() const {
    return m_settings.expectedModelSha256;
}

bool StemModelManager::validateManifest(
        const QJsonObject& root,
        const QByteArray& expectedModelSha256,
        qint64 expectedModelSizeBytes,
        QString* pErrorMessage) {
    const auto model = root.value(QStringLiteral("model")).toObject();
    const auto onnx = root.value(QStringLiteral("onnx")).toObject();
    const auto input = onnx.value(QStringLiteral("input")).toObject();
    const auto output = onnx.value(QStringLiteral("output")).toObject();
    const auto inputShape =
            input.value(QStringLiteral("shape")).toArray();
    const auto segmentSampleCount = inputShape.size() == 3
            ? inputShape[2].toInteger(-1)
            : -1;
    constexpr qint64 kMinimumSegmentSampleCount = 44100;
    constexpr qint64 kMaximumSegmentSampleCount = 343980;
    if (root.value(QStringLiteral("schema_version")).toInt(-1) != 1 ||
            root.value(QStringLiteral("format_version")).toInt(-1) != 1 ||
            model.value(QStringLiteral("name")).toString() !=
                    QStringLiteral("htdemucs") ||
            model.value(QStringLiteral("version")).toString() !=
                    QStringLiteral("955717e8") ||
            model.value(QStringLiteral("sample_rate")).toInt() != 44100 ||
            !stringArrayEquals(
                    model.value(QStringLiteral("logical_stem_order")),
                    {QStringLiteral("drums"),
                            QStringLiteral("bass"),
                            QStringLiteral("other"),
                            QStringLiteral("vocals")}) ||
            onnx.value(QStringLiteral("opset")).toInt() != 17 ||
            onnx.value(QStringLiteral("sha256"))
                            .toString()
                            .toLatin1() !=
                    expectedModelSha256 ||
            onnx.value(QStringLiteral("size_bytes")).toInteger(-1) !=
                    expectedModelSizeBytes ||
            input.value(QStringLiteral("dtype")).toString() !=
                    QStringLiteral("float32") ||
            segmentSampleCount < kMinimumSegmentSampleCount ||
            segmentSampleCount > kMaximumSegmentSampleCount ||
            !integerArrayEquals(
                    input.value(QStringLiteral("shape")),
                    {1, 2, segmentSampleCount}) ||
            output.value(QStringLiteral("dtype")).toString() !=
                    QStringLiteral("float32") ||
            !integerArrayEquals(
                    output.value(QStringLiteral("runtime_shape")),
                    {1, 4, 2, segmentSampleCount})) {
        return fail(pErrorMessage,
                QStringLiteral(
                        "HTDemucs manifest is incompatible"));
    }
    return true;
}

void StemModelManager::verifyExistingFiles() {
    const QPointer<StemModelManager> weakThis(this);
    const auto manifestPath = manifestFilePath();
    const auto modelPath = modelFilePath();
    const auto settings = m_settings;
    m_workerPool.start(QRunnable::create(
            [weakThis, manifestPath, modelPath, settings] {
                QString error;
                bool valid = false;
                QFile manifestFile(manifestPath);
                if (manifestFile.open(QIODevice::ReadOnly)) {
                    const auto manifestData = manifestFile.readAll();
                    QJsonParseError parseError;
                    const auto document = QJsonDocument::fromJson(
                            manifestData, &parseError);
                    const QFileInfo modelInfo(modelPath);
                    valid =
                            parseError.error ==
                                    QJsonParseError::NoError &&
                            document.isObject() &&
                            QCryptographicHash::hash(
                                    manifestData,
                                    QCryptographicHash::Sha256)
                                            .toHex() ==
                                    settings
                                            .expectedManifestSha256 &&
                            validateManifest(document.object(),
                                    settings.expectedModelSha256,
                                    settings.expectedModelSizeBytes,
                                    &error) &&
                            modelInfo.isFile() &&
                            modelInfo.size() ==
                                    settings.expectedModelSizeBytes &&
                            hashFile(modelPath, [weakThis] {
                                return !weakThis;
                            }) == settings.expectedModelSha256;
                }
                if (!valid && error.isEmpty()) {
                    error = QStringLiteral(
                            "HTDemucs model is unavailable or invalid");
                }
                if (!weakThis) {
                    return;
                }
                QMetaObject::invokeMethod(weakThis,
                        [weakThis, valid, error] {
                            if (weakThis) {
                                weakThis->existingFilesVerified(
                                        valid, error);
                            }
                        });
            }));
}

void StemModelManager::existingFilesVerified(
        bool valid, const QString& error) {
    if (m_shuttingDown) {
        return;
    }
    if (valid) {
        setError({});
        setProgress(100.0);
        setState(State::Ready);
        return;
    }
    setError(error);
    setProgress(0.0);
    setState(State::Unavailable);
    if (m_downloadRequested) {
        startManifestDownload();
    }
}

void StemModelManager::startManifestDownload() {
    m_downloadedManifest.clear();
    startDownload(DownloadPart::Manifest, m_settings.manifestUrl);
}

void StemModelManager::startModelDownload() {
    startDownload(DownloadPart::Model, m_settings.modelUrl);
}

void StemModelManager::startDownload(
        DownloadPart part, const QUrl& url) {
    if (m_pReply || !url.isValid()) {
        return;
    }
    const auto targetPath = part == DownloadPart::Manifest
            ? manifestFilePath()
            : modelFilePath();
    m_pDownloadFile = std::make_unique<QSaveFile>(targetPath);
    if (!m_pDownloadFile->open(QIODevice::WriteOnly)) {
        failDownload(QStringLiteral(
                "Failed to create HTDemucs download file"));
        return;
    }
    m_pDownloadHash = std::make_unique<QCryptographicHash>(
            QCryptographicHash::Sha256);
    m_downloadPart = part;
    setError({});
    setProgress(0.0);
    setState(State::Downloading);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
    m_pReply = m_pNetworkAccessManager->get(request);
    connect(m_pReply,
            &QNetworkReply::readyRead,
            this,
            &StemModelManager::readDownloadData);
    connect(m_pReply,
            &QNetworkReply::downloadProgress,
            this,
            [this](qint64 received, qint64 total) {
                if (total > 0) {
                    setProgress(std::clamp(
                            100.0 * static_cast<double>(received) /
                                    static_cast<double>(total),
                            0.0,
                            100.0));
                }
            });
    connect(m_pReply,
            &QNetworkReply::finished,
            this,
            &StemModelManager::downloadFinished);
}

void StemModelManager::readDownloadData() {
    if (!m_pReply || !m_pDownloadFile || !m_pDownloadHash) {
        return;
    }
    const auto data = m_pReply->readAll();
    if (data.isEmpty()) {
        return;
    }
    if (m_downloadPart == DownloadPart::Manifest) {
        m_downloadedManifest.append(data);
        if (m_downloadedManifest.size() > 1024 * 1024) {
            failDownload(QStringLiteral(
                    "HTDemucs manifest exceeds size limit"));
            return;
        }
    }
    m_pDownloadHash->addData(data);
    if (m_pDownloadFile->write(data) != data.size()) {
        failDownload(QStringLiteral(
                "Failed to write HTDemucs download"));
    }
}

void StemModelManager::downloadFinished() {
    if (!m_pReply) {
        return;
    }
    readDownloadData();
    if (!m_pReply) {
        return;
    }
    auto* pReply = std::exchange(m_pReply, nullptr);
    const auto networkError = pReply->error();
    const auto errorString = pReply->errorString();
    pReply->deleteLater();
    if (networkError != QNetworkReply::NoError) {
        if (networkError == QNetworkReply::OperationCanceledError) {
            failDownload(QStringLiteral(
                    "HTDemucs download was cancelled"));
        } else {
            failDownload(QStringLiteral(
                    "HTDemucs download failed: %1")
                            .arg(errorString));
        }
        return;
    }

    QString error;
    const auto expectedHash =
            m_downloadPart == DownloadPart::Manifest
            ? m_settings.expectedManifestSha256
            : m_settings.expectedModelSha256;
    if (!m_pDownloadHash ||
            m_pDownloadHash->result().toHex() != expectedHash) {
        failDownload(QStringLiteral(
                "HTDemucs download SHA-256 mismatch"));
        return;
    }
    if (m_downloadPart == DownloadPart::Manifest &&
            !validateDownloadedManifest(&error)) {
        failDownload(std::move(error));
        return;
    }
    if (m_downloadPart == DownloadPart::Model &&
            m_pDownloadFile->size() !=
                    m_settings.expectedModelSizeBytes) {
        failDownload(QStringLiteral(
                "HTDemucs download size mismatch"));
        return;
    }
    if (!m_pDownloadFile->commit()) {
        failDownload(QStringLiteral(
                "Failed to atomically publish HTDemucs download"));
        return;
    }

    m_pDownloadFile.reset();
    m_pDownloadHash.reset();
    if (m_downloadPart == DownloadPart::Manifest) {
        m_downloadPart = DownloadPart::None;
        startModelDownload();
    } else {
        m_downloadPart = DownloadPart::None;
        setProgress(100.0);
        setState(State::Ready);
    }
}

bool StemModelManager::validateDownloadedManifest(
        QString* pErrorMessage) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
            m_downloadedManifest, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !document.isObject()) {
        return fail(pErrorMessage,
                QStringLiteral("Invalid HTDemucs manifest JSON"));
    }
    return validateManifest(document.object(),
            m_settings.expectedModelSha256,
            m_settings.expectedModelSizeBytes,
            pErrorMessage);
}

void StemModelManager::setState(State state) {
    if (m_state == state) {
        return;
    }
    const auto wasAvailable = isAvailable();
    m_state = state;
    emit stateChanged(state);
    if (wasAvailable != isAvailable()) {
        emit availabilityChanged(isAvailable());
    }
}

void StemModelManager::setProgress(double percentage) {
    percentage = std::clamp(percentage, 0.0, 100.0);
    if (m_downloadPercentage == percentage) {
        return;
    }
    m_downloadPercentage = percentage;
    emit downloadProgressChanged(percentage);
}

void StemModelManager::setError(QString error) {
    if (m_error == error) {
        return;
    }
    m_error = std::move(error);
    emit errorChanged(m_error);
}

void StemModelManager::failDownload(QString error) {
    if (m_pReply) {
        auto* pReply = std::exchange(m_pReply, nullptr);
        pReply->abort();
        pReply->deleteLater();
    }
    if (m_pDownloadFile) {
        m_pDownloadFile->cancelWriting();
    }
    m_pDownloadFile.reset();
    m_pDownloadHash.reset();
    m_downloadedManifest.clear();
    m_downloadPart = DownloadPart::None;
    setError(std::move(error));
    setProgress(0.0);
    setState(State::Error);
}

QString StemModelManager::manifestFilePath() const {
    return QDir(m_settings.directoryPath)
            .filePath(kManifestFileName);
}

} // namespace mixxx::stems
