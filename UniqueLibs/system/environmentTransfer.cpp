#include "stdafx.h"
#include "environmentTransfer.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/environmentMerge.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentTransfer, "hc.environment.transfer")
namespace hc::environment {
EnvironmentImport ParseEnvironmentFile(const QByteArray& Bytes, EnvironmentFormat Format)
{
    EnvironmentImport Result;
    if (Bytes.size() > 16 * 1024 * 1024) return {{}, QStringLiteral("file-too-large")};
    QSet<QString> Seen;
    const auto Append = [&](Scope Source, const QString& Name, std::optional<Value> Value) {
        const auto Id = QString::number(int(Source)) + '/' + Name.toCaseFolded();
        if (Name.isEmpty() || Name.contains('=') || Name.contains(QChar(0)) || Seen.contains(Id) || (Value && ValidateValue(*Value))) { Result.error_ = "invalid-or-duplicate-variable"; return false; }
        if (Result.values_.size() >= 4096) { Result.error_ = "too-many-variables"; return false; }
        Seen.insert(Id); Result.values_.append({Source, Name, std::move(Value)}); return true;
    };
    if (Format == EnvironmentFormat::Reg) {
        const auto Parsed = registry::RegFileParser::Parse(Bytes, registry::View::Native64);
        if (!Parsed) return {{}, Parsed.error.isEmpty() ? QStringLiteral("encoding-confirmation-required") : Parsed.error};
        for (const auto& Operation : Parsed.operations) {
            Scope Source;
            if (Operation.location.Id() == EnvironmentLocation(Scope::User).Id()) Source = Scope::User;
            else if (Operation.location.Id() == EnvironmentLocation(Scope::System).Id()) Source = Scope::System;
            else return {{}, QStringLiteral("only-environment-keys-allowed")};
            if (Operation.kind == registry::OperationKind::CreateKey) continue;
            if (Operation.kind == registry::OperationKind::DeleteKey) return {{}, QStringLiteral("key-deletion-not-allowed")};
            std::optional<Value> Value;
            if (Operation.kind == registry::OperationKind::SetValue) {
                const auto Decoded = DecodeEnvironmentValue(Operation.value, Source); if (!Decoded) return {{}, QStringLiteral("unsupported-value")}; Value = Decoded.value;
            }
            if (!Append(Source, Operation.value.name, Value)) break;
        }
    } else if (Format == EnvironmentFormat::Json) {
        QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Bytes, &Error); const auto Object = Document.object();
        if (Error.error != QJsonParseError::NoError || Object["version"].toInt() != 1 || !Object["values"].isArray()) return {{}, QStringLiteral("invalid-json-schema")};
        for (const auto& Entry : Object["values"].toArray()) {
            const auto Item = Entry.toObject(); if (Item["redacted"].toBool()) continue;
            const auto ScopeName = Item["scope"].toString();
            if (ScopeName != "user" && ScopeName != "system") return {{}, QStringLiteral("invalid-scope")};
            const auto Source = ScopeName == "system" ? Scope::System : Scope::User;
            const auto Name = Item["name"].toString(); std::optional<Value> Value;
            if (!Item["delete"].toBool()) {
                if (!Item["value"].isString() || (Item["type"].toString() != "REG_SZ" && Item["type"].toString() != "REG_EXPAND_SZ")) return {{}, QStringLiteral("invalid-value")};
                Value = hc::environment::Value{Name.toStdWString(), Item["value"].toString().toStdWString(), Item["type"].toString() == "REG_SZ" ? ValueType::String : ValueType::ExpandString, Source};
            }
            if (!Append(Source, Name, Value)) break;
        }
    } else {
        QStringDecoder Decoder(QStringDecoder::Utf8); QString Text = Decoder(Bytes); if (Decoder.hasError()) return {{}, QStringLiteral("invalid-utf8")};
        if (Text.startsWith(QChar(0xfeff))) Text.remove(0, 1);
        for (auto Line : Text.split('\n')) {
            Line = Line.trimmed(); if (Line.isEmpty() || Line.startsWith('#')) continue;
            const auto Equal = Line.indexOf('='); if (Equal < 1) return {{}, QStringLiteral("expected-name-equals-value")};
            const auto Name = Line.left(Equal).trimmed(); auto Raw = Line.mid(Equal + 1).trimmed();
            if (Raw.startsWith('"') || Raw.startsWith('\'')) {
                const auto Quote = Raw.front(); if (Raw.size() < 2 || Raw.back() != Quote) return {{}, QStringLiteral("unterminated-quote")};
                Raw = Raw.mid(1, Raw.size() - 2); QString Unescaped;
                for (qsizetype I = 0; I < Raw.size(); ++I) { if (Raw[I] == '\\' && I + 1 < Raw.size() && (Raw[I + 1] == Quote || Raw[I + 1] == '\\')) ++I; Unescaped += Raw[I]; }
                Raw = Unescaped;
            }
            // Values are literal; never evaluate substitutions, commands or scripts.
            if (!Append(Scope::User, Name, Value{Name.toStdWString(), Raw.toStdWString(), ValueType::String, Scope::User})) break;
        }
    }
    if (!Result.error_.isEmpty()) Result.values_.clear();
    return Result;
}
registry::RegistryResult<QByteArray> ExportEnvironmentFile(const std::vector<Value>& Values, EnvironmentFormat Format)
{
    QJsonArray Json; QList<registry::RegistryOperation> Reg; QStringList Lines;
    std::set<std::wstring, NameLess> Sensitive;
    for (const auto& Value : Values) if (SensitiveName(Value.name_)) Sensitive.insert(Value.name_);
    for (size_t Pass = 0; Pass <= Values.size(); ++Pass) {
        const auto Before = Sensitive.size();
        for (const auto& Value : Values) for (const auto& Reference : References(Value.raw_))
            if (SensitiveName(Reference) || Sensitive.contains(Reference)) Sensitive.insert(Value.name_);
        if (Sensitive.size() == Before) break;
    }
    for (const auto& Value : Values) {
        if (Value.scope_ != Scope::User && Value.scope_ != Scope::System) continue;
        if (ValidateValue(Value)) return {{}, ERROR_INVALID_DATA};
        const auto Name = QString::fromStdWString(Value.name_); const auto Text = QString::fromStdWString(Value.raw_);
        const bool Hidden = Sensitive.contains(Value.name_);

        if (Format == EnvironmentFormat::Json) {
            QJsonObject Item{{"name", Name}, {"scope", Value.scope_ == Scope::System ? "system" : "user"}};
            if (Hidden) Item["redacted"] = true;
            else { Item["type"] = Value.type_ == ValueType::String ? "REG_SZ" : "REG_EXPAND_SZ"; Item["value"] = Text; }
            Json.append(Item);
        } else if (!Hidden && Format == EnvironmentFormat::Reg) {
            const auto Location = EnvironmentLocation(Value.scope_); Reg.append({registry::OperationKind::CreateKey, Location}); Reg.append({registry::OperationKind::SetValue, Location, EncodeEnvironmentValue(Value)});
        } else if (!Hidden && Format == EnvironmentFormat::DotEnv) {
            if (Text.contains('\n') || Text.contains('\r')) return {{}, ERROR_NOT_SUPPORTED};
            auto Escaped = Text; Escaped.replace('\\', "\\\\"); Escaped.replace('"', "\\\""); Lines.append(Name + "=\"" + Escaped + '"');
        }
    }
    if (Format == EnvironmentFormat::Reg) return {registry::RegFileWriter::Write(Reg)};
    if (Format == EnvironmentFormat::Json) return {QJsonDocument(QJsonObject{{"version", 1}, {"values", Json}}).toJson()};
    return {(Lines.join("\r\n") + "\r\n").toUtf8()};
}
EnvironmentPreview PreviewEnvironmentImport(const EnvironmentImport& Import, EnvironmentStorage& Storage)
{
    if (!Import) return {{}, ERROR_INVALID_DATA};
    const auto User = Storage.ReadScope(Scope::User), System = Storage.ReadScope(Scope::System);
    QList<EnvironmentChange> Changes;
    for (const auto& Item : Import.values_) {
        const auto& Current = Item.scope_ == Scope::System ? System : User; if (!Current) return {{}, Current.error};
        std::optional<Value> Before;
        for (const auto& Value : Current.value) if (SameName(Value.name_, Item.name_.toStdWString())) { Before = Value; break; }
        Changes.append({Item.scope_, Item.name_, Before, Item.value_});
    }
    return Storage.Preview(Changes);
}
}
