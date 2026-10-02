#pragma once
#include "columnQueryEngine.hpp"
#include "commonLibs/scripting/luaRuntime.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <limits>

namespace hc::columns {
    // JSON is only the immutable job envelope, never the numeric comparison value.
    inline QJsonArray EncodeScriptInput(const CellValue& Value) {
        if (!Value.Value()) return {};
        const int Type = int(*Value.Type());
        if (*Value.Type() == FieldType::Boolean) return {Type, Value.Raw().toBool()};
        if (*Value.Type() == FieldType::Real) return {Type, Value.Raw().toDouble()};
        if (*Value.Type() == FieldType::Timestamp) return {Type, Value.Raw().toDateTime().toString(Qt::ISODateWithMs)};
        return {Type, Value.Raw().toString()};
    }
    inline CellValue ScriptCell(const hc::script::ScriptValue& Value, FieldType Type) {
        using namespace hc::script;
        if (std::holds_alternative<hc::storage::Unit>(Value.data_)) return CellValue::Missing(ValueState::Empty);
        const auto* Text = std::get_if<std::string>(&Value.data_);
        const auto* Integer = std::get_if<std::int64_t>(&Value.data_);
        const auto String = Text ? QString::fromStdString(*Text) : QString{};
        switch (Type) {
        case FieldType::Text: if (Text) return CellValue::Ready(String); break;
        case FieldType::Integer: if (Integer) return CellValue::Ready(qint64(*Integer)); break;
        case FieldType::UnsignedInteger:
            if (Integer && *Integer >= 0) return CellValue::Ready(quint64(*Integer));
            if (Text) { bool Ok = false; const auto Number = String.toULongLong(&Ok); if (Ok && QString::number(Number) == String) return CellValue::Ready(Number); } break;
        case FieldType::Real:
            if (Integer) return CellValue::Ready(double(*Integer));
            if (const auto* Number = std::get_if<double>(&Value.data_)) return CellValue::Ready(*Number); break;
        case FieldType::Boolean: if (const auto* Boolean = std::get_if<bool>(&Value.data_)) return CellValue::Ready(*Boolean); break;
        case FieldType::Date: { const auto Date = QDate::fromString(String, Qt::ISODate); if (Date.isValid()) return CellValue::Ready(Date); break; }
        case FieldType::Time: { const auto Time = QTime::fromString(String, Qt::ISODateWithMs); if (Time.isValid()) return CellValue::Ready(Time); break; }
        case FieldType::Timestamp: { const auto Date = QDateTime::fromString(String, Qt::ISODateWithMs); if (Date.isValid()) return CellValue::Ready(Date); break; }
        }
        return CellValue::Missing(ValueState::Error, "script-output-type");
    }
    inline QueryEngine::Loader WithLuaColumns(QueryEngine::Loader Base, ColumnProfile Profile, QString Retry = {}) {
        return [Base = std::move(Base), Profile = std::move(Profile), Retry](const QString& Path, const QString& Family, std::stop_token Stop) -> FieldValues {
            if (!Family.startsWith("lua.")) return Base(Path, Family, Stop);
            const auto Fail = [](const QString& Code) { return FieldValues{{"__error", CellValue::Missing(ValueState::Error, Code)}}; };
            const auto It = std::find_if(Profile.scripts_.begin(), Profile.scripts_.end(), [&](const auto& Script) { return ScriptPrefix(Script) == Family + '.'; });
            if (It == Profile.scripts_.end()) return Fail("script-unavailable");
            if (!It->enabled_) return {{"__error", CellValue::Missing(ValueState::Unsupported, "script-disabled")}};
            if (Stop.stop_requested()) return {{"__error", CellValue::Missing(ValueState::OnDemand)}};
            struct Compiled { std::unique_ptr<hc::script::IColumnRuntime> runtime_; int failures_ = 0; QString error_; };
            // Each pool worker owns its Lua states; no QObject or plugin pointer enters Lua.
            thread_local std::map<QString, Compiled> Cache;
            const auto Key = Family + Retry + QString::fromLatin1(QCryptographicHash::hash(It->source_.toUtf8(), QCryptographicHash::Sha256).toHex());
            if (!Cache.contains(Key) && Cache.size() >= 4) Cache.erase(Cache.begin());
            auto& Entry = Cache[Key];
            if (Entry.failures_ >= 3) return Fail("script-disabled-after-errors: " + Entry.error_);
            if (!Entry.runtime_) {
                Entry.runtime_ = hc::script::MakeLuaColumnRuntime();
                hc::script::RuntimeLimits Limits; Limits.instructions_ = 100000; Limits.wallTime_ = std::chrono::milliseconds(75);
                auto Prepared = Entry.runtime_->Prepare(It->source_.toStdString(), Limits);
                if (!Prepared.HasValue()) { Entry.error_ = QString::fromStdString(Prepared.Failure().code_); ++Entry.failures_; Entry.runtime_.reset(); return Fail(Entry.error_); }
            }
            hc::script::ScriptValue::Object Input;
            const auto Inputs = QJsonDocument::fromJson(Path.toUtf8()).object()["values"].toObject();
            for (auto InputIt = Inputs.begin(); InputIt != Inputs.end(); ++InputIt) {
                const auto Pair = InputIt.value().toArray(); if (Pair.size() != 2) continue;
                hc::script::ScriptValue Value;
                switch (static_cast<FieldType>(Pair[0].toInt())) {
                case FieldType::Integer: Value = std::int64_t(Pair[1].toString().toLongLong()); break;
                case FieldType::UnsignedInteger: {
                    const auto Number = Pair[1].toString().toULongLong();
                    Value = Number <= INT64_MAX ? hc::script::ScriptValue(std::int64_t(Number)) : hc::script::ScriptValue(Pair[1].toString().toStdString()); break;
                }
                case FieldType::Real: Value = Pair[1].toDouble(); break;
                case FieldType::Boolean: Value = Pair[1].toBool(); break;
                default: Value = Pair[1].toString().toStdString(); break;
                }
                Input.emplace(InputIt.key().toStdString(), std::move(Value));
            }
            const auto Result = [&] {
                std::stop_callback Cancel(Stop, [Runtime = Entry.runtime_.get()] { Runtime->RequestInterrupt(); });
                return Entry.runtime_->Invoke(Input);
            }();
            if (Stop.stop_requested()) { Entry.runtime_.reset(); return {{"__error", CellValue::Missing(ValueState::OnDemand)}}; }
            if (!Result.HasValue()) { Entry.error_ = QString::fromStdString(Result.Failure().code_); ++Entry.failures_; return Fail(Entry.error_); }
            const auto* Outputs = std::get_if<hc::script::ScriptValue::Object>(&Result.Value().data_);
            if (!Outputs) { ++Entry.failures_; Entry.error_ = "script-result-object"; return Fail(Entry.error_); }
            FieldValues Values;
            for (const auto& Output : It->outputs_) {
                const auto Found = Outputs->find(Output.name_.toStdString());
                Values[ScriptPrefix(*It) + Output.name_] = Found == Outputs->end() ? CellValue::Missing(ValueState::Empty) : ScriptCell(Found->second, Output.type_);
            }
            Entry.failures_ = 0; return Values;
        };
    }
}
