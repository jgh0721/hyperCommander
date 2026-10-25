#include "stdafx.h"
#include "extensionEngine.hpp"
#include "engineRegistry.hpp"
#include "session.hpp"
#include "workerHost.hpp"
#include "nativeArchive.hpp"
#include <QCryptographicHash>

namespace hc::compare
{
    namespace
    {
        struct Extension { QString format_, engine_, schema_, operation_, validation_; };
        const std::vector<Extension>& extensions()
        { static const std::vector<Extension> entries{{"png", "image.png", "image.rgba.v1", "compare-image", "full-png-decode"},
            {"zip", "container.zip", "archive.entries.v1", "compare-archive", "all-zip-entries-verified"},
            {"docx", "office.docx", "office.paragraphs.v1", "compare-document", "docx-main-body-and-part-bytes"}}; return entries; }
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
            if (documents.size() != Inputs.size() || result["payloadSchema"] != Engine.schema_) throw Failure("ExtensionProtocol");
            for (int i = 0; i < documents.size(); ++i)
            {
                const auto document = documents[i].toObject();
                if (document["rawSha256"].toString() != QString::fromLatin1(QCryptographicHash::hash(Inputs[i].raw_, QCryptographicHash::Sha256).toHex())) throw Failure("ExtensionProtocol");
                if (Engine.schema_ == "image.rgba.v1")
                {
                    const auto width = document["width"].toInt(), height = document["height"].toInt();
                    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || qint64(width) * height > 4 * 1024 * 1024) throw Failure("ExtensionProtocol");
                    if (Raster && document["rgba"].toString().size() != ((qint64(width) * height * 4 + 2) / 3) * 4) throw Failure("ExtensionProtocol");
                }
                else if (document["entryCount"].toInt(-1) < 0 || document["entryCount"].toInt() > 10000 || result["entries"].toArray().size() > 20000) throw Failure("ExtensionProtocol");
                if (Engine.schema_ == "office.paragraphs.v1")
                {
                    const auto paragraphs = document["paragraphs"].toArray(); if (paragraphs.size() > 50000) throw Failure("ExtensionProtocol");
                    qsizetype units = 0; for (const auto& value : paragraphs) { const auto text = value.toObject()["text"].toString(); units += text.size(); if (text.size() > 256 * 1024 || units > 4 * 1024 * 1024) throw Failure("ExtensionProtocol"); }
                }
            }
            if (Engine.schema_ == "office.paragraphs.v1" && documents.size() == 2)
            {
                const auto left = documents[0].toObject()["paragraphs"].toArray().size(), right = documents[1].toObject()["paragraphs"].toArray().size(); int previousLeft = 0, previousRight = 0;
                for (const auto& value : result["changes"].toArray())
                {
                    const auto edit = value.toObject(); const int begin = edit["begin"].toInt(-1), end = edit["end"].toInt(-1), otherBegin = edit["otherBegin"].toInt(-1), otherEnd = edit["otherEnd"].toInt(-1);
                    if (begin < previousLeft || end < begin || end > left || otherBegin < previousRight || otherEnd < otherBegin || otherEnd > right || begin - previousLeft != otherBegin - previousRight) throw Failure("ExtensionProtocol");
                    previousLeft = end; previousRight = otherEnd;
                }
                if (left - previousLeft != right - previousRight) throw Failure("ExtensionProtocol");
            }
            return result;
        }
    }
    QJsonObject ProbeExtension(const QString& Format, const Snapshot& Input, const Budget& Limits)
    {
        if (Format == "7z" || Format == "rar") { const auto result = RunArchiveWorker({Input}, {}, {}, Limits); return {{"validation", "all-archive-ordinal-streams-verified"}, {"document", result["documents"].toArray()[0]}}; }
        const auto* entry = extension(Format); if (!entry) throw Failure("NoRegisteredEngine");
        const auto result = run(*entry, {Input}, false, Limits);
        auto document = result["documents"].toArray()[0].toObject();
        if (entry->schema_ == "office.paragraphs.v1") { document["paragraphCount"] = document["paragraphs"].toArray().size(); document.remove("paragraphs"); }
        return {{"validation", entry->validation_}, {"document", document}};
    }
    bool TryExtensionComparison(const Request& Input, Result& Value, const Budget& Limits, bool Save)
    {
        const auto format = InspectResourceFormat(Input.file1_, Limits);
        if (QStringList{"zip", "7z", "rar"}.contains(format)) {
            if (Input.archiveEngine_ != "zip") return TryArchiveComparison(Input, Value, Limits, Save, format);
            if (format != "zip" || Input.mode_ == "merge" || !Input.archiveMembers_[0].isEmpty() || !Input.archiveMembers_[1].isEmpty() || !Input.archivePasswordFiles_[0].isEmpty() || !Input.archivePasswordFiles_[1].isEmpty()) throw Failure("ArchiveLegacyOptionsUnsupported");
        }
        const auto* entry = extension(format); if (!entry) return false;
        const auto* descriptor = FindComparisonEngine(format); if (!descriptor) throw Failure("NoRegisteredEngine");
        if (Input.mode_ == "merge" || !Input.output_.isEmpty()) throw Failure("ExtensionMergeUnsupported");
        if (!Input.base_.isEmpty() || !Input.file3_.isEmpty()) throw Failure("ExtensionThreeUnsupported");
        if (InspectResourceFormat(Input.file2_, Limits) != format) throw Failure("MixedInputFormats");
        Value.operation_ = entry->operation_;
        for (const auto& path : {Input.file1_, Input.file2_}) Value.inputs_.append(ReadFileSnapshot(path, descriptor->maxInputBytes_, Limits));
        Value.extension_ = run(*entry, Value.inputs_, !Input.headless_, Limits);
        const bool equal = Value.inputs_[0].raw_ == Value.inputs_[1].raw_;
        if (entry->schema_ == "image.rgba.v1")
        {
            const auto pairs = Value.extension_["pairs"].toArray(); if (pairs.size() != 1) throw Failure("ExtensionProtocol");
            const auto pair = pairs[0].toObject(); const auto count = pair["differentPixels"].toInteger(-1);
            if (count < 0 || count > 8 * 1024 * 1024 || pair["rawEqual"].toBool() != equal || pair["pixelEqual"].toBool() != (count == 0)) throw Failure("ExtensionProtocol");
        }
        else if (Value.extension_["rawEqual"].toBool() != equal) throw Failure("ExtensionProtocol");
        Value.exitCode_ = equal ? 0 : 1; Value.outcome_ = equal ? "Equal" : "Different";
        return true;
    }
}
