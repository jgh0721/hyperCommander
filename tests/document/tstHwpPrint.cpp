#include "UniqueLibs/document/hwpPrintSnapshot.hpp"
#include "commonLibs/document/documentSession.hpp"
#include <QBuffer>
#include "UniqueLibs/document/hwpPrintBridge.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QImage>
#include <QtTest>
using namespace hc::document;
namespace {
    DocumentOperationTicket Ticket(){return {"print",2,3,4,DocumentOperation::Printing,{5,6,"snapshot"},{}};}
    HwpPrintPage Page(int Index,QSizeF Paper={794,1123}) {
        QImage Image(200,300,QImage::Format_RGB32);Image.fill(Index%2?Qt::blue:Qt::white);
        QByteArray Bytes;QBuffer Output(&Bytes);Output.open(QIODevice::WriteOnly);Image.save(&Output,"PNG");return {Index,Paper,Bytes};
    }
}
class HwpPrintTests:public QObject {
    Q_OBJECT
private slots:
    void streamedPrintBindingAndIntegrity() {
        const auto T=Ticket();const auto P=Page(0);
        const auto Message=[&](const char* Type,QJsonObject Extra={}) {Extra["version"]=1;Extra["type"]=Type;Extra["identity"]=HwpPrintIdentity(T);return Extra;};
        HwpPrintTransfer Transfer(T);
        QVERIFY(Transfer.Handle(Message("PrintBegin",{{"totalPages",1}})));
        QVERIFY(Transfer.Handle(Message("PrintPageBegin",{{"page",0},{"width",794},{"height",1123},{"byteLength",P.png_.size()},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(P.png_,QCryptographicHash::Sha256).toHex())}})));
        QVERIFY(Transfer.Handle(Message("PrintChunk",{{"offset",0},{"data",QString::fromLatin1(P.png_.toBase64())}})));
        QVERIFY(Transfer.Handle(Message("PrintPageEnd")));QVERIFY(!Transfer.Ready());
        QVERIFY(Transfer.Handle(Message("PrintCommit")));QVERIFY(Transfer.Ready());
        QVERIFY(BuildHwpPdf(T,Transfer.Pages(),Transfer.Frames()).HasValue());
        QVERIFY(!Transfer.Handle(Message("PrintCommit")));
        HwpPrintTransfer Stale(T);auto M=Message("PrintBegin",{{"totalPages",1}});auto Id=M["identity"].toObject();Id["token"]="99";M["identity"]=Id;
        QVERIFY(!Stale.Handle(M));QCOMPARE(Stale.Error(),QString("rhwp-print-stale"));
        HwpPrintTransfer Partial(T);QVERIFY(Partial.Handle(Message("PrintBegin",{{"totalPages",1}})));
        QVERIFY(!Partial.Handle(Message("PrintCommit")));
        HwpPrintTransfer Budget(T);QVERIFY(Budget.Handle(Message("PrintBegin",{{"totalPages",1}})));
        QVERIFY(Budget.Handle(Message("PrintPageBegin",{{"page",0},{"width",794},{"height",1123},{"byteLength",17*1024*1024},{"sha256",QString(64,'a')}})));
        HwpPrintTransfer Range(T,{1});QVERIFY(!Range.Handle(Message("PrintBegin",{{"totalPages",1}})));
    }
    void printCancellationRequiresRendererRetirement() {
        HwpPrintBridge Bridge;auto Id=HwpPrintIdentity(Ticket());auto Binding=Id;
        for(const auto* K:{"revision","token","changeSequence","savepoint"})Binding.remove(K);
        Bridge.Activate(Binding);
        const auto Invalid=QString::fromUtf8(QJsonDocument(QJsonObject{{"version",1},{"binding",Binding},{"type","PrintRetired"},{"id","late"}}).toJson(QJsonDocument::Compact));
        QVERIFY(!Bridge.Report(Invalid));QVERIFY(Bridge.Idle());QVERIFY(Bridge.TakeMessage().isEmpty());QVERIFY(Bridge.Idle());QVERIFY(Bridge.Begin(Id,{}));QVERIFY(!Bridge.Begin(Id,{}));
        const auto Command=Bridge.Pending();Bridge.Cancel();QVERIFY(!Bridge.Idle());
        const auto Report=[&](QString Type,QString Request){return QString::fromUtf8(QJsonDocument(QJsonObject{{"version",1},{"binding",Binding},{"type",Type},{"id",Request}}).toJson(QJsonDocument::Compact));};
        QVERIFY(!Bridge.Report(Report("PrintRetired","stale")));QVERIFY(!Bridge.Idle());
        QVERIFY(Bridge.Report(Report("PrintRetired",Command["id"].toString())));QVERIFY(Bridge.Idle());
        QCOMPARE(Bridge.Retired(),Id);QVERIFY(Bridge.Begin(Id,{}));QVERIFY(Bridge.Retired().isEmpty());
        auto Stale=Id;Stale["token"]="999";QVERIFY(!Bridge.Cancel(Stale));
    }
    void allPagesAndRange() {
        HwpPrintFrames Frames{Ticket(),3,{Page(0),Page(1),Page(2)}};
        auto All=BuildHwpPdf(Ticket(),{0,1,2},Frames);QVERIFY(All.HasValue());QVERIFY(All.Value().startsWith("%PDF-"));
        Frames.pages_={Page(1,{1123,794})};auto Selected=BuildHwpPdf(Ticket(),{1},Frames);QVERIFY(Selected.HasValue());
        QVERIFY(All.Value().size()>Selected.Value().size());
    }
    void rejectsStaleAndPartial() {
        HwpPrintFrames Frames{Ticket(),3,{Page(0)}};QVERIFY(!BuildHwpPdf(Ticket(),{0,1},Frames).HasValue());
        Frames.ticket_.token_++;QVERIFY(!BuildHwpPdf(Ticket(),{0},Frames).HasValue());
        Frames.ticket_=Ticket();Frames.pages_={Page(0),Page(0)};QVERIFY(!BuildHwpPdf(Ticket(),{0,0},Frames).HasValue());
    }
    void rejectsImageAndGeometry() {
        HwpPrintFrames Frames{Ticket(),1,{Page(0)}};Frames.pages_[0].png_="invalid";
        QVERIFY(!BuildHwpPdf(Ticket(),{0},Frames).HasValue());Frames.pages_={Page(0,{0,100})};
        QVERIFY(!BuildHwpPdf(Ticket(),{0},Frames).HasValue());
    }
    void printingNeverSavesChanges() {
        ExternalModelPolicy Policy;Policy.edit_=true;Policy.print_=true;
        DocumentSession Session("print",{},DocumentContentMode::ExternalModel,Policy);
        auto Load=Session.BeginLoad().Value();QVERIFY(Session.CompleteLoad(Load,LoadedExternalModel{{},{5,0,"base"}}));
        QVERIFY(Session.ApplyModelChange("print",Session.Snapshot().generation_,{5,1,"edited"},true,false));
        const auto Before=Session.Snapshot();auto Print=Session.BeginPrint();QVERIFY(Print.HasValue());QVERIFY(Session.IsBusy());
        QVERIFY(Session.CompletePrint(Print.Value(),hc::storage::Unit{}));QVERIFY(Session.Snapshot().dirty_);
        QCOMPARE(Session.Snapshot().savedRevision_,Before.savedRevision_);QVERIFY(Session.Snapshot().savedModel_==Before.savedModel_);
        QVERIFY(!Session.CompletePrint(Print.Value(),hc::storage::Unit{}));
        DocumentSession Denied("denied",{},DocumentContentMode::ExternalModel);QVERIFY(!Denied.BeginPrint().HasValue());
    }
};
QTEST_MAIN(HwpPrintTests)
#include "tstHwpPrint.moc"
