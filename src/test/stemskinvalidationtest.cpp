#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include "test/mixxxtest.h"

namespace {

const QRegularExpression kLocalResourcePattern(
        QStringLiteral("skin:/?([^<\"')\\s]+)"));

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

    auto resource =
            kLocalResourcePattern.globalMatch(combinedXml);
    while (resource.hasNext()) {
        const auto relativePath =
                resource.next().captured(1);
        EXPECT_TRUE(QFile::exists(
                skinDirectory.filePath(relativePath)))
                << qPrintable(relativePath);
    }
}

TEST_F(StemSkinValidationTest,
        UsesIntegratedStemPadsAndCurrentControls) {
    const QDir skinDirectory(stemSkinPath(getTestDir()));
    const auto skinXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("skin.xml")));
    const auto topbarXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("topbar.xml")));
    const auto deckXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("deck.xml")));
    const auto waveformXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("waveform.xml")));
    const auto padXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("stem_pad.xml")));
    const QString allStemXml =
            topbarXml + deckXml + waveformXml + padXml;

    EXPECT_TRUE(skinXml.contains(
            QStringLiteral("<MinimumSize>800,480</MinimumSize>")));
    EXPECT_TRUE(skinXml.contains(QStringLiteral(
            "<title>PioneerXDJ-RR Stems</title>")));
    EXPECT_FALSE(skinXml.contains(
            QStringLiteral("Stems_Singleton")));
    EXPECT_FALSE(topbarXml.contains(
            QStringLiteral("[Tab],stems")));
    EXPECT_TRUE(waveformXml.contains(
            QStringLiteral("WaveformInfo_StemPads")));
    EXPECT_TRUE(waveformXml.contains(
            QStringLiteral("StemPadDrums")));
    EXPECT_TRUE(waveformXml.contains(
            QStringLiteral("StemPadVocal")));
    EXPECT_TRUE(waveformXml.contains(
            QStringLiteral("StemPadBass")));
    EXPECT_TRUE(waveformXml.contains(
            QStringLiteral("StemPadOther")));
    EXPECT_TRUE(padXml.contains(QStringLiteral(
            "_Stem<Variable name=\"stem\"/>],mute")));
    EXPECT_TRUE(padXml.contains(QStringLiteral(
            "_Stem<Variable name=\"stem\"/>],solo")));
    EXPECT_TRUE(padXml.contains(
            QStringLiteral("],stem_live_ready")));
    EXPECT_TRUE(topbarXml.contains(
            QStringLiteral(
                    "[StemSeparation],active_mode")));
    EXPECT_TRUE(topbarXml.contains(
            QStringLiteral(
                    "[StemSeparation],inference_threads")));
    EXPECT_TRUE(topbarXml.contains(
            QStringLiteral(
                    "[StemSeparation],inference_threads_down")));
    EXPECT_TRUE(topbarXml.contains(
            QStringLiteral(
                    "[StemSeparation],inference_threads_up")));
    EXPECT_TRUE(deckXml.contains(
            QStringLiteral("],eject")));
    EXPECT_FALSE(allStemXml.contains(
            QStringLiteral("],stem_1_mute")));
    EXPECT_FALSE(allStemXml.contains(
            QStringLiteral("],stem_1_volume")));
}

TEST_F(StemSkinValidationTest,
        PreservesLicenseAttributionAndCompactLayout) {
    const QDir skinDirectory(stemSkinPath(getTestDir()));
    const auto license =
            readFile(skinDirectory.filePath(
                    QStringLiteral("LICENSE")));
    const auto readme =
            readFile(skinDirectory.filePath(
                    QStringLiteral("README.md")));
    const auto deckXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("deck.xml")));
    const auto waveformXml =
            readFile(skinDirectory.filePath(
                    QStringLiteral("waveform.xml")));

    EXPECT_TRUE(license.contains(
            QStringLiteral("GNU GENERAL PUBLIC LICENSE")));
    EXPECT_TRUE(readme.contains(
            QStringLiteral("timewasternl")));
    EXPECT_TRUE(readme.contains(
            QStringLiteral("not an official Mixxx release")));
    EXPECT_TRUE(deckXml.contains(
            QStringLiteral("<Size>58max,20max</Size>")));
    EXPECT_TRUE(waveformXml.contains(
            QStringLiteral("<Size>0me,58max</Size>")));
}

} // namespace
