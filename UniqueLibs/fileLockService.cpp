#include "stdafx.h"
#include "fileLockService.hpp"

/// Project headers
#include "adminChannel.hpp"
#include "nativeFileLocks.hpp"

/// Platform headers
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

namespace hc::filelocks {
namespace {
const hc::admin::ChannelSpec& Spec() {
    static const hc::admin::ChannelSpec Value{"hc.fileLocks", 1, "HyperCommander-fileLocks-", "--file-lock-session", 65536};
    return Value;
}
QString id() { return hc::admin::NewId(); }
QJsonObject publicRow(QJsonObject Row, const QString& Capability) {
    Row.remove("kernelPath");
    // Verification retains full native paths in the server, independently of
    // display truncation and the bounded transport frame.
    for (const auto& Key : {"path", "image", "user"}) if (Row[Key].toString().size() > 4096) Row[Key] = Row[Key].toString().left(4095) + QChar(0x2026);
    Row["capability"] = Capability; return Row;
}
}

FileLockClient::FileLockClient(QStringList Paths, bool Elevated, QString Helper, Launcher Launch)
    : paths_(std::move(Paths)), channel_(std::make_unique<hc::admin::ChannelClient>(Spec(), Elevated, std::move(Helper), std::move(Launch))) {
    valid_ = !paths_.isEmpty() && paths_.size() <= 32;
    for (const auto& Path : paths_) if (!IsLocalLockPath(Path)) valid_ = false;
    QJsonArray Targets; for (const auto& Path : paths_) Targets.append(Path);
    channel_->SetBootstrap({{"paths", Targets}});
}
FileLockClient::~FileLockClient() = default;
void FileLockClient::Cancel() { channel_->Cancel(); }
QJsonObject FileLockClient::exchange(QJsonObject Message, unsigned long Timeout) {
    const bool Action = Message["kind"] == "action";
    if (!valid_) { channel_->Cancel(); return hc::admin::TransportError(false, ERROR_INVALID_PARAMETER); }
    return channel_->Exchange(std::move(Message), Timeout, Action);
}
QJsonObject FileLockClient::Query() {
    std::lock_guard Lock(mutex_);
    auto Reply = exchange({{"kind","query"},{"requestId",id()}}, 30000);
    QJsonArray Rows = Reply["rows"].toArray();
    const auto Generation = Reply["generation"].toString();
    int Next = Reply["next"].toInt(-1);
    while (Next >= 0 && !channel_->Cancelled() && Rows.size() < 2000) {
        auto Page = exchange({{"kind","page"},{"requestId",id()},{"generation",Generation},{"offset",Next}}, 5000);
        if (Page["outcome"] != "Completed" || Page["generation"].toString() != Generation) { Reply["partial"] = true; Reply["outcome"] = "Unavailable"; break; }
        for (const auto& Row : Page["rows"].toArray()) Rows.append(Row);
        const int Previous = Next; Next = Page["next"].toInt(-1);
        if (Next >= 0 && Next <= Previous) { Reply["partial"] = true; break; }
    }
    Reply["rows"] = Rows; return Reply;
}
QJsonObject FileLockClient::Act(const QString& Capability, const QString& Generation, const QString& Action, const QString& RequestId) {
    std::lock_guard Lock(mutex_);
    return exchange({{"kind","action"},{"requestId",RequestId},{"capability",Capability},{"generation",Generation},{"action",Action},{"approved",true}}, 10000);
}

unsigned long ExecuteFileLockSession(const QJsonObject& Bootstrap, const QString& ExpectedCaller) {
    if (Bootstrap["paths"].toArray().isEmpty() || Bootstrap["paths"].toArray().size() > 32) return ERROR_INVALID_PARAMETER;
    QStringList Paths;
    for (const auto& Value : Bootstrap["paths"].toArray()) {
        if (!Value.isString() || !IsLocalLockPath(Value.toString())) return ERROR_INVALID_PARAMETER;
        Paths.append(Value.toString());
    }
    hc::admin::AcceptedChannel Channel;
    if (const auto Code = hc::admin::AcceptChannel(Bootstrap, Spec(), ExpectedCaller, Channel)) return Code;
    const auto Pid = Channel.parentPid; const auto& Nonce = Channel.nonce;
    QJsonArray Rows; QHash<QString,QJsonObject> Capabilities, Receipts;
    QString Generation; QDateTime Queried; bool Partial = false; QStringList Reasons;
    while (Channel.ParentAlive()) {
        const auto Request = hc::admin::ReceiveFrame(Channel.pipe, Spec(), Nonce, 120000);
        if (!Request) return ERROR_BROKEN_PIPE;
        const auto Kind = (*Request)["kind"].toString(), RequestId = (*Request)["requestId"].toString();
        if (QUuid(RequestId).isNull()) return ERROR_INVALID_PARAMETER;
        QJsonObject Reply{{"requestId",RequestId},{"outcome","Rejected"}};
        if (Kind == "query") {
            Capabilities.clear(); Rows = {}; Generation = id();
            auto Found = Scan(Paths, Pid, [&]{return !Channel.ParentAlive();});
            Partial = Found.partial_; Reasons = Found.reasons_; Queried = QDateTime::currentDateTimeUtc();
            for (const auto& Value : Found.rows_) { const auto Capability = id(); Capabilities.insert(Capability, Value.toObject()); Rows.append(publicRow(Value.toObject(),Capability)); }
        }
        if (Kind == "query" || (Kind == "page" && (*Request)["generation"].toString() == Generation)) {
            int Offset = Kind == "query" ? 0 : (*Request)["offset"].toInt(-1);
            if (Offset >= 0 && Offset <= Rows.size()) {
                QJsonArray Page;
                while (Offset < Rows.size() && Page.size() < 128) {
                    auto Candidate = Page; Candidate.append(Rows[Offset]);
                    if (QJsonDocument(Candidate).toJson(QJsonDocument::Compact).size() > 58000) break;
                    Page = Candidate; ++Offset;
                }
                Reply["outcome"] = "Completed"; Reply["rows"] = Page; Reply["generation"] = Generation;
                Reply["next"] = Offset < Rows.size() ? Offset : -1; Reply["partial"] = Partial;
                Reply["reasons"] = QJsonArray::fromStringList(Reasons);
            }
        } else if (Kind == "action") {
            if (Receipts.contains(RequestId)) Reply = Receipts.value(RequestId);
            else if (Receipts.size() < 128 && (*Request)["approved"].isBool() && (*Request)["approved"].toBool()
                && (*Request)["generation"].toString() == Generation && !Queried.isNull() && Queried.msecsTo(QDateTime::currentDateTimeUtc()) <= 120000
                && Capabilities.contains((*Request)["capability"].toString())) {
                const auto Row = Capabilities.value((*Request)["capability"].toString());
                Capabilities.clear(); // Any attempt consumes every capability from this snapshot.
                Reply = ExecuteAction(Row, (*Request)["action"].toString(), Pid); Reply["requestId"] = RequestId;
                Receipts.insert(RequestId, Reply);
            }
        }
        if (!hc::admin::SendFrame(Channel.pipe, Reply, Spec(), Nonce)) return ERROR_BROKEN_PIPE;
    }
    return ERROR_SUCCESS;
}
}
