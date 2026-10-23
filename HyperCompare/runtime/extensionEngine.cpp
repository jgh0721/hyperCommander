#include "stdafx.h"
#include "extensionEngine.hpp"
#include "engineRegistry.hpp"
#include "session.hpp"
#include "workerHost.hpp"
#include <QCryptographicHash>

namespace hc::compare
{
    namespace
    {
        struct Extension { QString format_, engine_; };
        const std::vector<Extension>& extensions()
        { static const std::vector<Extension> entries{{"png", "image.png"}}; return entries; }
        const Extension* extension(const QString& Format)
        { for (const auto& entry : extensions()) if (entry.format_ == Format) return &entry; return nullptr; }
        QJsonObject run(const Extension& Engine, const QVector<Snapshot>& Inputs, bool Raster, const Budget& Limits)
        {
            QJsonArray resources; for (const auto& input : Inputs) resources.append(QString::fromLatin1(input.raw_.toBase64()));
            WorkerPolicy policy; policy.timeoutMs_ = 5000; policy.memoryBytes_ = 512 * 1024 * 1024;
            policy.requestBytes_ = policy.responseBytes_ = 48 * 1024 * 1024;
            auto result = RunWorker(Engine.engine_, {{"resources", resources}, {"raster", Raster}}, Limits, policy);
            if (result["status"] != "Complete") throw Failure(result["reason"].toString("ExtensionWorkerFailed").toStdString());
            const auto documents = result["documents"].toArray();
            if (documents.size() != Inputs.size() || result["payloadSchema"] != "image.rgba.v1") throw Failure("ExtensionProtocol");
            for (int i = 0; i < documents.size(); ++i)
            {
                const auto document = documents[i].toObject(); const auto width = document["width"].toInt(), height = document["height"].toInt();
                if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || qint64(width) * height > 4 * 1024 * 1024 ||
                    document["rawSha256"].toString() != QString::fromLatin1(QCryptographicHash::hash(Inputs[i].raw_, QCryptographicHash::Sha256).toHex())) throw Failure("ExtensionProtocol");
                if (Raster && document["rgba"].toString().size() != ((qint64(width) * height * 4 + 2) / 3) * 4) throw Failure("ExtensionProtocol");
            }
            return result;
        }
    }
    QJsonObject ProbeExtension(const QString& Format, const Snapshot& Input, const Budget& Limits)
    {
        const auto* entry = extension(Format); if (!entry) throw Failure("NoRegisteredEngine");
        const auto result = run(*entry, {Input}, false, Limits);
        return {{"validation", "full-png-decode"}, {"document", result["documents"].toArray()[0]}};
    }
    bool TryExtensionComparison(const Request& Input, Result& Value, const Budget& Limits)
    {
        const auto format = InspectResourceFormat(Input.file1_, Limits); const auto* entry = extension(format); if (!entry) return false;
        const auto* descriptor = FindComparisonEngine(format); if (!descriptor) throw Failure("NoRegisteredEngine");
        if (Input.mode_ == "merge" || !Input.output_.isEmpty()) throw Failure("ExtensionMergeUnsupported");
        if (!Input.base_.isEmpty() || !Input.file3_.isEmpty()) throw Failure("ExtensionThreeUnsupported");
        if (InspectResourceFormat(Input.file2_, Limits) != format) throw Failure("MixedInputFormats");
        Value.operation_ = "compare-image";
        for (const auto& path : {Input.file1_, Input.file2_}) Value.inputs_.append(ReadFileSnapshot(path, descriptor->maxInputBytes_, Limits));
        Value.extension_ = run(*entry, Value.inputs_, !Input.headless_, Limits);
        const auto pairs = Value.extension_["pairs"].toArray(); if (pairs.size() != 1) throw Failure("ExtensionProtocol");
        const bool equal = Value.inputs_[0].raw_ == Value.inputs_[1].raw_;
        const auto pair = pairs[0].toObject(); const auto count = pair["differentPixels"].toInteger(-1);
        if (count < 0 || count > 8 * 1024 * 1024 || pair["rawEqual"].toBool() != equal || pair["pixelEqual"].toBool() != (count == 0)) throw Failure("ExtensionProtocol");
        Value.exitCode_ = equal ? 0 : 1; Value.outcome_ = equal ? "Equal" : "Different";
        return true;
    }
}
