#include "imageArchiveTransport.hpp"
#include "archiveCodec.hpp"
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QtEndian>
#include <cstring>
#include <algorithm>
namespace hc::plugins
{
using namespace storage;
namespace
{
Error Failure(const char *Code)
{
    return {ErrorCategory::Unavailable, Code, {"image-archive"}};
}
Result<QByteArray> Read(QLocalSocket &Socket, qsizetype Count, OperationContext Context)
{
    QByteArray Bytes;
    while (Bytes.size() < Count)
    {
        if (auto Stop = Context.Check({"image-archive"}))
            return *Stop;
        if (!Socket.bytesAvailable() && !Socket.waitForReadyRead(50))
        {
            if (Socket.state() == QLocalSocket::UnconnectedState)
                return Failure("image-archive-disconnected");
            continue;
        }
        Bytes += Socket.read(Count - Bytes.size());
    }
    return Bytes;
}
class Listing final : public IArchiveListing
{
  public:
    Listing(QJsonObject Endpoint, QString Id) : endpoint_(std::move(Endpoint)), id_(std::move(Id))
    {
    }
    ~Listing() override
    {
        OperationContext C;
        C.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        CallImageArchive(endpoint_, {{"op", "release"}, {"id", id_}}, C);
    }
    Result<ArchivePage> Next(std::size_t Count, OperationContext C) override
    {
        auto Reply = CallImageArchive(endpoint_,
                                      {{"op", "next"}, {"id", id_}, {"count", int((std::min)(Count, size_t(128)))}}, C);
        if (!Reply.HasValue())
            return Reply.Failure();
        ArchivePage Page;
        Page.end_ = Reply.Value().json_["end"].toBool();
        const auto Entries = Reply.Value().json_["entries"].toArray();
        if (Entries.size() > 128)
            return Failure("image-archive-page");
        for (const auto &Entry : Entries)
        {
            auto Header = DecodeArchiveHeader(Entry.toObject());
            if (!Header.HasValue())
                return Header.Failure();
            Page.entries_.push_back(Header.Value());
        }
        return Page;
    }

  private:
    QJsonObject endpoint_;
    QString id_;
};
class Stream final : public IArchiveReadSession
{
  public:
    Stream(QJsonObject Endpoint, QString Id) : endpoint_(std::move(Endpoint)), id_(std::move(Id))
    {
    }
    ~Stream() override
    {
        OperationContext C;
        C.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        CallImageArchive(endpoint_, {{"op", "release"}, {"id", id_}}, C);
    }
    Result<ReadChunk> Read(std::uint32_t Ordinal, std::size_t Count, OperationContext C) override
    {
        auto Reply = CallImageArchive(endpoint_,
                                      {{"op", "read"},
                                       {"id", id_},
                                       {"ordinal", QString::number(Ordinal)},
                                       {"count", int((std::min)(Count, size_t(192 * 1024)))}},
                                      C);
        if (!Reply.HasValue())
            return Reply.Failure();
        ReadChunk Out;
        const auto &Bytes = Reply.Value().bytes_;
        Out.bytes_.resize(Bytes.size());
        if (!Bytes.isEmpty())
            std::memcpy(Out.bytes_.data(), Bytes.constData(), Bytes.size());
        Out.eof_ = Reply.Value().json_["eof"].toBool();
        return Out;
    }

  private:
    QJsonObject endpoint_;
    QString id_;
};
class Backend final : public IArchiveBackend
{
  public:
    explicit Backend(QJsonObject Endpoint) : endpoint_(std::move(Endpoint))
    {
    }
    Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Path, bool, OperationContext C) override
    {
        auto Reply = CallImageArchive(endpoint_, {{"op", "open"}, {"path", QString::fromStdString(Path)}}, C);
        if (!Reply.HasValue())
            return Reply.Failure();
        return std::unique_ptr<IArchiveListing>(new Listing(endpoint_, Reply.Value().json_["id"].toString()));
    }
    Result<std::unique_ptr<IArchiveReadSession>> OpenReadSession(std::string Path, std::vector<std::uint32_t> Ordinals,
                                                                 OperationContext C) override
    {
        if (Ordinals.empty() || Ordinals.size()>128)
            return Failure("image-archive-single-entry");
        QJsonArray Indices;for(const auto Ordinal:Ordinals)Indices.append(QString::number(Ordinal));
        auto Reply = CallImageArchive(
            endpoint_,
            {{"op", "stream"}, {"path", QString::fromStdString(Path)}, {"ordinals",Indices}},
            C);
        if (!Reply.HasValue())
            return Reply.Failure();
        return std::unique_ptr<IArchiveReadSession>(new Stream(endpoint_, Reply.Value().json_["id"].toString()));
    }
    Result<Unit> Extract(std::string, ArchiveHeader, std::string, OperationContext) override
    {
        return Error{ErrorCategory::Unsupported, "image-archive-read-only"};
    }
    Result<Unit> Update(ArchiveUpdate, OperationContext) override
    {
        return Error{ErrorCategory::Unsupported, "image-archive-read-only"};
    }

  private:
    QJsonObject endpoint_;
};
} // namespace
Result<ImageArchiveReply> CallImageArchive(const QJsonObject &Endpoint, QJsonObject Request, OperationContext Context)
{
    if (auto Stop = Context.Check({"image-archive"}))
        return *Stop;
    Context.deadline_ = (std::min)(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(35));
    QLocalSocket Socket;
    Socket.connectToServer(Endpoint["address"].toString());
    while (Socket.state() != QLocalSocket::ConnectedState)
    {
        if (auto Stop = Context.Check({"image-archive"}))
            return *Stop;
        if (!Socket.waitForConnected(50) && Socket.state() == QLocalSocket::UnconnectedState)
            return Failure("image-archive-connect");
    }
    Request["token"] = Endpoint["token"];
    Request["context"] = EncodeContext(Context, 30000);
    const auto Json = QJsonDocument(Request).toJson(QJsonDocument::Compact);
    if (Json.size() > 64 * 1024)
        return Failure("image-archive-request");
    QByteArray Packet(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(quint32(Json.size()), Packet.data());
    Packet += Json;
    if (Socket.write(Packet) != Packet.size())
        return Failure("image-archive-write");
    while (Socket.bytesToWrite())
    {
        if (auto Stop = Context.Check({"image-archive"}))
            return *Stop;
        Socket.waitForBytesWritten(50);
    }
    auto Header = Read(Socket, 4, Context);
    if (!Header.HasValue())
        return Header.Failure();
    const auto Size = qFromLittleEndian<quint32>(Header.Value().constData());
    if (Size > 1024 * 1024)
        return Failure("image-archive-response");
    auto Body = Read(Socket, Size, Context);
    if (!Body.HasValue())
        return Body.Failure();
    const auto Doc = QJsonDocument::fromJson(Body.Value());
    if (!Doc.isObject())
        return Failure("image-archive-response");
    const auto Reply = Doc.object();
    if (Reply.contains("error"))
        return Error{ErrorCategory::Unavailable, Reply["error"].toString().toStdString(), {"image-archive"}};
    const int BinarySize = Reply["bytes"].toInt();
    if (BinarySize < 0 || BinarySize > 192 * 1024)
        return Failure("image-archive-response");
    auto Bytes = Read(Socket, BinarySize, Context);
    if (!Bytes.HasValue())
        return Bytes.Failure();
    if (Socket.write(QByteArray(1, char(6))) != 1) return Failure("image-archive-ack");
    while (Socket.bytesToWrite()) {
        if (auto Stop = Context.Check({"image-archive"})) return *Stop;
        Socket.waitForBytesWritten(50);
    }
    auto Ack = Read(Socket, 1, Context);
    if (!Ack.HasValue() || Ack.Value() != QByteArray(1, char(6))) return Failure("image-archive-ack");
    return ImageArchiveReply{Reply, Bytes.Value()};
}
std::shared_ptr<IArchiveBackend> MakeImageArchiveClient(QJsonObject Endpoint)
{
    return std::make_shared<Backend>(std::move(Endpoint));
}
} // namespace hc::plugins
