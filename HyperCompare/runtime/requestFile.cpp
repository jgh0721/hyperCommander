#include "stdafx.h"
#include "requestFile.hpp"
#include "decisionPlan.hpp"
#include "directoryPlan.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QUuid>

namespace hc::compare
{
    namespace
    {
        void keys(const QJsonObject& Value, const QStringList& Allowed)
        { for (auto it = Value.begin(); it != Value.end(); ++it) if (!Allowed.contains(it.key())) throw Failure(("UnknownRequestKey:" + it.key()).toStdString()); }
        QJsonObject object(const QJsonObject& Value, const QString& Key, bool Required = false)
        { if (!Value.contains(Key) && !Required) return {}; if (!Value[Key].isObject()) throw Failure(("InvalidRequestObject:" + Key).toStdString()); return Value[Key].toObject(); }
        QString string(const QJsonValue& Value)
        { if (!Value.isString() || Value.toString().isEmpty() || Value.toString().size() > 32767) throw Failure("InvalidRequestString"); return Value.toString(); }
        int number(const QJsonValue& Value, int Minimum, int Maximum)
        { const auto n = Value.toInteger(-1); if (!Value.isDouble() || n < Minimum || n > Maximum || double(n) != Value.toDouble()) throw Failure("InvalidRequestNumber"); return int(n); }
    }
    Parsed ParseRequestFile(const QString& Path, const QStringList& Overrides, bool Console, const Budget& Limits)
    {
        Parsed parsed; parsed.request_.headless_ = Console || Overrides.contains("-headless");
        try
        {
            const auto path = CheckedDirectoryPath(QFileInfo(Path).absoluteFilePath()); const auto snapshot = ReadFileSnapshot(path, 1024 * 1024, Limits);
            const auto json = ReadCompareJson(snapshot.raw_, Limits); keys(json, {"schemaVersion", "requestId", "mode", "inputs", "output", "policy", "limits", "options"});
            if (json["schemaVersion"] != QJsonValue(1)) throw Failure("UnsupportedRequestSchema");
            const QDir parent(QFileInfo(path).absolutePath());
            const auto absolute = [&](const QJsonValue& Value) { return QDir::cleanPath(parent.absoluteFilePath(string(Value))); };
            QStringList args{"HyperCompare"};
            if (json.contains("mode")) args << "-mode" << string(json["mode"]);
            if (json["mode"] == "batch") throw Failure("NestedBatchUnsupported");
            const auto inputs = object(json, "inputs"); keys(inputs, {"left", "right", "base", "center"});
            const bool three = inputs.contains("center"); if (three && inputs.contains("base")) throw Failure("AmbiguousRequestRoles");
            QMap<QString, int> roles{{"left", 0}, {"right", three ? 2 : 1}};
            if (three) roles["center"] = 1; else if (inputs.contains("base")) roles["base"] = 2;
            for (auto it = inputs.begin(); it != inputs.end(); ++it)
            {
                if (!it.value().isObject()) throw Failure("InvalidRequestLocator"); const auto locator = it.value().toObject(); keys(locator, {"path"});
                args << (it.key() == "base" ? "-base" : "-file" + QString::number(roles[it.key()] + 1)) << absolute(locator["path"]);
            }
            const auto output = object(json, "output"); keys(output, {"path", "overwrite"});
            if (!output.isEmpty()) { args << "-output" << absolute(output["path"]); if (output.contains("overwrite")) { if (!output["overwrite"].isBool()) throw Failure("InvalidRequestBoolean"); if (output["overwrite"].toBool()) args << "-overwrite"; } }
            const auto policy = object(json, "policy"); keys(policy, {"encoding", "encodingByRole", "semantic", "include", "exclude", "directoryLevel"});
            for (const auto& pair : {std::pair{"encoding", "encoding"}, {"semantic", "semantic"}, {"directoryLevel", "directory-level"}})
                if (policy.contains(pair.first)) args << '-' + QString(pair.second) << string(policy[pair.first]);
            for (const auto& key : {"include", "exclude"}) if (policy.contains(key))
            {
                if (!policy[key].isArray() || policy[key].toArray().isEmpty()) throw Failure("InvalidRequestFilter"); QStringList values;
                for (const auto& item : policy[key].toArray()) { const auto value = string(item); if (value.contains(';')) throw Failure("InvalidRequestFilter"); values << value; }
                args << '-' + QString(key) << values.join(';');
            }
            const auto encodings = object(policy, "encodingByRole"); keys(encodings, roles.keys()); std::array<QString, 3> codecs;
            for (auto it = encodings.begin(); it != encodings.end(); ++it)
            {
                const auto codec = string(it.value()); if (!inputs.contains(it.key()) || !QStringList{"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"}.contains(codec)) throw Failure("InvalidRoleEncoding"); codecs[roles[it.key()]] = codec;
            }
            const auto limits = object(json, "limits"); keys(limits, {"timeoutMs"});
            if (limits.contains("timeoutMs") && !Overrides.contains("-timeout-ms")) args << "-timeout-ms" << QString::number(number(limits["timeoutMs"], 100, 3600000));
            const auto options = object(json, "options"); keys(options, {"takeall", "planout", "decisions", "plan", "journal", "resume", "allowdelete", "mirror", "dryrun"});
            for (auto it = options.begin(); it != options.end(); ++it)
            {
                if (QStringList{"resume", "allowdelete", "mirror", "dryrun"}.contains(it.key())) { if (!it.value().isBool()) throw Failure("InvalidRequestBoolean"); if (it.value().toBool()) args << '-' + it.key(); }
                else args << '-' + it.key() << (it.key() == "takeall" ? string(it.value()) : absolute(it.value()));
            }
            args << Overrides; parsed = Parse(args, Console); if (!parsed.error_.isEmpty()) return parsed;
            auto& request = parsed.request_; request.inputEncodings_ = codecs; request.requestFile_ = path; request.protectedInputs_ << path;
            request.requestId_ = json.contains("requestId") ? string(json["requestId"]) : QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (request.requestId_.size() > 128) throw Failure("RequestIdLimit");
            if (request.mode_ == "directory-merge")
            {
                const auto codec = InputEncoding(request, 0);
                if (InputEncoding(request, 1) != codec || (!request.base_.isEmpty() && InputEncoding(request, 2) != codec)) throw Failure("DirectoryRoleEncodingUnsupported");
                request.encoding_ = codec;
            }
            CheckProtectedInputs(request, {request.output_, request.planOut_, request.report_, request.journal_}, Limits);
        }
        catch (const std::exception& error) { parsed.error_ = QString::fromUtf8(error.what()); }
        return parsed;
    }
    void CheckProtectedInputs(const Request& Input, const QStringList& Writes, const Budget& Limits)
    {
        if (Input.protectedInputs_.isEmpty()) return;
        const auto identity = [&](const QString& Path) {
            if (!QFileInfo::exists(Path)) return QString{};
            storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.workerRead_ = true;
            const storage::Locator locator{Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()};
            const auto entry = storage::MakeNativeReadProvider(locator.kind_)->Resolve(locator, ctx).get(); if (!entry.HasValue()) throw Failure(entry.Failure().code_);
            const auto revision = entry.Value().id_.revision_; const auto token = revision ? QString::fromStdString(revision->token_) : QString{}; const auto at = token.indexOf(":identity:"); return at < 0 ? QString{} : token.mid(at);
        };
        for (const auto& protectedPath : Input.protectedInputs_)
        {
            Limits.Check(); const auto source = CheckedDirectoryPath(protectedPath), sourceId = identity(source);
            for (const auto& write : Writes)
            {
                if (write.isEmpty() || write == "-") continue; const auto target = CheckedDirectoryPath(write);
                if (DirectoryPathsOverlap(source, target) || (!sourceId.isEmpty() && identity(target) == sourceId)) throw Failure("RequestArtifactAlias");
            }
        }
    }
}
