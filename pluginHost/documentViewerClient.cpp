#include "stdafx.h"
#include "documentViewerClient.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <thread>
#include <future>
#include <QProcess>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <algorithm>
#include <iterator>
#include "UniqueLibs/settings/appSettings.hpp"
namespace hc::plugins
{
    using namespace storage;
    Result<QJsonObject> DocumentViewerClient::Call(QString Type, QJsonObject Payload, QString Document, QString Generation, OperationContext Context)
    {
        const auto Id = QString::number(++sequence_);
        QJsonObject Envelope{{"protocolVersion", int(document::HostVersion)}, {"requestId", Id}, {"workspaceId", workspace_},
            {"documentId", Document}, {"generation", Generation}, {"messageType", Type},
            {"payloadLength", QJsonDocument(Payload).toJson(QJsonDocument::Compact).size()}, {"payload", Payload}};
        auto Reply = host_.ViewerCall(WireKind::DocumentCommand, Envelope, Context);
        if (!Reply.HasValue()) return Reply.Failure();
        const auto& Message = Reply.Value();
        if (Message["protocolVersion"].toInt() != document::HostVersion || Message["requestId"].toString() != Id
            || !Message["payload"].isObject() || Message["payloadLength"].toInteger(-1) != QJsonDocument(Message["payload"].toObject()).toJson(QJsonDocument::Compact).size()
            || (!workspace_.isEmpty() && Message["workspaceId"].toString() != workspace_))
            return Error{ErrorCategory::Unavailable, "document-host-envelope"};
        const auto Body = Message["payload"].toObject();
        if (Message["messageType"].toString() == "Error") return Error{ErrorCategory::InvalidRequest, Body["code"].toString("document-host-error").toStdString()};
        if (Type == "Hello") {
            if (Message["messageType"].toString() != "Ready" || Message["workspaceId"].toString().size() != 32
                || !Body["capabilities"].toArray().contains("saveStream")) return Error{ErrorCategory::Unsupported, "document-host-capabilities"};
            workspace_ = Message["workspaceId"].toString();
            for (const auto& Extension : Body["imageExtensions"].toArray())
                if (Extension.isString()) imageExtensions_.insert(Extension.toString());
        }
        if (Type == "OpenDocuments") for (const auto& Item : Body["documents"].toArray())
            if (Item.toObject().contains("error")) return Error{ErrorCategory::InvalidRequest, Item.toObject()["error"].toString().toStdString()};
        return Body;
    }
    Result<Unit> DocumentViewerClient::Hello(OperationContext Context)
    {
        const auto Deadline = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(10));
        do {
            if (auto Stop = Context.Check({"document"})) return *Stop;
            auto Ready = Call("Hello", {}, {}, {}, Context);
            if (Ready.HasValue()) return Unit{};
            if (Ready.Failure().code_ != "document-host-envelope" && Ready.Failure().code_ != "host-document-not-ready") return Ready.Failure();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < Deadline);
        return Error{ErrorCategory::Unavailable, "document-helper-startup-timeout"};
    }
    QString DocumentViewerClient::Bind(const std::shared_ptr<IContentSource>& Source, std::shared_ptr<IWritableStorage> Writer,
        std::shared_ptr<IMaterializedFileLease> Lease, QByteArray Hash, bool EnableArchiveNavigation)
    {
        const auto Entry = Source->Describe();
        const auto Content = std::dynamic_pointer_cast<ProviderContentSource>(Source);
        const auto Token = broker_.Bind(Entry, std::move(Writer), std::move(Lease), std::move(Hash),
            Content ? Content->GetArchiveWriteback() : ProviderContentSource::ArchiveWriteback{});
        if (!EnableArchiveNavigation || Token.isEmpty() || !Content || Entry.locator_.kind_ != LocatorKind::Archive
            || Entry.locator_.nesting_.empty() || !imageExtensions_.contains(QFileInfo(QString::fromUtf8(Entry.name_)).suffix().toLower()))
            return Token;
        auto Folder = Entry.locator_;
        auto& Path = Folder.nesting_.back().entry_;
        const auto Slash = Path.rfind('/');
        if (const auto Provider = Content->ReadProvider(); Provider && Slash != std::string::npos) {
            Path.resize(std::max(std::size_t{1}, Slash));
            archiveImages_.emplace(Token, ArchiveImage{std::make_shared<ArchiveFolder>(ArchiveFolder{Provider, std::move(Folder)}), Entry.id_});
        }
        return Token;
    }

    Result<QJsonObject> DocumentViewerClient::prepareArchiveImage(const QString& Token, int Direction, IMaterializer& Materializer, OperationContext Context)
    {
        const auto At = archiveImages_.find(Token);
        if (At == archiveImages_.end() || (Direction != 1 && Direction != -1))
            return Error{ErrorCategory::InvalidRequest, "document-archive-image-source"};
        const auto& Folder = At->second.folder_;
        Context.deadline_ = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(30));
        if (!Folder->images_) {
            auto Cursor = Folder->provider_->Enumerate(Folder->location_, Context).get();
            if (!Cursor.HasValue()) return Cursor.Failure();
            std::vector<EntrySnapshot> Images;
            for (;;) {
                if (const auto Stop = Context.Check(Folder->provider_->Id())) return *Stop;
                auto Page = Cursor.Value()->Next(Context.maxPageItems_, Context).get();
                if (!Page.HasValue()) return Page.Failure();
                for (auto& Entry : Page.Value().entries_)
                    if (!Entry.directory_ && imageExtensions_.contains(QFileInfo(QString::fromUtf8(Entry.name_)).suffix().toLower()))
                        Images.push_back(std::move(Entry));
                if (Page.Value().end_) break;
            }
            std::stable_sort(Images.begin(), Images.end(), [](const EntrySnapshot& Left, const EntrySnapshot& Right) {
                return QString::fromUtf8(Left.name_) < QString::fromUtf8(Right.name_);
            });
            Folder->images_ = std::move(Images);
        }
        const auto& Images = *Folder->images_;
        const auto Current = std::find_if(Images.begin(), Images.end(), [&](const auto& Entry) { return Entry.id_ == At->second.id_; });
        if (Current == Images.end()) return Error{ErrorCategory::StaleRevision, "document-archive-image-changed"};
        if (Images.size() == 1) return QJsonObject{};
        const auto Count = std::ssize(Images);
        const auto& Entry = Images[(std::distance(Images.begin(), Current) + Direction + Count) % Count];
        auto Lease = Materializer.Materialize({std::make_shared<ProviderContentSource>(Folder->provider_, Entry)}, Context).get();
        if (!Lease.HasValue()) return Lease.Failure();
        const auto NextToken = broker_.Bind(Entry, {}, Lease.Value());
        if (NextToken.isEmpty()) return Error{ErrorCategory::InvalidRequest, "document-source-binding"};
        archiveImages_.emplace(NextToken, ArchiveImage{Folder, Entry.id_});
        return QJsonObject{{"path", QString::fromUtf8(Lease.Value()->LocalPath())}, {"displayName", QString::fromUtf8(Entry.name_)},
            {"sourceToken", NextToken}, {"entry", QString::fromUtf8(Entry.locator_.nesting_.back().entry_)}};
    }
    Result<QJsonObject> DocumentViewerClient::Execute(QJsonObject Request, OperationContext Context)
    {
        const auto Id = Request.take("documentId").toString();
        auto Generation = Request.take("generation").toString();
        if (!Id.isEmpty() && Generation.isEmpty()) for (const auto& Item : state_["documents"].toArray())
            if (Item.toObject()["documentId"].toString() == Id) Generation = Item.toObject()["generation"].toString();
        auto Result = Call("Execute", std::move(Request), Id, Generation, Context);
        if (Result.HasValue() && Result.Value().contains("error")) return Error{ErrorCategory::InvalidRequest, Result.Value()["error"].toString().toStdString()};
        return Result;
    }
    Result<QJsonObject> DocumentViewerClient::Poll(OperationContext Context, IMaterializer* Materializer)
    {
        auto Current = Call("DocumentState", {}, {}, {}, Context); if (!Current.HasValue()) return Current.Failure();
        state_ = Current.Value();
        std::set<QString> Tokens;
        for (const auto& Item : state_["documents"].toArray()) {
            const auto Tab = Item.toObject();
            const auto Token = Tab["sourceToken"].toString(); if (!Token.isEmpty()) Tokens.insert(Token);
            const auto Archive = Tab["archiveNavigationToken"].toString(); if (!Archive.isEmpty()) Tokens.insert(Archive);
            const auto Pending = Tab["navigationSourceToken"].toString(); if (!Pending.isEmpty()) Tokens.insert(Pending);
            if (!Tab["loading"].toBool()) broker_.CaptureLoadedHash(Token, QByteArray::fromHex(Tab["inputSha256"].toString().toLatin1()));
        }
        broker_.Retain(Tokens);
        std::erase_if(archiveImages_, [&](const auto& Binding) { return !Tokens.contains(Binding.first); });
        for (const auto& Item : state_["documents"].toArray()) {
            const auto Tab = Item.toObject();
            const auto Step = Tab["archiveImageStep"].toObject();
            if (Step.isEmpty()) continue;
            auto Next = Materializer
                ? prepareArchiveImage(Tab["archiveNavigationToken"].toString(), Step["direction"].toInt(), *Materializer, Context)
                : Result<QJsonObject>(Error{ErrorCategory::Unavailable, "document-archive-image-materializer"});
            auto Reply = Next.HasValue() ? std::move(Next.Value()) : QJsonObject{{"failure", QString::fromStdString(Next.Failure().code_)}};
            Reply["action"] = "archiveImageResult";
            Reply["request"] = Step["request"];
            auto Completed = Call("Execute", Reply, Tab["documentId"].toString(), Tab["generation"].toString(), Context);
            if (!Completed.HasValue() && Completed.Failure().code_ != "document-stale-id"
                && Completed.Failure().code_ != "document-stale-generation") return Completed.Failure();
        }
        const auto Edit=state_["editRequest"].toObject();
        // A new EPUB from HyperViewer's Ctrl+N: no document and no path cross this boundary.
        // HyperEditor shows the dialog and creates the file itself (--new-epub).
        if(Edit["format"]=="epub-new" || Edit["format"]=="hwpx-new"){
            const auto Program=QCoreApplication::applicationDirPath()+"/HyperEditor.exe";
            QStringList Args{"--config-dir",hc::settings::DocumentPaths().directory_,Edit["format"]=="hwpx-new" ? "--new-hwpx" : "--new-epub"};
            const auto Appearance=QJsonDocument(Edit["appearance"].toObject()).toJson(QJsonDocument::Compact).toBase64();
            if(!Edit["appearance"].toObject().isEmpty() && Appearance.size()<=16384)Args<<"--document-appearance"<<QString::fromLatin1(Appearance);
            auto Start=std::make_shared<std::promise<bool>>();auto Result=Start->get_future();
            std::thread([Program,Args,Start]{
                QProcess Process;Process.start(Program,Args);const bool Started=Process.waitForStarted(5000);
                Start->set_value(Started);if(Started)Process.waitForFinished(-1);
            }).detach();
            const bool Started=Result.get();
            auto Completed=Call("Execute",{{"action","editResult"},{"token",Edit["token"]},{"accepted",Started}},{},{},Context);
            if(!Completed.HasValue()&&Completed.Failure().code_!="document-edit-token")return Completed.Failure();
        }
        // 탐색기의 ‘HyperEditor에서 편집’ · ‘HyperEditor에서 폴더 열기’: 로컬 파일 · 폴더 경로만 넘긴다.
        // 워크스페이스 묶기 · 상위 찾기 · 열린 창 고르기는 HyperEditor 가 같은 규칙으로 한다.
        else if(Edit["format"]=="explorer"){
            const auto Path=Edit["path"].toString();const QFileInfo Info(Path);const bool Folder=Edit["folder"].toBool();
            bool Started=false;
            if(Info.isAbsolute() && !Path.contains(QChar::Null) && Path.size()<=32767 && (Folder?Info.isDir():Info.isFile())){
                const auto Program=QCoreApplication::applicationDirPath()+"/HyperEditor.exe";
                const QStringList Args{"--config-dir",hc::settings::DocumentPaths().directory_,"--",Info.absoluteFilePath()};
                auto Start=std::make_shared<std::promise<bool>>();auto Result=Start->get_future();
                std::thread([Program,Args,Start]{
                    QProcess Process;Process.start(Program,Args);const bool Launched=Process.waitForStarted(5000);
                    Start->set_value(Launched);if(Launched)Process.waitForFinished(-1);
                }).detach();
                Started=Result.get();
            }
            auto Completed=Call("Execute",{{"action","editResult"},{"token",Edit["token"]},{"accepted",Started}},{},{},Context);
            if(!Completed.HasValue()&&Completed.Failure().code_!="document-edit-token")return Completed.Failure();
        }
        else if(!Edit.isEmpty()){
            const bool Epub=Edit["format"]=="epub";
            const bool Search=Edit["format"]=="search";
            std::optional<document::DocumentHostBroker::EditorInput> Input;
            for(const auto& Item:state_["documents"].toArray()){
                const auto Tab=Item.toObject();const auto View=Tab["view"].toObject();
                if(Tab["documentId"]!=Edit["documentId"]||Tab["generation"]!=Edit["generation"]
                    ||Tab["lifecycle"]!="ready"||Tab["loading"].toBool()||!View["ready"].toBool()
                    ||(Search?!QStringList{"text","epub"}.contains(View["viewId"].toString()):Epub?View["viewId"]!="epub":(View["viewId"]!="hwp"&&View["viewId"]!="hwpx")))continue;
                const auto SourceToken=Tab["sourceToken"].toString();
                if(!SourceToken.isEmpty())Input=broker_.ResolveEditorInput(SourceToken);
                else if(!Tab["hostSlot"].toBool())Input=document::DocumentHostBroker::EditorInput{Tab["path"].toString(),true,{}};
                break;
            }
            bool Started=false;
            if(Input&&QFileInfo(Input->path_).isAbsolute()&&(Search|| (Epub?QFileInfo(Input->path_).suffix().compare("epub",Qt::CaseInsensitive)==0:QStringList{"hwp","hwpx"}.contains(QFileInfo(Input->path_).suffix().toLower())))){
                const auto Program=QCoreApplication::applicationDirPath()+"/HyperEditor.exe";
                QStringList Args{"--config-dir",hc::settings::DocumentPaths().directory_};
                bool ArgumentsValid=true;
                if(Search) {
                    auto SearchContext=Edit["search"].toObject();
                    if(!Input->original_) { SearchContext["root"]=QFileInfo(Input->path_).absolutePath(); Args<<"--document-copy"; }
                    const auto Encoded=QJsonDocument(SearchContext).toJson(QJsonDocument::Compact).toBase64();
                    ArgumentsValid=Encoded.size()<=16384;
                    Args<<"--find-in-files"<<QString::fromLatin1(Encoded);
                    if(QFileInfo(Input->path_).suffix().compare("epub",Qt::CaseInsensitive)==0)Args<<"--epub-edit";
                } else Args<<(Epub?"--epub-edit":Input->original_?"--rhwp-edit":"--rhwp-edit-copy");
                if(Epub) {
                    const auto Locator=Edit["locator"].toString();const auto Chapter=Edit["chapter"].toInt(-1);
                    if(Locator.size()<=4096 && Locator.startsWith("epubcfi(") && Locator.endsWith(')') && !Locator.contains(QChar::Null))Args<<"--epub-locator"<<Locator;
                    if(Chapter>=0 && Chapter<4096)Args<<"--epub-chapter"<<QString::number(Chapter);
                    const auto Appearance=QJsonDocument(Edit["appearance"].toObject()).toJson(QJsonDocument::Compact).toBase64();
                    if(!Edit["appearance"].toObject().isEmpty() && Appearance.size()<=16384)Args<<"--document-appearance"<<QString::fromLatin1(Appearance);
                }
                Args<<"--"<<Input->path_;
                if(ArgumentsValid){
                auto Start=std::make_shared<std::promise<bool>>();auto Result=Start->get_future();
                // Run outside the restricted viewer Job. Retain provider input
                // until the independent editor exits, even after the tab closes.
                std::thread([Program,Args,Lease=Input->lease_,Start]{
                    QProcess Process;Process.start(Program,Args);const bool Started=Process.waitForStarted(5000);
                    Start->set_value(Started);if(Started)Process.waitForFinished(-1);
                }).detach();
                Started=Result.get();
                }
            }
            auto Completed=Call("Execute",{{"action","editResult"},{"token",Edit["token"]},{"accepted",Started}},{},{},Context);
            // Closing the originating tab can retire its token while the
            // independent editor is starting. That reply does not fail the viewer.
            if(!Completed.HasValue()&&Completed.Failure().code_!="document-edit-token")return Completed.Failure();
        }
        for (const auto& Item : state_["documents"].toArray()) {
            const auto Tab = Item.toObject();
            if (!Tab["archiveUpdating"].toBool()) continue;
            auto Result = broker_.CommitArchive(Tab["sourceToken"].toString(), Tab["archiveDiscard"].toBool(), Context);
            Result["action"] = "archiveResult";
            auto Replied = Call("Execute", Result, Tab["documentId"].toString(), Tab["generation"].toString(), Context);
            if (!Replied.HasValue()) return Replied.Failure();
        }
        for (const auto& Item : state_["pendingSaves"].toArray()) {
            const auto Prepared = Item.toObject(); bool Valid = false;
            const auto Length = Prepared["byteLength"].toString().toULongLong(&Valid);
            const auto Id = Prepared["documentId"].toString(), Generation = Prepared["generation"].toString();
            QByteArray Bytes; std::optional<Error> Failure;
            if (!Valid || Length > quint64(std::numeric_limits<qsizetype>::max())) Failure = Error{ErrorCategory::BudgetExceeded, "document-save-stream-length"};
            while (!Failure && quint64(Bytes.size()) < Length) {
                auto Chunk = Call("SaveRead", {{"saveToken", Prepared["saveToken"]}, {"offset", QString::number(Bytes.size())},
                    {"count", int(document::HostChunkLimit)}}, Id, Generation, Context);
                if (!Chunk.HasValue()) { Failure = Chunk.Failure(); break; }
                const auto Encoded = Chunk.Value()["bytes"].toString().toLatin1();
                if (Encoded.size() > ((document::HostChunkLimit + 2) / 3) * 4) { Failure = Error{ErrorCategory::BudgetExceeded, "document-save-stream-chunk"}; break; }
                const auto Decoded = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (Decoded.isEmpty() || Decoded.toBase64() != Encoded || quint64(Decoded.size()) > Length - quint64(Bytes.size())
                    || Chunk.Value()["offset"].toString() != QString::number(Bytes.size())
                    || Chunk.Value()["eof"].toBool() != (quint64(Bytes.size() + Decoded.size()) == Length)) {
                    Failure = Error{ErrorCategory::InvalidRequest, "document-save-stream-corrupt"}; break;
                }
                Bytes += Decoded;
            }
            QJsonObject Result;
            if (Failure) Result = {{"action", "commitSave"}, {"documentId", Id}, {"saveToken", Prepared["saveToken"]},
                {"revision", Prepared["revision"]}, {"committed", false}, {"error", QString::fromStdString(Failure->code_)}, {"errorCategory", int(Failure->category_)}};
            else Result = broker_.CommitBytes(Prepared, Bytes, Context);
            auto Completed = Call("SaveResult", Result, Id, Generation, Context); if (!Completed.HasValue()) return Completed.Failure();
        }
        return state_;
    }
}
