#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include "stems/stemmodelmanager.h"

namespace mixxx::stems {
namespace {

const QByteArray kModelSha(
        "db37d1314ac1e1051e7978d25ef45b3f"
        "1d3f43c837678752f592c0f2deca752d");
constexpr qint64 kModelSize = 304413278;

QJsonObject validManifest() {
    return {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("format_version"), 1},
            {QStringLiteral("model"),
                    QJsonObject{
                            {QStringLiteral("name"),
                                    QStringLiteral("htdemucs")},
                            {QStringLiteral("version"),
                                    QStringLiteral("955717e8")},
                            {QStringLiteral("sample_rate"), 44100},
                            {QStringLiteral("logical_stem_order"),
                                    QJsonArray{
                                            QStringLiteral("drums"),
                                            QStringLiteral("bass"),
                                            QStringLiteral("other"),
                                            QStringLiteral("vocals"),
                                    }},
                    }},
            {QStringLiteral("onnx"),
                    QJsonObject{
                            {QStringLiteral("opset"), 17},
                            {QStringLiteral("sha256"),
                                    QString::fromLatin1(kModelSha)},
                            {QStringLiteral("size_bytes"), kModelSize},
                            {QStringLiteral("input"),
                                    QJsonObject{
                                            {QStringLiteral("dtype"),
                                                    QStringLiteral(
                                                            "float32")},
                                            {QStringLiteral("shape"),
                                                    QJsonArray{
                                                            1,
                                                            2,
                                                            343980,
                                                    }},
                                    }},
                            {QStringLiteral("output"),
                                    QJsonObject{
                                            {QStringLiteral("dtype"),
                                                    QStringLiteral(
                                                            "float32")},
                                            {QStringLiteral("runtime_shape"),
                                                    QJsonArray{
                                                            1,
                                                            4,
                                                            2,
                                                            343980,
                                                    }},
                                    }},
                    }},
    };
}

TEST(StemModelManagerTest, AcceptsPinnedManifestContract) {
    QString error;
    EXPECT_TRUE(StemModelManager::validateManifest(
            validManifest(), kModelSha, kModelSize, &error));
    EXPECT_TRUE(error.isEmpty());
}

TEST(StemModelManagerTest, RejectsUnexpectedStemOrder) {
    auto manifest = validManifest();
    auto model = manifest.value(QStringLiteral("model")).toObject();
    model.insert(QStringLiteral("logical_stem_order"),
            QJsonArray{
                    QStringLiteral("vocals"),
                    QStringLiteral("drums"),
                    QStringLiteral("bass"),
                    QStringLiteral("other"),
            });
    manifest.insert(QStringLiteral("model"), model);

    QString error;
    EXPECT_FALSE(StemModelManager::validateManifest(
            manifest, kModelSha, kModelSize, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(StemModelManagerTest, RejectsUnexpectedTensorShape) {
    auto manifest = validManifest();
    auto onnx = manifest.value(QStringLiteral("onnx")).toObject();
    auto output = onnx.value(QStringLiteral("output")).toObject();
    output.insert(QStringLiteral("runtime_shape"),
            QJsonArray{1, 4, 2, 1});
    onnx.insert(QStringLiteral("output"), output);
    manifest.insert(QStringLiteral("onnx"), onnx);

    EXPECT_FALSE(StemModelManager::validateManifest(
            manifest, kModelSha, kModelSize));
}

TEST(StemModelManagerTest, AcceptsShortSegmentManifestContract) {
    auto manifest = validManifest();
    auto onnx = manifest.value(QStringLiteral("onnx")).toObject();
    auto input = onnx.value(QStringLiteral("input")).toObject();
    input.insert(QStringLiteral("shape"), QJsonArray{1, 2, 171990});
    onnx.insert(QStringLiteral("input"), input);
    auto output = onnx.value(QStringLiteral("output")).toObject();
    output.insert(QStringLiteral("runtime_shape"),
            QJsonArray{1, 4, 2, 171990});
    onnx.insert(QStringLiteral("output"), output);
    manifest.insert(QStringLiteral("onnx"), onnx);

    EXPECT_TRUE(StemModelManager::validateManifest(
            manifest, kModelSha, kModelSize));
}

TEST(StemModelManagerTest, RejectsTooShortSegmentManifestContract) {
    auto manifest = validManifest();
    auto onnx = manifest.value(QStringLiteral("onnx")).toObject();
    auto input = onnx.value(QStringLiteral("input")).toObject();
    input.insert(QStringLiteral("shape"), QJsonArray{1, 2, 44099});
    onnx.insert(QStringLiteral("input"), input);
    auto output = onnx.value(QStringLiteral("output")).toObject();
    output.insert(QStringLiteral("runtime_shape"),
            QJsonArray{1, 4, 2, 44099});
    onnx.insert(QStringLiteral("output"), output);
    manifest.insert(QStringLiteral("onnx"), onnx);

    EXPECT_FALSE(StemModelManager::validateManifest(
            manifest, kModelSha, kModelSize));
}

} // namespace
} // namespace mixxx::stems
