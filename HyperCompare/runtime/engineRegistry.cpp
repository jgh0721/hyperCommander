#include "stdafx.h"
#include "engineRegistry.hpp"
#include "session.hpp"
#include "extensionEngine.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <Windows.h>

namespace hc::compare
{
    QJsonObject EngineDescriptor::Json() const
    {
        return {{"id", id_}, {"version", version_}, {"kind", kind_}, {"formats", QJsonArray::fromStringList(formats_)},
            {"decoder", decoder_}, {"writer", writer_}, {"view", view_}, {"locationSchema", locationSchema_},
            {"compare", QJsonObject{{"two", compareTwo_}, {"three", compareThree_}, {"independentThree", independentThree_}}},
            {"merge", QJsonObject{{"two", mergeTwo_}, {"base", mergeBase_}}}, {"isolated", isolated_}, {"maxInputBytes", double(maxInputBytes_)}, {"limits", limits_}};
    }
    const std::vector<EngineDescriptor>& RegisteredEngines()
    {
        static const std::vector<EngineDescriptor> engines{
            {"text.myers", "1", "comparison", {"text"}, "bounded-text-codec", "native-verified-replace", "scintilla-text", "text.bytes.v1", true, true, true, true, true, false, 16 * 1024 * 1024},
            {"directory.native", "2", "comparison", {"directory"}, "native-directory-snapshot", "reviewed-directory-plan", "virtual-directory", "directory.entry.v1", true, true, true, true, false, false, 0},
            {"image.png", "1", "comparison", {"png"}, "qt-png-isolated", {}, "image-rgba", "image.rect.v1", true, false, false, false, false, true, 16 * 1024 * 1024,
                {{"maxPixels", 4 * 1024 * 1024}, {"maxDimension", 8192}, {"maxSampleBits", 8}, {"animation", false}}},
            {"source.tree-sitter", "0.27.1/c-0.24.2/cpp-0.23.4", "annotation", {"c", "cpp"}, "tree-sitter", {}, "function-relations", "text.bytes.v1", true, true, false, false, true, true, 16 * 1024 * 1024}
        };
        return engines;
    }
    const EngineDescriptor* FindComparisonEngine(const QString& Format)
    {
        for (const auto& engine : RegisteredEngines()) if (engine.kind_ == "comparison" && engine.formats_.contains(Format)) return &engine;
        return nullptr;
    }
    QString DetectResourceFormat(const QString& Path, const QByteArray& Prefix)
    {
        if (Prefix.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) return "png";
        if (Prefix.startsWith(QByteArray::fromHex("ffd8ff"))) return "jpeg";
        if (Prefix.startsWith("GIF87a") || Prefix.startsWith("GIF89a")) return "gif";
        if (Prefix.startsWith("RIFF") && Prefix.mid(8, 4) == "WEBP") return "webp";
        if (Prefix.startsWith("PK\x03\x04") || Prefix.startsWith("PK\x05\x06") || Prefix.startsWith("PK\x07\x08"))
        {
            const auto suffix = QFileInfo(Path).suffix().toLower();
            return QStringList{"docx", "xlsx", "pptx"}.contains(suffix) ? suffix : "zip";
        }
        if (Prefix.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1"))) return "office-compound";
        if (Prefix.startsWith("%PDF-")) return "pdf";
        if (Prefix.startsWith("\xff\xfe") || Prefix.startsWith("\xfe\xff")) return "text";
        return Prefix.contains('\0') ? "binary" : "text";
    }
    QJsonObject EngineCatalog()
    {
        QJsonArray engines; for (const auto& engine : RegisteredEngines()) engines.append(engine.Json());
        return {{"schemaVersion", 1}, {"engines", engines}, {"registration", "static"},
            {"location", "schema-version-resourceRevision-payload"}, {"nestedResources", "versioned-provider-locator"},
            {"probe", "-mode probe -file1 PATH -file2 PATH [-base PATH|-file3 PATH]"},
            {"unsupportedFormats", QJsonArray{"jpeg", "gif", "webp", "zip", "docx", "xlsx", "pptx", "office-compound", "pdf", "binary"}}};
    }
    namespace
    {
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        QJsonObject probeOne(const QString& Path, const QString& Encoding, const Budget& Limits)
        {
            Limits.Check(); const auto path = QDir::cleanPath(QFileInfo(Path).absoluteFilePath());
            const storage::Locator locator{path.startsWith("//") || path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, path.toUtf8().toStdString()};
            const auto provider = storage::MakeNativeReadProvider(locator.kind_);
            storage::OperationContext context; context.deadline_ = Limits.deadline_; context.workerRead_ = true; context.denyConcurrentMutation_ = true;
            const auto entry = require(provider->Resolve(locator, context).get());
            if (entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseInputUnsupported");
            QString format = "directory", validation = "root-only";
            if (!entry.directory_)
            {
                const auto stream = require(provider->OpenRead(entry.id_, context).get()); const auto chunk = require(stream->Read(4096, context).get());
                const QByteArray prefix(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size()));
                Limits.Check(); format = DetectResourceFormat(path, prefix); validation = "signature-only";
                if (format == "binary" && !Encoding.isEmpty()) format = "text";
                const auto after = require(provider->Stat(entry.id_, context).get()); if (entry.id_.revision_ != after.id_.revision_) throw Failure("ChangedDuringRead");
            }
            QJsonObject result{{"path", path}, {"format", format}, {"size", QString::number(entry.size_)},
                {"revision", entry.id_.revision_ ? QString::fromStdString(entry.id_.revision_->token_) : QString{}},
                {"provider", locator.kind_ == storage::LocatorKind::Smb ? "smb" : "local"}, {"access", entry.directory_ ? "directory-enumeration" : "streaming-read"},
                {"materialization", "none"}, {"validation", validation}, {"available", false}};
            const auto* engine = FindComparisonEngine(format);
            if (!engine) { result["reason"] = "NoRegisteredEngine"; return result; }
            if (format == "text")
            {
                try
                {
                    const auto text = ReadTextSnapshot(path, Encoding, Limits);
                    if (entry.id_.revision_ != text.entry_.id_.revision_) throw Failure("ChangedDuringRead");
                    result["encoding"] = text.encoding_; result["decodedBytes"] = double(text.utf8_.size()); result["validation"] = "full-text-decode";
                }
                catch (const Failure& failure)
                {
                    if (failure.code_ == "Canceled" || failure.code_ == "Timeout" || failure.code_ == "ChangedDuringRead") throw;
                    result["reason"] = QString::fromStdString(failure.code_); return result;
                }
            }
            else if (format != "directory")
            {
                try
                {
                    const auto snapshot = ReadFileSnapshot(path, engine->maxInputBytes_, Limits);
                    if (entry.id_.revision_ != snapshot.entry_.id_.revision_) throw Failure("ChangedDuringRead");
                    const auto validation = ProbeExtension(format, snapshot, Limits);
                    result["validation"] = validation["validation"]; result["document"] = validation["document"];
                }
                catch (const Failure& failure)
                {
                    if (failure.code_ == "Canceled" || failure.code_ == "ChangedDuringRead") throw;
                    result["reason"] = QString::fromStdString(failure.code_); return result;
                }
            }
            result["engine"] = engine->id_; result["available"] = true; return result;
        }
    }
    QString InspectResourceFormat(const QString& Path, const Budget& Limits)
    {
        Limits.Check(); const storage::Locator locator{Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()};
        const auto provider = storage::MakeNativeReadProvider(locator.kind_); storage::OperationContext context;
        context.deadline_ = Limits.deadline_; context.workerRead_ = true; context.denyConcurrentMutation_ = true;
        const auto entry = require(provider->Resolve(locator, context).get());
        if (entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseInputUnsupported");
        if (entry.directory_) return "directory";
        const auto stream = require(provider->OpenRead(entry.id_, context).get()); const auto chunk = require(stream->Read(4096, context).get());
        const auto after = require(provider->Stat(entry.id_, context).get()); if (entry.id_.revision_ != after.id_.revision_) throw Failure("ChangedDuringRead");
        Limits.Check(); return DetectResourceFormat(Path, QByteArray(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size())));
    }
    QJsonObject ProbeComparison(const Request& Input, const Budget& Limits)
    {
        QJsonArray inputs; QString format; bool compatible = true;
        for (const auto& path : {Input.file1_, Input.file2_, Input.base_.isEmpty() ? Input.file3_ : Input.base_})
        {
            if (path.isEmpty()) continue; auto probe = probeOne(path, Input.encoding_, Limits);
            if (format.isEmpty()) format = probe["format"].toString();
            compatible &= probe["format"] == format && probe["available"].toBool(); inputs.append(probe);
        }
        const auto* engine = FindComparisonEngine(format); QString reason;
        if (inputs.size() < 2) throw Failure("BothInputsRequired");
        if (!compatible || !engine) { compatible = false; reason = "InputCapabilityMismatch"; }
        else if (inputs.size() == 3 && !engine->compareThree_) { compatible = false; reason = "ThreeUnsupported"; }
        else if (inputs.size() == 3 && Input.base_.isEmpty() && !engine->independentThree_) { compatible = false; reason = "IndependentThreeUnsupported"; }
        return {{"schemaVersion", 1}, {"available", compatible}, {"reason", reason}, {"inputs", inputs},
            {"engine", engine ? engine->Json() : QJsonObject{}},
            {"compareAvailable", compatible}, {"mergeTwoAvailable", compatible && inputs.size() == 2 && engine && engine->mergeTwo_},
            {"mergeBaseAvailable", compatible && !Input.base_.isEmpty() && engine && engine->mergeBase_},
            {"outputValidated", false}, {"warning", "Probe is read-only. Input revisions and output policy are revalidated when executing."}};
    }
}
