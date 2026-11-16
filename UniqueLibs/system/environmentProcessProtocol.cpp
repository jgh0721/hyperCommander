#include "stdafx.h"
#include "environmentProcessProtocol.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QScopeGuard>
#include <QUuid>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentProcessProtocol, "hc.environment.processProtocol")
namespace hc::environment {
namespace {
constexpr qsizetype Maximum = 32 * 1024 * 1024, ChunkSize = 24 * 1024;
QJsonObject Failure(quint32 Error) { return {{"outcome", "Failed"}, {"nativeCode", QString::number(Error)}}; }
}
void ProcessEnvironmentServer::clear() { bytes_.fill(0); bytes_.clear(); hash_.clear(); id_.clear(); }
ProcessEnvironmentServer::~ProcessEnvironmentServer() { clear(); }
QJsonObject ProcessEnvironmentServer::Handle(const QString& Kind, const QJsonObject& Request)
{
    const auto Now = QDateTime::currentMSecsSinceEpoch();
    if (!id_.isEmpty() && Now - touched_ > 30000) clear();
    if (Kind == "process.environment") {
        clear();
        const auto Pid = Request["pid"].toInteger(-1); bool Valid = false;
        const auto Created = Request["created"].toString().toULongLong(&Valid);
        if (!Valid || !Created || Pid <= 0 || Pid > std::numeric_limits<quint32>::max()) return Failure(ERROR_INVALID_PARAMETER);
        const auto Result = read_({quint32(Pid), Created});
        QJsonArray Values;
        if (Result.status_ == ProcessReadStatus::Ok) for (const auto& [Name, Text] : Result.values_)
            Values.append(QJsonArray{QString::fromStdWString(Name), QString::fromStdWString(Text)});
        QJsonObject Object{{"status", int(Result.status_)}, {"error", QString::number(Result.nativeError_)}, {"wow64", Result.wow64_}, {"values", Values}};
        if (Result.currentDirectory_) Object["cwd"] = QString::fromStdWString(*Result.currentDirectory_);
        bytes_ = QJsonDocument(Object).toJson(QJsonDocument::Compact);
        if (bytes_.size() > Maximum) { clear(); return Failure(ERROR_BUFFER_OVERFLOW); }
        hash_ = QCryptographicHash::hash(bytes_, QCryptographicHash::Sha256); id_ = QUuid::createUuid().toString(QUuid::WithoutBraces); touched_ = Now;
        return {{"outcome", "Completed"}, {"snapshot", id_}, {"size", qint64(bytes_.size())}, {"sha256", QString::fromLatin1(hash_.toHex())}};
    }
    if (id_.isEmpty() || Request["snapshot"].toString() != id_) return Failure(ERROR_NOT_FOUND);
    touched_ = Now;
    if (Kind == "process.environment.release") { clear(); return {{"outcome", "Completed"}}; }
    if (Kind == "process.environment.chunk") {
        const auto Offset = Request["offset"].toInteger(-1);
        if (Offset < 0 || Offset >= bytes_.size() || Offset % ChunkSize) return Failure(ERROR_INVALID_PARAMETER);
        return {{"outcome", "Completed"}, {"offset", Offset}, {"data", QString::fromLatin1(bytes_.mid(Offset, ChunkSize).toBase64())}};
    }
    return Failure(ERROR_NOT_SUPPORTED);
}
ProcessEnvironment ReadRemoteEnvironment(const hc::system::ProcessKey& Target, const EnvironmentExchange& Exchange)
{
    ProcessEnvironment Result;
    const auto Begin = Exchange("process.environment", {{"pid", qint64(Target.pid)}, {"created", QString::number(Target.createTime)}});
    if (Begin["outcome"] != "Completed") { Result.nativeError_ = Begin["nativeCode"].toString().toUInt(); return Result; }
    const auto Id = Begin["snapshot"].toString(); const auto Size = Begin["size"].toInteger(-1);
    const auto Release = qScopeGuard([&] { Exchange("process.environment.release", {{"snapshot", Id}}); });
    if (QUuid(Id).isNull() || Size <= 0 || Size > Maximum) { Result.status_ = ProcessReadStatus::TooLarge; return Result; }
    QByteArray Bytes; Bytes.reserve(Size);
    const auto Clear = qScopeGuard([&] { Bytes.fill(0); });
    while (Bytes.size() < Size) {
        const auto Reply = Exchange("process.environment.chunk", {{"snapshot", Id}, {"offset", qint64(Bytes.size())}});
        if (Reply["outcome"] != "Completed" || Reply["offset"].toInteger(-1) != Bytes.size()) return Result;
        const auto Decoded = QByteArray::fromBase64Encoding(Reply["data"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!Decoded || Decoded.decoded.size() != std::min<qint64>(ChunkSize, Size - Bytes.size())) return Result;
        Bytes.append(Decoded.decoded);
    }
    if (QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()) != Begin["sha256"].toString()) return Result;
    QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Bytes, &Error); const auto Object = Document.object();
    const int Status = Object["status"].toInt(-1);
    if (Error.error != QJsonParseError::NoError || Status < 0 || Status > int(ProcessReadStatus::ReadFailed) || !Object["values"].isArray()) return Result;
    Result.status_ = ProcessReadStatus(Status); Result.nativeError_ = Object["error"].toString().toUInt(); Result.wow64_ = Object["wow64"].toBool();
    if (Result.status_ != ProcessReadStatus::Ok) return Result;
    ValueMap Values;
    for (const auto& Item : Object["values"].toArray()) {
        const auto Pair = Item.toArray();
        if (Pair.size() != 2 || !Pair[0].isString() || !Pair[1].isString()) { Result.status_ = ProcessReadStatus::InvalidBlock; return Result; }
        const auto Name = Pair[0].toString().toStdWString(), Text = Pair[1].toString().toStdWString();
        if (Name.empty() || Name.find(L'\0') != Name.npos || Text.find(L'\0') != Text.npos || !Values.emplace(Name, Text).second) { Result.status_ = ProcessReadStatus::InvalidBlock; return Result; }
    }
    Result.values_ = std::move(Values);
    if (Object["cwd"].isString()) Result.currentDirectory_ = Object["cwd"].toString().toStdWString();
    return Result;
}
}
