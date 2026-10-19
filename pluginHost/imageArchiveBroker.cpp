#include "imageArchiveBroker.hpp"
#include "archiveCodec.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "UniqueLibs/qtArchivePassword.hpp"
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileInfo>
#include <QUuid>
#include <QtEndian>
#include <Windows.h>
#include <map>
#include <QSet>
#include <atomic>
#include <algorithm>
#include <future>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <QScopeGuard>
namespace hc::plugins
{
using namespace storage;
struct ImageArchiveBroker::State
{
    QString address_ = QStringLiteral("hc-image-") + QUuid::createUuid().toString(QUuid::Id128),
            token_ = QUuid::createUuid().toString(QUuid::Id128);
    std::mutex mutex_;
    QSet<QString> roots_, files_;
    std::atomic<std::uint32_t> client_{0};
    std::jthread worker_;
    bool allowed(const QString &Path)
    {
        const QFileInfo File(Path);
        const auto Canonical = File.canonicalFilePath();
        if (Canonical.isEmpty() || !File.isFile())
            return false;
        std::lock_guard Lock(mutex_);
        return files_.contains(Canonical.toCaseFolded()) ||
               roots_.contains(QFileInfo(Canonical).absolutePath().toCaseFolded());
    }
};
namespace
{
Result<QByteArray> Read(QLocalSocket &Socket, qsizetype Count, OperationContext C)
{
    QByteArray Bytes;
    while (Bytes.size() < Count)
    {
        if (auto Stop = C.Check({"image-archive"}))
            return *Stop;
        if (!Socket.bytesAvailable() && !Socket.waitForReadyRead(50))
        {
            if (Socket.state() == QLocalSocket::UnconnectedState)
                return Error{ErrorCategory::Unavailable, "image-archive-disconnected"};
            continue;
        }
        Bytes += Socket.read(Count - Bytes.size());
    }
    return Bytes;
}
} // namespace
ImageArchiveBroker::ImageArchiveBroker(std::shared_ptr<State> Value) : state_(std::move(Value))
{
}
Result<std::unique_ptr<ImageArchiveBroker>> ImageArchiveBroker::Start()
{
    auto Shared = std::make_shared<State>();
    std::promise<bool> Started;
    auto Ready = Started.get_future();
    Shared->worker_ = std::jthread(
        [Shared, Started = std::move(Started)](std::stop_token Stop) mutable
        {
            QLocalServer Server;
            Server.setSocketOptions(QLocalServer::UserAccessOption);
            const bool Listening = Server.listen(Shared->address_);
            Started.set_value(Listening);
            if (!Listening)
                return;
            auto Options = BuiltinSevenZipOptions();
            Options.passwordPrompt_ = PromptArchivePassword;
            auto Backend = MakeApprovedWcxBackend(std::move(Options));
            std::map<QString, std::unique_ptr<IArchiveListing>> Listings;
            std::map<QString, std::unique_ptr<IArchiveReadSession>> Streams;
            std::map<QString, std::chrono::steady_clock::time_point> Expiry;
            while (!Stop.stop_requested())
            {
                for (auto At = Expiry.begin(); At != Expiry.end();)
                    if (At->second < std::chrono::steady_clock::now())
                    {
                        Listings.erase(At->first);
                        Streams.erase(At->first);
                        At = Expiry.erase(At);
                    }
                    else
                        ++At;
                if (!Server.waitForNewConnection(50))
                    continue;
                std::unique_ptr<QLocalSocket> Socket(Server.nextPendingConnection());
                if (!Socket)
                    continue;
                ULONG Pid = 0;
                if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(Socket->socketDescriptor()), &Pid) ||
                    !Shared->client_.load() || Pid != Shared->client_.load())
                    continue;
                std::stop_source Operation;
                std::stop_callback GlobalCancel(Stop, [&] { Operation.request_stop(); });
                const auto Pipe = reinterpret_cast<HANDLE>(Socket->socketDescriptor());
                std::jthread Disconnect(
                    [Pipe, &Operation](std::stop_token Done)
                    {
                        std::mutex Mutex;
                        std::condition_variable_any Wake;
                        std::unique_lock Lock(Mutex);
                        while (!Done.stop_requested())
                        {
                            if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, nullptr, nullptr))
                            {
                                Operation.request_stop();
                                return;
                            }
                            Wake.wait_for(Lock, Done, std::chrono::milliseconds(25), [] { return false; });
                        }
                    });
                OperationContext C;
                C.stopToken_ = Operation.get_token();
                C.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                C.passwordPrompt_ = PromptArchivePassword;
                auto SizeBytes = Read(*Socket, 4, C);
                if (!SizeBytes.HasValue())
                    continue;
                const auto Size = qFromLittleEndian<quint32>(SizeBytes.Value().constData());
                if (Size > 64 * 1024)
                    continue;
                auto Data = Read(*Socket, Size, C);
                if (!Data.HasValue())
                    continue;
                const auto Doc = QJsonDocument::fromJson(Data.Value());
                if (!Doc.isObject())
                    continue;
                const auto Request = Doc.object();
                if (Request["token"].toString() != Shared->token_)
                    continue;
                QJsonObject Reply;
                QByteArray Bytes;
                QString Resource;
                auto Rollback = qScopeGuard(
                    [&]
                    {
                        if (!Resource.isEmpty())
                        {
                            Listings.erase(Resource);
                            Streams.erase(Resource);
                            Expiry.erase(Resource);
                        }
                    });
                const auto Fail = [&](const Error &E) { Reply = {{"error", QString::fromStdString(E.code_)}}; };
                const auto Op = Request["op"].toString(), Id = Request["id"].toString(),
                           Path = Request["path"].toString();
                if (Op == "open" || Op == "stream")
                {
                    const auto Ext = QFileInfo(Path).suffix().toLower();
                    if (!Shared->allowed(Path) || !QStringList{"zip", "cbz", "rar", "cbr", "7z", "cb7"}.contains(Ext) ||
                        Listings.size() + Streams.size() >= 32)
                        Reply = {{"error", "image-archive-source-denied"}};
                    else if (Op == "open")
                    {
                        auto Value = Backend->Enumerate(Path.toUtf8().toStdString(), false, C);
                        if (!Value.HasValue())
                            Fail(Value.Failure());
                        else
                        {
                            const auto Key = QUuid::createUuid().toString(QUuid::Id128);
                            Listings.emplace(Key, std::move(Value.Value()));
                            Resource = Key;
                            Reply["id"] = Key;
                        }
                    }
                    else
                    {
                        auto Ordinal = WireUnsigned(Request["ordinal"]);
                        if (!Ordinal.HasValue() || Ordinal.Value() > UINT32_MAX)
                            Reply = {{"error", "image-archive-ordinal"}};
                        else
                        {
                            auto Value = Backend->OpenReadSession(Path.toUtf8().toStdString(),
                                                                  {std::uint32_t(Ordinal.Value())}, C);
                            if (!Value.HasValue())
                                Fail(Value.Failure());
                            else
                            {
                                const auto Key = QUuid::createUuid().toString(QUuid::Id128);
                                Streams.emplace(Key, std::move(Value.Value()));
                                Resource = Key;
                                Reply["id"] = Key;
                            }
                        }
                    }
                }
                else if (Op == "next" && Listings.contains(Id))
                {
                    Resource = Id;
                    auto Value = Listings[Id]->Next(size_t(std::clamp(Request["count"].toInt(), 1, 32)), C);
                    if (!Value.HasValue())
                    {
                        Fail(Value.Failure());
                        Listings.erase(Id);
                    }
                    else
                    {
                        QJsonArray Entries;
                        for (const auto &E : Value.Value().entries_)
                            Entries.append(EncodeArchiveHeader(E));
                        Reply["entries"] = Entries;
                        Reply["end"] = Value.Value().end_;
                        if (Value.Value().end_)
                            Listings.erase(Id);
                    }
                }
                else if (Op == "read" && Streams.contains(Id))
                {
                    Resource = Id;
                    auto Ordinal = WireUnsigned(Request["ordinal"]);
                    if (!Ordinal.HasValue() || Ordinal.Value() > UINT32_MAX)
                        Reply = {{"error", "image-archive-ordinal"}};
                    else
                    {
                        auto Value = Streams[Id]->Read(std::uint32_t(Ordinal.Value()),
                                                       size_t(std::clamp(Request["count"].toInt(), 1, 192 * 1024)), C);
                        if (!Value.HasValue())
                        {
                            Fail(Value.Failure());
                            Streams.erase(Id);
                        }
                        else
                        {
                            Reply["eof"] = Value.Value().eof_;
                            const auto &Raw = Value.Value().bytes_;
                            Bytes = QByteArray(reinterpret_cast<const char *>(Raw.data()), qsizetype(Raw.size()));
                            if (Value.Value().eof_)
                                Streams.erase(Id);
                        }
                    }
                }
                else if (Op == "release")
                {
                    Listings.erase(Id);
                    Streams.erase(Id);
                    Expiry.erase(Id);
                    Reply["released"] = true;
                }
                else
                    Reply = {{"error", "image-archive-operation"}};
                Reply["bytes"] = Bytes.size();
                const auto Json = QJsonDocument(Reply).toJson(QJsonDocument::Compact);
                if (Json.size() > 1024 * 1024)
                    continue;
                QByteArray Packet(4, Qt::Uninitialized);
                qToLittleEndian<quint32>(quint32(Json.size()), Packet.data());
                Packet += Json;
                Packet += Bytes;
                if (Socket->write(Packet) != Packet.size())
                    continue;
                while (Socket->bytesToWrite() && !C.Check({"image-archive"}))
                {
                    if (!Socket->waitForBytesWritten(50) && Socket->state() == QLocalSocket::UnconnectedState)
                        break;
                }
                auto Ack = Read(*Socket, 1, C);
                if (!Ack.HasValue() || Ack.Value() != QByteArray(1, char(6)))
                    continue;
                if (Socket->write(QByteArray(1, char(6))) != 1)
                    continue;
                while (Socket->bytesToWrite() && !C.Check({"image-archive"}))
                    Socket->waitForBytesWritten(50);
                if (Socket->bytesToWrite())
                    continue;
                if (Listings.contains(Resource) || Streams.contains(Resource))
                    Expiry[Resource] = std::chrono::steady_clock::now() + std::chrono::minutes(2);
                else
                    Expiry.erase(Resource);
                Rollback.dismiss();
                Socket->disconnectFromServer();
            }
            Streams.clear();
            Listings.clear();
            CloseApprovedWcxBackend(Backend);
            Backend.reset();
            Server.close();
        });
    if (!Ready.get())
    {
        if (Shared->worker_.joinable())
            Shared->worker_.join();
        return Error{ErrorCategory::Unavailable, "image-archive-listen"};
    }
    return std::unique_ptr<ImageArchiveBroker>(new ImageArchiveBroker(Shared));
}
ImageArchiveBroker::~ImageArchiveBroker()
{
    state_->worker_.request_stop();
    if (state_->worker_.joinable())
        state_->worker_.join();
}
QJsonObject ImageArchiveBroker::Endpoint() const
{
    return {{"address", state_->address_}, {"token", state_->token_}};
}
void ImageArchiveBroker::BindClient(std::uint32_t Process)
{
    state_->client_ = Process;
}
void ImageArchiveBroker::Allow(const QString &Path)
{
    const QFileInfo File(Path);
    const auto Canonical = File.canonicalFilePath();
    if (Canonical.isEmpty())
        return;
    std::lock_guard Lock(state_->mutex_);
    state_->files_.insert(Canonical.toCaseFolded());
    state_->roots_.insert(QFileInfo(Canonical).absolutePath().toCaseFolded());
}
} // namespace hc::plugins
