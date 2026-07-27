#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>

#include "stems/stemalternatesourcelinker.h"
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
    void setInferenceThreadCount(int threadCount) override;
    void discardCached(const QString& entryId) override;

  private:
    Result cancelledOrPaused(const Callbacks& callbacks) const;
    Result processLive(const StemSeparationRequest& request,
            const Callbacks& callbacks);
    bool prepareRuntime(
            const Callbacks& callbacks, QString* pErrorMessage);
    QSet<QString> protectedEntryIds(
            const QString& activeEntryId) const;

    Settings m_settings;
    ProtectedEntryIdsProvider m_protectedEntryIdsProvider;
    StemCache m_cache;
    StemAlternateSourceLinker m_alternateSourceLinker;
    std::unique_ptr<DemucsOnnxRunner> m_pRunner;
    bool m_cacheInitialized = false;
    bool m_modelVerified = false;
};

} // namespace mixxx::stems
