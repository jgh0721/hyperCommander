#include "wire.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <charconv>
#include <cmath>

namespace hc::script::comparison {
namespace {
Result<ScriptValue> Read(const QJsonValue& Value, int Depth, int& Nodes) {
    if(Depth>16 || ++Nodes>4096) return Error{ErrorCategory::BudgetExceeded,"wire-depth-or-nodes"};
    if(Value.isNull()) return ScriptValue{};
    if(Value.isBool()) return ScriptValue(Value.toBool());
    if(Value.isString()) return ScriptValue(Value.toString().toStdString());
    if(Value.isDouble()) {
        const auto Number=Value.toDouble();
        if(std::trunc(Number)==Number) {
            if(std::abs(Number)>9007199254740991.0) return Error{ErrorCategory::InvalidRequest,"wire-integer-needs-tag"};
            return ScriptValue(static_cast<std::int64_t>(Number));
        }
        return ScriptValue(Number);
    }
    if(Value.isArray()) {
        ScriptValue::Array Items; for(const auto& Child:Value.toArray()) { auto Item=Read(Child,Depth+1,Nodes); if(!Item.HasValue()) return Item.Failure(); Items.push_back(std::move(Item.Value())); } return ScriptValue(std::move(Items));
    }
    if(Value.isObject()) {
        const auto Object=Value.toObject();
        if(Object.contains("$double")) {
            if(Object.size()!=1 || !Object["$double"].isDouble() || !std::isfinite(Object["$double"].toDouble()))
                return Error{ErrorCategory::InvalidRequest,"wire-double-tag"};
            return ScriptValue(Object["$double"].toDouble());
        }
        if(Object.contains("$int64")) {
            if(Object.size()!=1 || !Object["$int64"].isString()) return Error{ErrorCategory::InvalidRequest,"wire-int64-tag"};
            const auto Bytes=Object["$int64"].toString().toUtf8(); std::int64_t Integer=0;
            const auto Parsed=std::from_chars(Bytes.constData(),Bytes.constData()+Bytes.size(),Integer);
            if(Parsed.ec!=std::errc{} || Parsed.ptr!=Bytes.constData()+Bytes.size()) return Error{ErrorCategory::InvalidRequest,"wire-int64-tag"};
            return ScriptValue(Integer);
        }
        ScriptValue::Object Items;
        for(auto It=Object.begin();It!=Object.end();++It) { auto Item=Read(It.value(),Depth+1,Nodes); if(!Item.HasValue()) return Item.Failure(); Items.emplace(It.key().toStdString(),std::move(Item.Value())); }
        return ScriptValue(std::move(Items));
    }
    return Error{ErrorCategory::InvalidRequest,"wire-value"};
}
QJsonValue Write(const ScriptValue& Value) {
    return std::visit([](const auto& Item) -> QJsonValue {
        using T=std::decay_t<decltype(Item)>;
        if constexpr(std::is_same_v<T,Unit>) return QJsonValue::Null;
        else if constexpr(std::is_same_v<T,std::string>) return QString::fromStdString(Item);
        else if constexpr(std::is_same_v<T,std::int64_t>) return QJsonObject{{"$int64",QString::number(Item)}};
        else if constexpr(std::is_same_v<T,double>) return QJsonObject{{"$double",Item}};
        else if constexpr(std::is_same_v<T,ScriptValue::Array>) { QJsonArray Result; for(const auto& Child:Item) Result.append(Write(Child)); return Result; }
        else if constexpr(std::is_same_v<T,ScriptValue::Object>) { QJsonObject Result; for(const auto& [Key,Child]:Item) Result.insert(QString::fromStdString(Key),Write(Child)); return Result; }
        else return Item;
    },Value.data_);
}
}
Result<ScriptValue> Decode(const QByteArray& Json) {
    if(Json.size()>1024*1024) return Error{ErrorCategory::BudgetExceeded,"wire-bytes"};
    QJsonParseError Error; const auto Document=QJsonDocument::fromJson("["+Json+"]",&Error);
    if(Error.error!=QJsonParseError::NoError || !Document.isArray() || Document.array().size()!=1) return hc::script::Error{ErrorCategory::InvalidRequest,"wire-json"};
    int Nodes=0; auto Value=Read(Document.array()[0],0,Nodes); if(!Value.HasValue()) return Value;
    auto Valid=Validate(Value.Value()); if(!Valid.HasValue()) return Valid.Failure(); return Value;
}
QByteArray Encode(const ScriptValue& Value) { const auto Bytes=QJsonDocument(QJsonArray{Write(Value)}).toJson(QJsonDocument::Compact); return Bytes.mid(1,Bytes.size()-2); }
QByteArray Call(IScriptHostApi& Api, std::string_view Method, const QByteArray& Arguments) {
    auto Parsed=Decode(Arguments); auto Result=Parsed.HasValue() ? Api.Call(Method,Parsed.Value()) : hc::script::Result<ScriptValue>(Parsed.Failure());
    if(Result.HasValue()) return Encode(ScriptValue::Object{{"ok",true},{"value",std::move(Result.Value())}});
    return Encode(ScriptValue::Object{{"ok",false},{"code",Result.Failure().code_},{"category",std::int64_t(Result.Failure().category_)}});
}
}
