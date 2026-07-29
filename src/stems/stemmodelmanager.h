#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <memory>

class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

namespace mixxx::stems {

/// Locates, downloads, and verifies the pinned HTDemucs ONNX model.
///
/// Existing-file hashing runs on a low-priority worker. Downloads are streamed
/// directly into an atomic QSaveFile while calculating SHA-256, so the full
/// 300 MiB model is never retained in memory.
class StemModelManager final : public QObject {
    Q_OBJECT

  public:
    enum class State {
        Checking,
        Unavailable,
        Downloading,
        Ready,
        Error,
    };
    Q_ENUM(State)

    struct Settings {
        QString directoryPath;
        QUrl manifestUrl;
        QUrl modelUrl;
        QByteArray expectedManifestSha256;
        QByteArray expectedModelSha256;
        qint64 expectedModelSizeBytes = 0;
    };

    static Settings pinnedSettings(const QString& directoryPath);

    explicit StemModelManager(Settings settings,
            QNetworkAccessManager* pNetworkAccessManager = nullptr,
            QObject* pParent = nullptr);
    ~StemModelManager() override;

    void initialize();
    void requestAvailable();
    void cancel();

    State state() const noexcept;
    bool isAvailable() const noexcept;
    double downloadPercentage() const noexcept;
    QString error() const;
    QString modelFilePath() const;
    QByteArray modelSha256() const;

    static bool validateManifest(const QJsonObject& root,
            const QByteArray& expectedModelSha256,
            qint64 expectedModelSizeBytes,
            QString* pErrorMessage = nullptr);

  signals:
    void stateChanged(mixxx::stems::StemModelManager::State state);
    void availabilityChanged(bool available);
    void downloadProgressChanged(double percentage);
    void errorChanged(const QString& error);

  private:
    enum class DownloadPart {
        None,
        Manifest,
        Model,
    };

    void verifyExistingFiles();
    void existingFilesVerified(
            bool valid, const QString& error);
    void startManifestDownload();
    void startModelDownload();
    void startDownload(DownloadPart part, const QUrl& url);
    void readDownloadData();
    void downloadFinished();
    bool validateDownloadedManifest(QString* pErrorMessage);
    void setState(State state);
    void setProgress(double percentage);
    void setError(QString error);
    void failDownload(QString error);
    QString manifestFilePath() const;

    Settings m_settings;
    std::unique_ptr<QNetworkAccessManager> m_pOwnedNetworkAccessManager;
    QNetworkAccessManager* m_pNetworkAccessManager = nullptr;
    QNetworkReply* m_pReply = nullptr;
    std::unique_ptr<QSaveFile> m_pDownloadFile;
    std::unique_ptr<QCryptographicHash> m_pDownloadHash;
    QByteArray m_downloadedManifest;
    QThreadPool m_workerPool;
    State m_state = State::Unavailable;
    DownloadPart m_downloadPart = DownloadPart::None;
    double m_downloadPercentage = 0.0;
    QString m_error;
    bool m_initialized = false;
    bool m_downloadRequested = false;
    bool m_shuttingDown = false;
};

} // namespace mixxx::stems
