#include "UniqueLibs/document/hwpExportTransfer.hpp"
#include "UniqueLibs/document/hwpAsyncSave.hpp"
#include "UniqueLibs/document/hwpEditBridge.hpp"
#include "commonLibs/document/documentSession.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "UniqueLibs/document/documentStorageAdapter.hpp"
using namespace hc::document;
namespace {
    DocumentOperationTicket Ticket()
    { return {"document", 2, 3, 4, DocumentOperation::Exporting, {5, 6, "snapshot"}, {}}; }
    QByteArray Bytes() { return QByteArray::fromHex("d0cf11e0a1b11ae1") + QByteArray(400000, 'x'); }
    QJsonObject Message(const HwpExportTransfer& Transfer, const char* Type)
    { return {{"version", 1}, {"type", Type}, {"identity", Transfer.Identity()}}; }
    QJsonObject Begin(const HwpExportTransfer& Transfer, const QByteArray& Data)
    {
        auto Value = Message(Transfer, "ExportBegin"); Value["format"] = "hwp";
        Value["byteLength"] = Data.size(); Value["sha256"] = QString::fromLatin1(QCryptographicHash::hash(Data, QCryptographicHash::Sha256).toHex());
        Value["loss"] = QJsonObject{{"schemaVersion", 1}, {"outputFormat", "hwp"}, {"count", 0}, {"losses", QJsonArray{}}}; return Value;
    }
    bool Feed(HwpExportTransfer& Transfer, const QByteArray& Data)
    {
        if (Transfer.Handle(Begin(Transfer, Data))["type"] != "ExportAccepted") return false;
        for (qsizetype Offset = 0; Offset < Data.size(); Offset += 192 * 1024) {
            auto Chunk = Message(Transfer, "ExportChunk"); Chunk["offset"] = Offset;
            Chunk["data"] = QString::fromLatin1(Data.mid(Offset, 192 * 1024).toBase64());
            if (Transfer.Handle(Chunk)["type"] != "ExportChunkAccepted") return false;
        }
        return Transfer.Handle(Message(Transfer, "ExportCommit"))["type"] == "ExportReady";
    }
}
class HwpExportTest : public QObject {
    Q_OBJECT
private slots:
    void editBridgeNativeGrantAndModel() {
        HwpEditBridge Bridge; auto Load = Ticket(); Load.operation_ = DocumentOperation::Loading;
        const auto Pin = QByteArray(64, 'a');
        QVERIFY(!Bridge.Activate(Load, 7, 8, Pin, false)); QVERIFY(Bridge.State().isEmpty());
        QVERIFY(Bridge.Activate(Load, 7, 8, Pin, true)); QVERIFY(!Bridge.Activate(Load, 7, 8, Pin, true));
        HwpEditEndpoint Endpoint(&Bridge, nullptr); const auto Meta = Endpoint.metaObject();
        for (const auto Name : {"Activate", "Retire", "BeginExport", "NotifySaved", "ModelChanged"})
            for (int I = Meta->methodOffset(); I < Meta->methodCount(); ++I) QVERIFY(Meta->method(I).name() != Name);
        auto Changed = QJsonObject{{"version",1},{"type","Changed"},{"binding",Bridge.State()["binding"]},{"sequence","1"}};
        const auto Send = [&](QJsonObject Value) { return Endpoint.Report(QString::fromUtf8(QJsonDocument(Value).toJson())); };
        auto Invalid = Changed; Invalid["sequence"] = 1; QVERIFY(!Send(Invalid));
        Invalid = Changed; Invalid["sequence"] = "2"; QVERIFY(!Send(Invalid));
        Invalid = Changed; Invalid["binding"] = QJsonObject{}; QVERIFY(!Send(Invalid));
        QVERIFY(Send(Changed)); QCOMPARE(Bridge.Model().changeSequence_, quint64(1));
        QVERIFY(!Send(Changed)); QVERIFY(Bridge.Model().savepoint_ != Pin.toStdString());
        Bridge.Retire(); Changed["sequence"] = "2"; QVERIFY(!Send(Changed));
        QVERIFY(!Bridge.Activate(Load, 7, 8, Pin, true));
    }
    void editBridgeExportBackpressureAndStale() {
        HwpEditBridge Bridge; auto Load = Ticket(); Load.operation_ = DocumentOperation::Loading;
        QVERIFY(Bridge.Activate(Load, 7, 8, QByteArray(64,'a'), true));
        auto Export = Ticket(); Export.model_ = Bridge.Model();
        HwpExportTransfer Transfer(Export, 7, 8, "hwp");
        QVERIFY(Bridge.BeginExport(Transfer.Identity(), "hwp"));
        auto Command = Bridge.PendingCommand();
        const auto Send = [&](QJsonObject Message) {
            return Bridge.Report(QString::fromUtf8(QJsonDocument(QJsonObject{{"version",1},{"binding",Bridge.State()["binding"]},
                {"type","ExportMessage"},{"id",Command["id"]},{"message",Message}}).toJson()));
        };
        const auto Data = Bytes(); QVERIFY(Send(Begin(Transfer, Data)));
        QVERIFY(!Send(Begin(Transfer, Data))); auto Out = Bridge.TakeExportMessage(); QVERIFY(Out);
        QCOMPARE(Transfer.Handle(*Out)["type"].toString(), QString("ExportAccepted"));
        for (qsizetype Offset = 0; Offset < Data.size(); Offset += 192 * 1024) {
            auto Chunk = Message(Transfer, "ExportChunk"); Chunk["offset"] = Offset;
            Chunk["data"] = QString::fromLatin1(Data.mid(Offset,192*1024).toBase64());
            QVERIFY(Send(Chunk)); QVERIFY(!Send(Chunk)); Out = Bridge.TakeExportMessage(); QVERIFY(Out);
            QCOMPARE(Transfer.Handle(*Out)["type"].toString(),QString("ExportChunkAccepted"));
        }
        QVERIFY(Send(Message(Transfer,"ExportCommit"))); Out = Bridge.TakeExportMessage(); QVERIFY(Out);
        QCOMPARE(Transfer.Handle(*Out)["type"].toString(),QString("ExportReady"));
        QVERIFY(Bridge.NotifySaved(Bridge.Model()));
        const auto Saved = Bridge.PendingCommand();
        auto Ack = QJsonObject{{"version",1},{"binding",Bridge.State()["binding"]},{"type","SavedObserved"},{"id",Command["id"]}};
        QVERIFY(!Bridge.Report(QString::fromUtf8(QJsonDocument(Ack).toJson())));
        Ack["id"] = Saved["id"]; QVERIFY(Bridge.Report(QString::fromUtf8(QJsonDocument(Ack).toJson())));
        QVERIFY(!Send(Message(Transfer,"ExportCommit")));
        QVERIFY(Bridge.BeginExport(Transfer.Identity(),"hwp")); Command = Bridge.PendingCommand();
        auto Changed = QJsonObject{{"version",1},{"type","Changed"},{"binding",Bridge.State()["binding"]},{"sequence","1"}};
        QVERIFY(Bridge.Report(QString::fromUtf8(QJsonDocument(Changed).toJson())));
        QVERIFY(!Send(Begin(Transfer,Data))); QCOMPARE(Bridge.Error(),QString("rhwp-edit-export-stale"));
    }
    void editorRetirementAndCloseBarrier() {
        HwpEditBridge Bridge; auto Load = Ticket(); Load.operation_ = DocumentOperation::Loading;
        QVERIFY(Bridge.Activate(Load, 7, 8, QByteArray(64,'a'), true));
        const auto Send = [&](const char* Type, QJsonObject Fields) {
            Fields["type"] = Type; Fields["version"] = 1; Fields["binding"] = Bridge.State()["binding"];
            return Bridge.Report(QString::fromUtf8(QJsonDocument(Fields).toJson()));
        };
        auto Export = Ticket(); Export.model_ = Bridge.Model(); HwpExportTransfer Transfer(Export,7,8,"hwp");
        QVERIFY(Bridge.BeginExport(Transfer.Identity(),"hwp")); const QJsonValue Old = Bridge.PendingCommand()["id"];
        QVERIFY(Send("Failed",{{"id",Old}}));
        // A serialization error must never disable delivery of subsequent edits.
        QVERIFY(Send("Changed",{{"sequence","1"}}));
        Bridge.DiscardExport(); const QJsonValue Discard = Bridge.PendingCommand()["id"];
        QVERIFY(Bridge.State()["retiring"].toBool()); QVERIFY(!Bridge.Synchronize(QString(32,'a')));
        QVERIFY(!Send("CommandObserved",{{"id",Old}}));
        QVERIFY(Send("CommandObserved",{{"id",Discard}})); QVERIFY(!Bridge.State()["retiring"].toBool());
        QVERIFY(Bridge.Synchronize(QString(32,'a')));
        QVERIFY(Send("Changed",{{"sequence","2"}}));
        QVERIFY(Send("Synchronized",{{"id",QString(32,'a')}}));
        QCOMPARE(Bridge.State()["synchronized"].toString(),QString(32,'a'));
        QCOMPARE(Bridge.Model().changeSequence_,quint64(2));
        QVERIFY(Bridge.Resume()); QVERIFY(Bridge.State()["resuming"].toBool());
        QVERIFY(Send("CommandObserved",{{"id",Bridge.PendingCommand()["id"]}}));
        QVERIFY(!Bridge.State()["resuming"].toBool()); QVERIFY(Bridge.State()["synchronized"].toString().isEmpty());
    }
    void checkpointExpiredBeforeAdmissionRetainsOperation() {
        DocumentSession Session("document",{},DocumentContentMode::ExternalModel,{true,true,true,true});
        const auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        QVERIFY(Session.ApplyModelChange("document",Session.Snapshot().generation_,{5,1,"edited"},false,false));
        const auto Operation=Session.BeginCheckpoint();QVERIFY(Operation.HasValue());
        HwpExportTransfer Transfer(Operation.Value(),7,8,"hwp",100);
        QVERIFY(Feed(Transfer,Bytes()));QTest::qWait(130);
        auto Result=Transfer.AdmitCheckpoint(Session);QVERIFY(!Result.HasValue());QVERIFY(Session.IsBusy());
        QCOMPARE(Result.Failure().code_,std::string("rhwp-export-timeout"));
        // The owner must still finish this exact ticket, then retire the renderer operation.
        QVERIFY(!Session.CompleteCheckpoint(Operation.Value(),Result.Failure()).HasValue());
        QVERIFY(!Session.IsBusy());QVERIFY(Session.Snapshot().dirty_);
    }
    void verifiedOneShot() {
        HwpExportTransfer Transfer(Ticket(), 7, 8, "hwp"); QVERIFY(!Transfer.Take());
        QVERIFY(Feed(Transfer, Bytes())); auto Payload = Transfer.Take(); QVERIFY(Payload);
        QCOMPARE(Payload->bytes_, Bytes().toStdString()); QVERIFY(Payload->model_ == Ticket().model_);
        QVERIFY(!Transfer.Take()); QCOMPARE(Transfer.CurrentState(), HwpExportTransfer::State::Consumed);
    }
    void lossBlocked_data() {
        QTest::addColumn<int>("Mode"); for (int Index = 0; Index < 5; ++Index) QTest::newRow(qPrintable(QString::number(Index))) << Index;
    }
    void lossBlocked() {
        QFETCH(int, Mode); HwpExportTransfer Transfer(Ticket(), 7, 8, "hwp"); auto Request = Begin(Transfer, Bytes());
        auto Loss = Request["loss"].toObject();
        if (Mode == 0) Request.remove("loss");
        else { if (Mode == 1) Loss["count"] = 1; if (Mode == 2) Loss["outputFormat"] = "hwpx";
            if (Mode == 3) Loss["losses"] = QJsonArray{QJsonObject{}}; if (Mode == 4) Loss["schemaVersion"] = 2; Request["loss"] = Loss; }
        QVERIFY(Transfer.Handle(Request).contains("error")); QVERIFY(!Transfer.Take());
    }
    void staleDoesNotConsumeCurrent() {
        HwpExportTransfer Transfer(Ticket(), 7, 8, "hwp"); auto Old = Begin(Transfer, Bytes());
        auto Identity = Old["identity"].toObject(); Identity["revision"] = "999"; Old["identity"] = Identity;
        QCOMPARE(Transfer.Handle(Old)["error"].toString(), QString("rhwp-export-stale")); QVERIFY(Feed(Transfer, Bytes()));
    }
    void orderingIntegrityAndContainer_data() {
        QTest::addColumn<int>("Mode"); for (int Index = 0; Index < 3; ++Index) QTest::newRow(qPrintable(QString::number(Index))) << Index;
    }
    void orderingIntegrityAndContainer() {
        QFETCH(int, Mode); HwpExportTransfer Transfer(Ticket(), 7, 8, "hwp"); auto Data = Bytes().left(100);
        if (Mode == 2) Data.fill('x'); auto Request = Begin(Transfer, Data);
        if (Mode == 1) Request["sha256"] = QString(64, '0'); Transfer.Handle(Request);
        auto Chunk = Message(Transfer, "ExportChunk"); Chunk["offset"] = Mode == 0 ? 1 : 0;
        Chunk["data"] = QString::fromLatin1(Data.toBase64()); Transfer.Handle(Chunk);
        QVERIFY(Transfer.Handle(Message(Transfer, "ExportCommit")).contains("error")); QVERIFY(!Transfer.Take());
    }
    void timeoutAndCancel() {
        HwpExportTransfer Expired(Ticket(), 7, 8, "hwp", 1); QTest::qSleep(5); QVERIFY(!Expired.Take());
        QCOMPARE(Expired.CurrentState(), HwpExportTransfer::State::Failed);
        HwpExportTransfer Cancelled(Ticket(), 7, 8, "hwp"); QVERIFY(Feed(Cancelled, Bytes())); Cancelled.Cancel(); QVERIFY(!Cancelled.Take());
    }
    void asyncPreparationNegotiationAndCancellation() {
        DocumentSession Session("document", {}, DocumentContentMode::ExternalModel, {true,true,true,true});
        auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        HwpAsyncSave Unsupported; const SaveRequest Request{SaveDisposition::SaveAs,"copy.hwp"};
        QVERIFY(Unsupported.Begin(Session,Request,{"saveStream"},true,"hwp",7,8).contains("error"));
        QVERIFY(!Session.IsBusy());
        QVERIFY(Unsupported.Begin(Session,Request,{"saveStream","asyncExportV1"},false,"hwp",7,8).contains("error"));
        HwpAsyncSave Save;const auto Accepted=Save.Begin(Session,Request,{"saveStream","asyncExportV1"},true,"hwp",7,8);
        QCOMPARE(Accepted["state"].toString(),QString("accepted"));const auto Token=Accepted["exportToken"].toString();
        QElapsedTimer Timer;Timer.start();QCOMPARE(Save.Poll(Session,Token)["state"].toString(),QString("preparing"));
        QVERIFY(Timer.elapsed()<1000);QVERIFY(Save.Read(Token,0).contains("error"));
        QVERIFY(Save.Poll(Session,"stale").contains("error"));QVERIFY(Session.IsBusy());
        Save.RequestCancel(Session);QVERIFY(Session.IsBusy());
        QCOMPARE(Save.Poll(Session,Token)["state"].toString(),QString("cancelling"));
        QVERIFY(Save.Retired(Session));QVERIFY(!Session.IsBusy());QVERIFY(!Save.Retired(Session));
    }
    void incompleteAdmissionDoesNotAcknowledgeRetirement() {
        DocumentSession Session("document",{},DocumentContentMode::ExternalModel,{true,true,true,true});
        auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        const auto Operation=Session.BeginExport({SaveDisposition::SaveAs,"copy.hwp"}).Value();
        HwpExportTransfer Transfer(Operation,7,8,"hwp",1);QTest::qSleep(5);
        const auto Result=Transfer.Admit(Session);QVERIFY(!Result.HasValue());
        QCOMPARE(Result.Failure().code_,std::string("rhwp-export-timeout"));QVERIFY(Session.IsBusy());
        Session.Cancel();QVERIFY(Session.CompleteCancellation(Operation));QVERIFY(!Session.IsBusy());
    }
    void staleTimeoutCannotCancelNextOperation() {
        DocumentSession Session("document",{},DocumentContentMode::ExternalModel,{true,true,true,true});
        auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        HwpAsyncSave Old;const SaveRequest Request{SaveDisposition::SaveAs,"copy.hwp"};
        const auto Started=Old.Begin(Session,Request,{"asyncExportV1","saveStream"},true,"hwp",7,8,1);
        const auto Identity=Old.Identity();const auto State=Session.Snapshot();
        DocumentOperationTicket Original{State.id_,State.generation_,Identity["token"].toString().toULongLong(),State.revision_,DocumentOperation::Exporting,State.model_,State.source_.lease_};
        QVERIFY(Session.Cancel(Original));QVERIFY(Session.CompleteCancellation(Original));
        const auto Next=Session.BeginExport(Request);QVERIFY(Next.HasValue());QTest::qSleep(5);
        QCOMPARE(Old.Poll(Session,Started["exportToken"].toString())["error"].toString(),QString("rhwp-async-export-stale"));
        Old.RequestCancel(Session);QVERIFY(!Session.Snapshot().cancelRequested_);QVERIFY(Session.IsBusy());
        QVERIFY(!Old.Retired(Session));QVERIFY(Session.Cancel(Next.Value()));QVERIFY(Session.CompleteCancellation(Next.Value()));
    }
    void asyncTimeoutRequiresRetirement() {
        DocumentSession Session("document",{},DocumentContentMode::ExternalModel,{true,true,true,true});
        auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        HwpAsyncSave Save;const SaveRequest Request{SaveDisposition::SaveAs,"copy.hwp"};
        const auto Started=Save.Begin(Session,Request,{"asyncExportV1","saveStream"},true,"hwp",7,8,1);
        QTest::qSleep(5);const auto Reply=Save.Poll(Session,Started["exportToken"].toString());
        QCOMPARE(Reply["state"].toString(),QString("cancelling"));
        QCOMPARE(Reply["error"].toString(),QString("rhwp-export-timeout"));
        QVERIFY(Session.IsBusy());QVERIFY(!Session.BeginExport(Request).HasValue());
        QVERIFY(!Session.Capabilities().close_);QVERIFY(Save.Retired(Session));QVERIFY(!Session.IsBusy());
        QVERIFY(Save.Poll(Session,Started["exportToken"].toString()).contains("error"));
    }
    void asyncStreamAndReceipt() {
        const auto Root=qEnvironmentVariable("HC_RHWP_TEST_EXPORTS");if(Root.isEmpty())QSKIP("Real export fixture required");
        QFile File(Root+"/c02.hwp.hwp");QVERIFY(File.open(QIODevice::ReadOnly));const auto Data=File.readAll();
        DocumentSession Session("document",{},DocumentContentMode::ExternalModel,{true,true,true,true});
        auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        QVERIFY(Session.ApplyModelChange("document",Session.Snapshot().generation_,{5,1,"edited"},true,false));
        HwpAsyncSave Save;const auto Accepted=Save.Begin(Session,{SaveDisposition::SaveAs,"copy.hwp"},{"asyncExportV1","saveStream"},true,"hwp",7,8);
        const auto Token=Accepted["exportToken"].toString();QVERIFY(!Token.isEmpty());
        auto Message=[&](const char* Type){return QJsonObject{{"version",1},{"type",Type},{"identity",Save.Identity()}};};
        auto Start=Message("ExportBegin");Start["format"]="hwp";Start["byteLength"]=Data.size();
        Start["sha256"]=QString::fromLatin1(QCryptographicHash::hash(Data,QCryptographicHash::Sha256).toHex());
        Start["loss"]=QJsonObject{{"schemaVersion",1},{"outputFormat","hwp"},{"count",0},{"losses",QJsonArray{}}};
        QCOMPARE(Save.Accept(Start)["type"].toString(),QString("ExportAccepted"));
        auto Chunk=Message("ExportChunk");Chunk["offset"]=0;Chunk["data"]=QString::fromLatin1(Data.toBase64());
        QCOMPARE(Save.Accept(Chunk)["type"].toString(),QString("ExportChunkAccepted"));
        QCOMPARE(Save.Accept(Message("ExportCommit"))["type"].toString(),QString("ExportReady"));
        const auto Ready=Save.Poll(Session,Token);QCOMPARE(Ready["state"].toString(),QString("streamReady"));
        const auto SaveToken=Ready["saveToken"].toString();QVERIFY(SaveToken!=Token);
        QVERIFY(!Save.Complete(Session,SaveToken,DocumentSaveResult{true}));
        QVERIFY(Save.Read("stale",0).contains("error"));const auto Part=Save.Read(SaveToken,0);
        QCOMPARE(QByteArray::fromBase64(Part["data"].toString().toLatin1()),Data);QVERIFY(Part["eof"].toBool());
        QVERIFY(Save.Read(SaveToken,0).contains("error"));
        QVERIFY(Session.ApplyModelChange("document",Session.Snapshot().generation_,{5,2,"newer"},true,false));
        Save.RequestCancel(Session);QVERIFY(Save.Complete(Session,SaveToken,DocumentSaveResult{true}));
        QVERIFY(Session.Snapshot().dirty_);QVERIFY(!Session.IsBusy());QVERIFY(!Save.Complete(Session,SaveToken,DocumentSaveResult{true}));
    }
    void nativeSaveKeepsNewerEdits() {
        const auto Root = qEnvironmentVariable("HC_RHWP_TEST_EXPORTS");
        if (Root.isEmpty()) QSKIP("Real engine export fixtures not requested");
        QFile Input(Root + "/c02.hwp.hwp"); QVERIFY(Input.open(QIODevice::ReadOnly)); const auto Data = Input.readAll();
        QCOMPARE(QCryptographicHash::hash(Data,QCryptographicHash::Sha256).toHex(), QByteArray("f4e179f14588caca7ba90a1a266ed93dc684e5c5b897018419baec4a7edbfd41"));
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); const auto Destination = Directory.filePath("saved.hwp");
        DocumentSession Session("document", {}, DocumentContentMode::ExternalModel, {true,true,true,true});
        auto Load = Session.BeginLoad().Value(); QVERIFY(Session.CompleteLoad(Load, LoadedExternalModel{{}, {5,0,"base"}}));
        QVERIFY(Session.ApplyModelChange("document",Session.Snapshot().generation_,{5,1,"edited"},true,false));
        const auto Operation = Session.BeginExport({SaveDisposition::SaveAs,Destination.toStdString()}); QVERIFY(Operation.HasValue());
        HwpExportTransfer Transfer(Operation.Value(),7,8,"hwp"); QVERIFY(Feed(Transfer,Data));
        auto Save = Transfer.Admit(Session); QVERIFY(Save.HasValue());
        QVERIFY(Session.ApplyModelChange("document",Session.Snapshot().generation_,{5,2,"newer"},true,false));
        auto Written = SaveTextFile(Destination,QByteArray::fromStdString(Save.Value().text_),hc::storage::DestinationExpectation{},{});
        QVERIFY(Written.HasValue()); QVERIFY(Session.CompleteSave(Save.Value(),DocumentSaveResult{true}));
        QVERIFY(Session.Snapshot().dirty_); QCOMPARE(Session.Snapshot().savedModel_.savepoint_,std::string("edited"));
        QFile Stored(Destination);QVERIFY(Stored.open(QIODevice::ReadOnly));QCOMPARE(Stored.readAll(),Data);
    }
    void sessionRevisionFence() {
        DocumentSession Session("document", {}, DocumentContentMode::ExternalModel, {true,true,true,true});
        auto Load = Session.BeginLoad(); QVERIFY(Load.HasValue());
        QVERIFY(Session.CompleteLoad(Load.Value(), LoadedExternalModel{{}, {5, 0, "base"}}));
        QVERIFY(Session.ApplyModelChange("document", Session.Snapshot().generation_, {5, 1, "edited"}, true, false));
        auto Export = Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}); QVERIFY(Export.HasValue());
        HwpExportTransfer Transfer(Export.Value(), 7, 8, "hwp"); QVERIFY(Feed(Transfer, Bytes()));
        QVERIFY(Session.ApplyModelChange("document", Session.Snapshot().generation_, {5, 2, "newer"}, true, false));
        auto Payload = Transfer.Take(); QVERIFY(Payload); QVERIFY(!Session.CompleteExport(Export.Value(), std::move(*Payload)).HasValue());
        QVERIFY(Session.Snapshot().dirty_); QVERIFY(!Session.IsBusy());
    }
};
QTEST_GUILESS_MAIN(HwpExportTest)
#include "tstHwpExport.moc"
