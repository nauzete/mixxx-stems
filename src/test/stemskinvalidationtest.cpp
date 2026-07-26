#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include "test/mixxxtest.h"

namespace {

QString readFile(const QString& path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

QString stemSkinPath(const QDir& testDirectory) {
    return testDirectory
            .absoluteFilePath(
                    QStringLiteral(
                            "../../res/skins/PioneerXDJ-RR Stems"));
}

class StemSkinValidationTest : public MixxxTest {
};

TEST_F(StemSkinValidationTest, XmlAndLocalResourcesAreValid) {
    const QDir skinDirectory(stemSkinPath(getTestDir()));
    ASSERT_TRUE(skinDirectory.exists());
    QString combinedXml;
    QDirIterator files(skinDirectory.path(),
            {QStringLiteral("*.xml")},
            QDir::Files,
            QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        const auto contents = readFile(path);
        combinedXml.append(contents);
        QXmlStreamReader reader(contents);
        while (!reader.atEnd()) {
            reader.readNext();
        }
        EXPECT_FALSE(reader.hasError())
                << qPrintable(path) << ": "
                << qPrintable(reader.errorString());
    }

    const QRegularExpression localResource(
            QStringLiteral("skin:/?([^<\"')\\s]+)"));
    auto resource = localResource.globalMatch(combinedXml);
    while (resource.hasNext()) {
        const auto relativePath =
                resource.next().captured(1);
        EXPECT_TRUE(QFile::exists(
                skinDirectory.filePath(relativePath)))
                << qPrintable(relativePath);
    }
}

TEST_F(StemSkinValidationTest,
        UsesCurrentControlsAndTouchVolumeKnobs) {
    const QDir skinDirectory(stemSkinPath(getTestDir()));
    const auto skinXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("skin.xml")));
    const auto stemsXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("stems.xml")));
    const auto deckXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("stem_deck.xml")));
    const auto rowXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("stem_row.xml")));
    const QString allStemXml =
            stemsXml + deckXml + rowXml;

    EXPECT_TRUE(skinXml.contains(
            QStringLiteral("<MinimumSize>800,480</MinimumSize>")));
    EXPECT_TRUE(skinXml.contains(QStringLiteral(
            "<title>PioneerXDJ-RR Stems</title>")));
    EXPECT_TRUE(rowXml.contains(
            QStringLiteral("<KnobComposed>")));
    EXPECT_TRUE(rowXml.contains(QStringLiteral(
            "_Stem<Variable name=\"stem\"/>],mute")));
    EXPECT_TRUE(rowXml.contains(QStringLiteral(
            "_Stem<Variable name=\"stem\"/>],volume")));
    EXPECT_TRUE(rowXml.contains(QStringLiteral(
            "_Stem<Variable name=\"stem\"/>],color")));
    EXPECT_TRUE(deckXml.contains(
            QStringLiteral("],stem_count")));

    const QStringList deckControls{
            QStringLiteral("separation_trigger"),
            QStringLiteral("separation_cancel"),
            QStringLiteral("separation_percentage"),
            QStringLiteral("separation_state"),
            QStringLiteral("separation_queue_position"),
            QStringLiteral("stem_cache_status"),
            QStringLiteral("separation_error"),
    };
    for (const auto& control : deckControls) {
        EXPECT_TRUE(allStemXml.contains(control))
                << qPrintable(control);
    }
    const QStringList globalControls{
            QStringLiteral("model_available"),
            QStringLiteral("model_download_progress"),
            QStringLiteral("queue_size"),
            QStringLiteral("active_jobs"),
            QStringLiteral("worker_state"),
    };
    for (const auto& control : globalControls) {
        EXPECT_TRUE(stemsXml.contains(
                QStringLiteral("[StemSeparation],") + control))
                << qPrintable(control);
    }
    EXPECT_FALSE(allStemXml.contains(
            QStringLiteral("],stem_1_mute")));
    EXPECT_FALSE(allStemXml.contains(
            QStringLiteral("],stem_1_volume")));
}

TEST_F(StemSkinValidationTest,
        PreservesLicenseAttributionAndCompactRows) {
    const QDir skinDirectory(stemSkinPath(getTestDir()));
    const auto license =
            readFile(skinDirectory.filePath(
                    QStringLiteral("LICENSE")));
    const auto readme =
            readFile(skinDirectory.filePath(
                    QStringLiteral("README.md")));
    const auto deckXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("stem_deck.xml")));
    const auto rowXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("stem_row.xml")));

    EXPECT_TRUE(license.contains(
            QStringLiteral("GNU GENERAL PUBLIC LICENSE")));
    EXPECT_TRUE(readme.contains(
            QStringLiteral("timewasternl")));
    EXPECT_TRUE(readme.contains(
            QStringLiteral("not an official Mixxx release")));
    EXPECT_TRUE(deckXml.contains(
            QStringLiteral("<Size>110max,34max</Size>")));
    EXPECT_TRUE(rowXml.contains(
            QStringLiteral("<Size>38max,38max</Size>")));
}

} // namespace
