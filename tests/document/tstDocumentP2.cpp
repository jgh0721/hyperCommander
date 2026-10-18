#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <QtTest>
#include "commonLibs/layout/productLayout.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QBuffer>
#include <QToolBar>
#include <QElapsedTimer>
#include <QProcess>
#include <QScreen>
#include <QScrollArea>
#include <QLabel>
#include "image/QImageView.h"
#include <iostream>
#include <Windows.h>
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_annot.h>
#include <fpdf_save.h>
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include "UniqueLibs/document/imageResampler.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "pdf/QPDFView.h"
void InitializeDocumentPdfium();
using namespace hc::document;
namespace {
bool Put(const QString& Path,const QByteArray& Bytes){QFile File(Path);return File.open(QIODevice::WriteOnly)&&File.write(Bytes)==Bytes.size();}
QByteArray Read(const QString& Path){QFile File(Path);return File.open(QIODevice::ReadOnly)?File.readAll():QByteArray{};}
QJsonObject Doc(const DocumentWorkspaceWidget& Workspace,const QString& Id){for(auto Item:Workspace.Snapshot()["documents"].toArray())if(Item.toObject()["documentId"]==Id)return Item.toObject();return {};}
bool Ready(const DocumentWorkspaceWidget& Workspace,const QString& Id){return Doc(Workspace,Id)["lifecycle"]=="ready";}
QByteArray PdfFixture(){
    InitializeDocumentPdfium();auto Document=FPDF_CreateNewDocument();
    for(int At=0;At<2;++At){auto Page=FPDFPage_New(Document,At,300,400);FPDFPage_SetRotation(Page,At?2:0);
        auto Text=FPDFPageObj_NewTextObj(Document,"Helvetica",18);const unsigned short Message[]={'P','2',' ','s','e','a','r','c','h',0};
        FPDFText_SetText(Text,Message);FPDFPageObj_Transform(Text,1,0,0,1,30,250);FPDFPage_InsertObject(Page,Text);FPDFPage_GenerateContent(Page);FPDF_ClosePage(Page);}
    struct Writer:FPDF_FILEWRITE {QByteArray bytes_;static int Write(FPDF_FILEWRITE* Self,const void* Data,unsigned long Size){static_cast<Writer*>(Self)->bytes_.append(static_cast<const char*>(Data),Size);return 1;}} Writer;
    Writer.version=1;Writer.WriteBlock=Writer::Write;FPDF_SaveAsCopy(Document,&Writer,0);FPDF_CloseDocument(Document);return Writer.bytes_;
}
QImage ImageFixture(){QImage Image(24,18,QImage::Format_RGBA8888);for(int Y=0;Y<Image.height();++Y)for(int X=0;X<Image.width();++X)Image.setPixelColor(X,Y,QColor(X*10,Y*14,(X+Y)*6,(X+Y)%5?255:0));return Image;}
QByteArray Png(const QImage& Image){QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);Image.save(&Buffer,"PNG");return Bytes;}
QJsonObject Native(HWND Viewer,HMODULE Module,const QJsonObject& Request){using Call=BOOL(*)(HWND,const char*,UINT,char*,UINT*);auto Fn=reinterpret_cast<Call>(GetProcAddress(Module,"HC_DocumentCommand"));auto Input=QJsonDocument(Request).toJson(QJsonDocument::Compact);QByteArray Output(768*1024,Qt::Uninitialized);UINT Size=Output.size();if(!Fn||!Fn(Viewer,Input.data(),Input.size(),Output.data(),&Size))return {{"error","not-ready"}};Output.resize(Size);return QJsonDocument::fromJson(Output).object();}
}
class DocumentP2Tests final:public QObject {
Q_OBJECT
private slots:
void imageFitTracksViewportAfterHiddenLoad(){
    QImageView View; View.resize(900,650);
    QImage Source(320,200,QImage::Format_RGB32); Source.fill(Qt::blue);
    View.LoadContent(Source,"fit.png",false);
    View.show(); QVERIFY(QTest::qWaitForWindowExposed(&View));
    auto* Scroll=View.findChild<QScrollArea*>("canvasScrollArea"); QVERIFY(Scroll);
    auto* Label=qobject_cast<QLabel*>(Scroll->widget()); QVERIFY(Label);
    const auto Fits=[&]{return Label->pixmap().deviceIndependentSize().toSize()==Source.size().scaled(Scroll->viewport()->size(),Qt::KeepAspectRatio);};
    QTRY_VERIFY_WITH_TIMEOUT(Fits(),10000);
    QTest::qWait(500); // Let the initial asynchronous render settle.
    const auto Outer=View.size();
    Scroll->viewport()->setFixedSize(300,240);
    QTRY_VERIFY_WITH_TIMEOUT(Fits(),3000);
    QCOMPARE(View.size(),Outer);
    Scroll->viewport()->setFixedSize(700,500);
    QTRY_VERIFY_WITH_TIMEOUT(Fits(),3000);
    View.setZoomLevel(50);
    QTRY_COMPARE(Label->pixmap().size(),QSize(160,100));
    Scroll->viewport()->setFixedSize(400,300);
    QTest::qWait(100);
    QCOMPARE(Label->pixmap().size(),QSize(160,100));
}
void initTestCase(){if(qEnvironmentVariableIsSet("HC_DOCUMENT_EXPECT_DPR"))QVERIFY(std::abs(QApplication::primaryScreen()->devicePixelRatio()-qEnvironmentVariable("HC_DOCUMENT_EXPECT_DPR").toDouble())<.02);}
void codecsPreserveScalarBomAndMixedEol(){
    const auto Text=QString("한글 A\u00a0B\u2028C\u2029D\r\nLF\nCR\rEND");
    for(const auto& Encoding:QStringList{"UTF-8","UTF-16LE","UTF-16BE","CP949"}) {
        const auto Value=Encoding=="CP949"?QString("한글\r\nLF\n"):Text;
        for(bool Bom:{false,true}) {
            auto Encoded=EncodeDocumentText(Value,Encoding,Bom);QVERIFY(Encoded.HasValue());
            auto Decoded=DecodeDocumentText(Encoded.Value(),Encoding);QVERIFY(Decoded.HasValue());QCOMPARE(Decoded.Value().text_,Value);
            auto Again=EncodeDocumentText(Decoded.Value().text_,Encoding,Bom);QVERIFY(Again.HasValue());QCOMPARE(Again.Value(),Encoded.Value());
        }
    }
    QVERIFY(!EncodeDocumentText(QString::fromUcs4(U"한글🙂"),"CP949",false).HasValue());
    QVERIFY(!DecodeDocumentText(QByteArray("\xff\xfe\x00",3),"UTF-16LE").HasValue());
    QVERIFY(!DecodeDocumentText(QByteArray("\xc0\xaf",2),"UTF-8").HasValue());
}
void largeTextAndCancellation(){
    QTemporaryDir Root;const auto Path=Root.filePath("large.txt");QByteArray Bytes(2*1024*1024,'x');Bytes.append("\r\n한글");QVERIFY(Put(Path,Bytes));
    auto Input=ReadDocumentFile(Path);QVERIFY(Input.HasValue());QCOMPARE(Input.Value().viewId_,QString("text"));QVERIFY(Input.Value().text_.size()>256*1024);
    std::stop_source Stop;Stop.request_stop();hc::storage::OperationContext Context;Context.stopToken_=Stop.get_token();
    QVERIFY(!ReadDocumentFile(Path,Context).HasValue());QCOMPARE(Read(Path),Bytes);
    DocumentWorkspaceWidget Workspace(Root.filePath("recovery"));auto Id=Workspace.Handle({{"action","open"},{"path",Path}})["documentId"].toString();
    Workspace.Handle({{"action","cancel"},{"documentId",Id}});Workspace.Handle({{"action","close"},{"documentId",Id},{"discard",true}});
    QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);QVERIFY(Workspace.Snapshot()["documents"].toArray().isEmpty());
}
void filterKernelsLinearLightAlphaAndTiles(){
    auto Filters=ImageFilterRegistry::Filters();QCOMPARE(Filters.size(),std::size_t(5));
    QVERIFY(!ImageFilterRegistry::Register(Filters.front()));
    for(const auto& Filter:Filters){QVERIFY(Filter.kernel_(0)>0);QCOMPARE(Filter.kernel_(Filter.support_+1),0.0);}
    QVERIFY(Filters[1].kernel_(1.5)<0);QCOMPARE(ImageFilterRegistry::Resolve("auto",QTransform::fromScale(.5,.25)).id_,QString("ewaRobidoux"));
    QImage BW(2,1,QImage::Format_RGBA8888);BW.setPixelColor(0,0,Qt::black);BW.setPixelColor(1,0,Qt::white);
    ImageRenderRequest Average{BW,QSize(1,1),QTransform::fromScale(.5,1),"bilinear"};
    auto Result=ResampleImage(Average);QVERIFY(Result.error_.isEmpty());auto Grey=Result.image_.pixelColor(0,0).red();QVERIFY(Grey>=187&&Grey<=189);
    BW.setPixelColor(0,0,QColor(255,0,0,0));BW.setPixelColor(1,0,QColor(0,0,255,255));
    Average.source_=BW;Result=ResampleImage(Average);QCOMPARE(Result.image_.pixelColor(0,0).red(),0);QVERIFY(Result.image_.pixelColor(0,0).blue()>250);
    auto Source=ImageFixture();
    for(const auto& Filter:Filters) {
        ImageRenderRequest Request{Source,QSize(17,13),QTransform::fromScale(17./24,13./18),Filter.id_};
        auto Full=ResampleImage(Request);QVERIFY2(Full.error_.isEmpty(),qPrintable(Full.error_));
        Request.tile_=QRect(0,0,8,13);auto Left=ResampleImage(Request);Request.tile_=QRect(8,0,9,13);auto Right=ResampleImage(Request);
        for(int Y=0;Y<13;++Y)for(int X=0;X<17;++X)QCOMPARE(Full.image_.pixelColor(X,Y),X<8?Left.image_.pixelColor(X,Y):Right.image_.pixelColor(X-8,Y));
        QImage Solid(1,7,QImage::Format_RGBA8888);Solid.fill(QColor(45,110,220,128));Request={Solid,QSize(5,3),QTransform::fromScale(5,3./7),Filter.id_};
        auto One=ResampleImage(Request);QVERIFY(One.error_.isEmpty());QVERIFY(std::abs(One.image_.pixelColor(4,2).green()-110)<=1);
    }
    std::stop_source Stop;Stop.request_stop();Average.stop_=Stop.get_token();QCOMPARE(ResampleImage(Average).error_,QString("image-render-cancelled"));
    Average.stop_={};Average.sourceToOutput_=QTransform::fromScale(0,1);QCOMPARE(ResampleImage(Average).error_,QString("image-render-singular-transform"));
}
void mixedTabsEditSaveReopenAndRecovery(){
    QTemporaryDir Root;QVERIFY(Put(Root.filePath("text.txt"),"find find\r\n한글"));
    QVERIFY(Put(Root.filePath("document.pdf"),PdfFixture()));QVERIFY(Put(Root.filePath("image.png"),Png(ImageFixture())));
    DocumentWorkspaceWidget Workspace(Root.filePath("recovery"));Workspace.show();
    QStringList Ids;
    for(auto Name:{"text.txt","document.pdf","image.png"})Ids<<Workspace.Handle({{"action","open"},{"path",Root.filePath(Name)}})["documentId"].toString();
    QTRY_VERIFY(Ready(Workspace,Ids[0])&&Ready(Workspace,Ids[1])&&Ready(Workspace,Ids[2]));QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);
    QCOMPARE(Doc(Workspace,Ids[0])["view"].toObject()["viewId"].toString(),QString("text"));
    QCOMPARE(Doc(Workspace,Ids[1])["view"].toObject()["viewId"].toString(),QString("pdf"));
    QCOMPARE(Doc(Workspace,Ids[2])["view"].toObject()["viewId"].toString(),QString("image"));
    Workspace.Handle({{"action","text.replaceAll"},{"documentId",Ids[0]},{"text","find"},{"replacement","found"}});
    QVERIFY(Doc(Workspace,Ids[0])["dirty"].toBool());
    Workspace.Handle({{"action","pdf.search"},{"documentId",Ids[1]},{"text","search"}});
    QVERIFY(Doc(Workspace,Ids[1])["view"].toObject()["searchCount"].toInt()>0);
    Workspace.Handle({{"action","pdf.rotate"},{"documentId",Ids[1]},{"angle",90}});
    for(const auto& Kind:QStringList{"ink","highlight","underline","strikeOut","freeText","note"}) {
        auto Reply=Workspace.Handle({{"action","pdf.annotate"},{"documentId",Ids[1]},{"page",0},{"kind",Kind},{"x",35},{"y",80},{"width",80},{"height",35},{"text","P2 한글 주석"}});
        QVERIFY2(!Reply.contains("error"),qPrintable(Kind));
    }
    Workspace.Handle({{"action","image.filter"},{"documentId",Ids[2]},{"filter","lanczos3"}});QVERIFY(!Doc(Workspace,Ids[2])["dirty"].toBool());
    Workspace.Handle({{"action","image.crop"},{"documentId",Ids[2]},{"x",1},{"y",2},{"width",20},{"height",12}});QVERIFY(Doc(Workspace,Ids[2])["dirty"].toBool());
    Workspace.Handle({{"action","undo"},{"documentId",Ids[2]}});QVERIFY(!Doc(Workspace,Ids[2])["dirty"].toBool());
    Workspace.Handle({{"action","redo"},{"documentId",Ids[2]}});QCOMPARE(Doc(Workspace,Ids[2])["view"].toObject()["width"].toInt(),20);
    QImage Overlay(3,3,QImage::Format_RGBA8888);Overlay.fill(QColor(255,0,0,120));
    QVERIFY(!Workspace.Handle({{"action","image.composite"},{"documentId",Ids[2]},{"bytes",QString::fromLatin1(Png(Overlay).toBase64())},{"x",2},{"y",2},{"width",9},{"height",5},{"angle",31}}).contains("error"));
    QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);Workspace.Checkpoint();QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);
    DocumentRecoveryStore Recovery(Root.filePath("recovery"));auto Records=Recovery.List();QVERIFY(Records.HasValue());QCOMPARE(Records.Value().size(),std::size_t(3));
    auto ImageRecord=std::find_if(Records.Value().begin(),Records.Value().end(),[](const auto& Record){return Record.viewId_=="image";});QVERIFY(ImageRecord!=Records.Value().end());
    const auto Restored=Workspace.Handle({{"action","recover"},{"path",QString::fromStdString(ImageRecord->snapshotPath_)}})["documentId"].toString();
    QTRY_VERIFY(Ready(Workspace,Restored));QCOMPARE(Doc(Workspace,Restored)["view"].toObject()["viewId"].toString(),QString("image"));QVERIFY(!Doc(Workspace,Restored)["canSave"].toBool());
    for(auto Id:Ids)Workspace.Handle({{"action","save"},{"documentId",Id}});
    QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);
    for(auto Id:Ids){QVERIFY(!Doc(Workspace,Id)["dirty"].toBool());QCOMPARE(Doc(Workspace,Id)["error"].toString(),QString{});}
    QCOMPARE(Read(Root.filePath("text.txt")),QString("found found\r\n한글").toUtf8());
    auto Pdf=Read(Root.filePath("document.pdf"));auto Document=FPDF_LoadMemDocument64(Pdf.constData(),Pdf.size(),nullptr);QVERIFY(Document);
    auto Page=FPDF_LoadPage(Document,0);QCOMPARE(FPDFPage_GetRotation(Page),1);QCOMPARE(FPDFPage_GetAnnotCount(Page),6);FPDF_ClosePage(Page);
    Page=FPDF_LoadPage(Document,1);QCOMPARE(FPDFPage_GetRotation(Page),2);FPDF_ClosePage(Page);FPDF_CloseDocument(Document);
    QImage Reopened(Root.filePath("image.png"));QCOMPARE(Reopened.size(),QSize(20,12));
    for(auto Id:Ids)Workspace.Handle({{"action","reload"},{"documentId",Id}});
    QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);for(auto Id:Ids)QVERIFY(Ready(Workspace,Id));
}
void textRegexAndCancelledOutputPreserveSource(){
    QTemporaryDir Root;const auto Path=Root.filePath("regex.txt");QVERIFY(Put(Path,"a1 a2\r\nline"));
    DocumentWorkspaceWidget Workspace(Root.filePath("recovery"));auto Id=Workspace.Handle({{"action","open"},{"path",Path}})["documentId"].toString();
    QTRY_VERIFY(Ready(Workspace,Id));
    Workspace.Handle({{"action","text.replaceAll"},{"documentId",Id},{"text","a([0-9])"},{"replacement","b\\1"},{"regex",true}});
    Workspace.Handle({{"action","save"},{"documentId",Id}});QTRY_VERIFY(!Doc(Workspace,Id)["saving"].toBool());QCOMPARE(Read(Path),QByteArray("b1 b2\r\nline"));
    Workspace.Handle({{"action","text.replaceAll"},{"documentId",Id},{"text","^"},{"replacement",">"},{"regex",true}});
    QVERIFY(Doc(Workspace,Id)["dirty"].toBool());Workspace.Handle({{"action","undo"},{"documentId",Id}});QVERIFY(!Doc(Workspace,Id)["dirty"].toBool());
    const auto ImagePath=Root.filePath("cancel.png");const auto Original=Png(ImageFixture());QVERIFY(Put(ImagePath,Original));
    auto ImageId=Workspace.Handle({{"action","open"},{"path",ImagePath}})["documentId"].toString();QTRY_VERIFY(Ready(Workspace,ImageId));QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);
    Workspace.Handle({{"action","image.resize"},{"documentId",ImageId},{"width",4096},{"height",2048},{"filter","lanczos3"}});
    Workspace.Handle({{"action","cancel"},{"documentId",ImageId}});QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),20000);
    QCOMPARE(Doc(Workspace,ImageId)["view"].toObject()["width"].toInt(),24);QVERIFY(!Doc(Workspace,ImageId)["dirty"].toBool());QCOMPARE(Read(ImagePath),Original);
}
void renderCorpusMeasurements(){
    QImage Source(256,192,QImage::Format_RGBA8888);
    for(int Y=0;Y<192;++Y)for(int X=0;X<256;++X)Source.setPixelColor(X,Y,QColor((X*17+Y)%256,(Y*13+X)%256,(X^Y)%256,(X+Y)%7?255:0));
    for(const auto& Filter:ImageFilterRegistry::Filters()){
        QList<qint64> Times;QImage Last;
        for(int Repeat=0;Repeat<5;++Repeat){ImageRenderRequest Request{Source,QSize(512,384),QTransform::fromScale(2,2),Filter.id_};QElapsedTimer Timer;Timer.start();auto Result=ResampleImage(Request);Times<<Timer.elapsed();QVERIFY(Result.error_.isEmpty());Last=Result.image_;}
        std::sort(Times.begin(),Times.end());qInfo().noquote()<<QString("P2_RENDER_METRIC filter=%1 source=256x192 output=512x384 n=5 p95_ms=%2 dpr=%3").arg(Filter.id_).arg(Times.back()).arg(QApplication::primaryScreen()->devicePixelRatio());QVERIFY(!Last.isNull());
    }
}
void nativeDllMixedTabsAndCopyOnlySave(){
    QTemporaryDir Root;QVERIFY(Put(Root.filePath("text.txt"),"native"));
    QVERIFY(Put(Root.filePath("document.pdf"),PdfFixture()));QVERIFY(ImageFixture().save(Root.filePath("image.jpg"),"JPEG"));
    QWidget Parent;Parent.resize(900,600);Parent.show();
    auto Dll=QCoreApplication::applicationDirPath()+'/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
    auto Module=LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);QVERIFY(Module);
    using Create=HWND(*)(HWND,LPRECT,DWORD);RECT Rect{0,0,900,600};auto Viewer=reinterpret_cast<Create>(GetProcAddress(Module,"DVP_CreateViewer"))(reinterpret_cast<HWND>(Parent.winId()),&Rect,0);QVERIFY(Viewer);
    auto Cleanup=qScopeGuard([&]{DestroyWindow(Viewer);FreeLibrary(Module);});
    QTRY_VERIFY(Native(Viewer,Module,{{"action","state"}}).contains("documents"));
    QStringList Ids;Ids<<Native(Viewer,Module,{{"action","openHost"},{"path",Root.filePath("document.pdf")}})["documentId"].toString();
    for(auto Name:{"text.txt","image.jpg"})Ids<<Native(Viewer,Module,{{"action","open"},{"path",Root.filePath(Name)}})["documentId"].toString();
    QTRY_VERIFY(Native(Viewer,Module,{{"action","state"}})["idle"].toBool());
    auto State=Native(Viewer,Module,{{"action","state"}});QCOMPARE(State["documents"].toArray().size(),3);
    QVERIFY(!State["documents"].toArray()[0].toObject()["canSave"].toBool());
    QVERIFY(!Native(Viewer,Module,{{"action","pdf.rotate"},{"documentId",Ids[0]},{"angle",90}}).contains("error"));
    const auto Output=Root.filePath("native-saved.pdf");
    Native(Viewer,Module,{{"action","saveAs"},{"documentId",Ids[0]},{"path",Output}});
    QTRY_VERIFY(QFile::exists(Output));QTRY_VERIFY(Native(Viewer,Module,{{"action","state"}})["idle"].toBool());
    auto Bytes=Read(Output);auto Document=FPDF_LoadMemDocument64(Bytes.constData(),Bytes.size(),nullptr);QVERIFY(Document);
    auto Page=FPDF_LoadPage(Document,1);QCOMPARE(FPDFPage_GetRotation(Page),2);FPDF_ClosePage(Page);FPDF_CloseDocument(Document);
}
};
int main(int Argc,char** Argv){

    QApplication App(Argc,Argv);
    if(App.arguments().contains("--probe-dpr")){std::cout<<App.primaryScreen()->devicePixelRatio()<<std::endl;return 0;}
    if(App.arguments().contains("--export-fixtures")){
        const auto Directory=App.arguments().last();QDir().mkpath(Directory);
        return Put(Directory+"/basic.pdf",PdfFixture())&&Put(Directory+"/alpha.png",Png(ImageFixture()))&&ImageFixture().save(Directory+"/photo.jpg","JPEG")&&Put(Directory+"/sample.cpp","// P2 mixed workspace\r\nint main() { return 0; }\r\n")?0:1;
    }
    QTemporaryDir Config;App.setProperty("documentViewsConfigDir",Config.path());DocumentP2Tests Tests;return QTest::qExec(&Tests,Argc,Argv);

}
#include "tstDocumentP2.moc"
