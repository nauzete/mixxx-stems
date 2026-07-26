#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QSet>
#include <QString>

#include "stems/stemcache.h"
#include "stems/stemseparationmanager.h"

namespace mixxx::stems {

class DemucsOnnxRunner;

/// Executes the complete decoder -> HTDemucs -> STEM MP4 -> cache path.
///
/// One instance is owned by StemSeparationManager's single low-priority
/// worker. It is not thread-safe and never runs on the audio thread.
class DemucsStemSeparationProcessor final
        : public StemSeparationProcessor {
  public:
    using ProtectedEntryIdsProvider =
            std::function<QSet<QString>()>;

    struct Settings {
        QString modelFilePath;
        QByteArray expectedModelSha256;
        QString cacheDirectoryPath;
        StemCache::Limits cacheLimits;
        int inferenceThreadCount = 2;
        int bitRatePerStream = 128000;
    };

    explicit DemucsStemSeparationProcessor(
            Settings settings,
            ProtectedEntryIdsProvider protectedEntryIdsProvider = {});
    ~DemucsStemSeparationProcessor() override;

    Result process(const StemSeparationRequest& request,
            const Callbacks& callbacks) override;

  private:
    Result cancelledOrPaused(const Callbacks& callbacks) const;
    bool prepareRuntime(
            const Callbacks& callbacks, QString* pErrorMessage);
    QSet<QString> protectedEntryIds(
            const QString& activeEntryId) const;

    Settings m_settings;
    ProtectedEntryIdsProvider m_protectedEntryIdsProvider;
    StemCache m_cache;
    std::unique_ptr<DemucsOnnxRunner> m_pRunner;
    bool m_cacheInitialized = false;
    bool m_modelVerified = false;
};

} // namespace mixxx::stems
