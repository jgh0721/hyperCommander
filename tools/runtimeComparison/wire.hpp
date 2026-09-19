#pragma once
#include "commonLibs/scripting/scriptApi.hpp"
#include <QByteArray>
#include <QJsonValue>
namespace hc::script::comparison {
Result<ScriptValue> Decode(const QByteArray& Json);
QByteArray Encode(const ScriptValue& Value);
QByteArray Call(IScriptHostApi& Api, std::string_view Method, const QByteArray& Arguments);
}
