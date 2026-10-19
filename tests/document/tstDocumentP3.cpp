#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "commonLibs/layout/productLayout.hpp"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QProcess>
#include <QClipboard>
#include <QScreen>
#include <QLabel>
#include <QLineEdit>
#include <QShortcut>
#include <QStackedLayout>
#include <QScrollArea>
#include <QDialog>
#include <QCheckBox>
#include <QMessageBox>
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include <QVBoxLayout>
#include <iostream>
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <atomic>
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentHostBroker.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "UniqueLibs/qtPluginViewerSurface.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "pluginHost/dopusAbi.hpp"
using namespace hc::document;
using namespace hc::storage;
using namespace hc::plugins;
namespace {
bool Put(const QString& Path,const QByteArray& Bytes){QFile File(Path);return File.open(QIODevice::WriteOnly)&&File.write(Bytes)==Bytes.size();}
QByteArray Read(const QString& Path){QFile File(Path);return File.open(QIODevice::ReadOnly)?File.readAll():QByteArray{};}
bool Wait(std::function<bool()> Predicate,int Timeout=15000){QElapsedTimer Timer;Timer.start();do{if(Predicate())return true;QTest::qWait(20);}while(Timer.elapsed()<Timeout);return false;}
QJsonObject Doc(QJsonObject State,QString Id){for(const auto& Item:State["documents"].toArray())if(Item.toObject()["documentId"].toString()==Id)return Item.toObject();return {};}
QString Fixture(QString Name){return QString(HC_DOCUMENT_FIXTURES)+"/"+Name;}
class Native final {
public:
    QWidget parent_; HMODULE module_=nullptr; HWND viewer_=nullptr;
    Native(){parent_.resize(1000,700);parent_.show();const auto Dll=QCoreApplication::applicationDirPath()+'/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
        module_=LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(module_){using Create=HWND(*)(HWND,LPRECT,DWORD);RECT Rect{0,0,1000,700};auto Fn=reinterpret_cast<Create>(GetProcAddress(module_,"DVP_CreateViewer"));if(Fn)viewer_=Fn(reinterpret_cast<HWND>(parent_.winId()),&Rect,0);}}
    ~Native(){if(viewer_)DestroyWindow(viewer_);if(module_)FreeLibrary(module_);}
    QJsonObject Call(QJsonObject Request){using Fn=BOOL(*)(HWND,const char*,UINT,char*,UINT*);auto Invoke=module_?reinterpret_cast<Fn>(GetProcAddress(module_,"HC_DocumentCommand")):nullptr;
        const auto Input=QJsonDocument(Request).toJson(QJsonDocument::Compact);QByteArray Output(HostPayloadLimit,Qt::Uninitialized);UINT Size=Output.size();
        if(!Invoke||!viewer_||!Invoke(viewer_,Input.constData(),Input.size(),Output.data(),&Size))return {{"error","not-ready"}};
        Output.resize(Size);return QJsonDocument::fromJson(Output).object();}
};
class Source final:public IContentSource{
public:
    std::shared_ptr<IStorageProvider> provider_; EntrySnapshot entry_;
    EntrySnapshot Describe()const override{return entry_;}
    Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context)override{return provider_->OpenRead(entry_.id_,Context);}
    Task<Unit> Verify(OperationContext Context)override{auto At=provider_->Stat(entry_.id_,Context).get();return Ready<Unit>(At.HasValue()?Result<Unit>{Unit{}}:Result<Unit>{At.Failure()});}
};
std::shared_ptr<Source> Content(QString Path){auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto At=Reader->Resolve({LocatorKind::Local,Path.toStdString()},{}).get();if(!At.HasValue())return {};auto Result=std::make_shared<Source>();Result->provider_=Reader;Result->entry_=At.Value();return Result;}
class Lease final:public IMaterializedFileLease{
public:
    EntrySnapshot entry_;std::shared_ptr<std::atomic<int>> live_;QString local_;
    Lease(EntrySnapshot Entry,std::shared_ptr<std::atomic<int>> Live,QString Local={}):entry_(Entry),live_(Live),local_(std::move(Local)){++*live_;}
    ~Lease(){--*live_;}
    std::string LocalPath()const override{return local_.isEmpty()?entry_.locator_.root_:local_.toStdString();}
    EntrySnapshot Describe()const override{return entry_;}
    std::size_t Depth()const override{return 0;}
};
class Materializer final:public IMaterializer{
public:
    std::shared_ptr<std::atomic<int>> live_;QString copyRoot_;
    Task<std::shared_ptr<IMaterializedFileLease>> Materialize(MaterializationRequest Request,OperationContext Context)override{
        if(auto Stop=Context.Check({"test"}))return Ready<std::shared_ptr<IMaterializedFileLease>>(*Stop);
        QString Local;if(!copyRoot_.isEmpty()){const auto Entry=Request.source_->Describe();Local=copyRoot_+'/'+QFileInfo(QString::fromStdString(Entry.name_)).fileName();if(!QFile::exists(Local)&&!QFile::copy(QString::fromStdString(Entry.locator_.root_),Local))return Ready<std::shared_ptr<IMaterializedFileLease>>(Error{ErrorCategory::Unavailable,"test-materialize-copy"});}
        return Ready<std::shared_ptr<IMaterializedFileLease>>(std::shared_ptr<IMaterializedFileLease>(std::make_shared<Lease>(Request.source_->Describe(),live_,Local)));}
    MaterializationCleanup Recover()override{return {};}
    MaterializationCleanup DrainCleanup()override{return {};}
};
PluginViewerOptions Options(){PluginViewerOptions Result;Result.executable_=QCoreApplication::applicationDirPath()+"/hc-plugin-host-x64.exe";
    Result.manifestPath_=QCoreApplication::applicationDirPath()+'/' + hc::layout::HyperViewerPackage + "/hyperviewer.json";auto Inspected=InspectManifestFile(Result.manifestPath_);
    if(Inspected.HasValue()){Result.binaryPath_=Inspected.Value().binaryPath_;const auto Manifest=Inspected.Value().manifest_;Result.approval_={Manifest,true,false,Manifest.permissions_};}return Result;}
}
class DocumentP3Tests final:public QObject{
    Q_OBJECT
private slots:
void viewerEscapeSettingAndUnsavedChanges(){
    QTemporaryDir Root; const auto Previous=qApp->property("documentViewsConfigDir");
    qApp->setProperty("documentViewsConfigDir",Root.path());
    const auto Restore=qScopeGuard([&]{qApp->setProperty("documentViewsConfigDir",Previous);});
    DocumentSettingsDialog Settings(hc::settings::UnifiedConfig::FileName(Root.path()),{});
    auto* Option=Settings.findChild<QCheckBox*>("documentViews/common/closeViewerOnEscape");
    QVERIFY(Option); QVERIFY(Option->isChecked());
    Option->setChecked(false); QVERIFY(Settings.Apply());
    DocumentWorkspaceWidget Viewer(Root.filePath("viewer"),true);
    QVERIFY(!Viewer.Snapshot()["closeViewerOnEscape"].toBool());
    QVERIFY(!Viewer.Handle({{"action","escape"}})["accepted"].toBool());
    QVERIFY(!Viewer.Snapshot()["closeRequested"].toBool());
    Option->setChecked(true); QVERIFY(Settings.Apply());
    const auto Path=Root.filePath("dirty.txt"); QVERIFY(Put(Path,"original"));
    const auto Id=Viewer.Handle({{"action","open"},{"path",Path}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Id)["lifecycle"]=="ready";}));
    Viewer.Handle({{"action","replaceText"},{"documentId",Id},{"text","changed"}});
    Viewer.SetClosePrompt([](const QString&){return int(QMessageBox::Cancel);});
    Viewer.Handle({{"action","escape"}});
    QVERIFY(!Viewer.Snapshot()["closeRequested"].toBool());
    QVERIFY(Doc(Viewer.Snapshot(),Id)["dirty"].toBool());
    Viewer.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
    Viewer.show(); Viewer.activateWindow(); Viewer.setFocus();
    QVERIFY(QTest::qWaitForWindowActive(&Viewer));
    QTest::keyClick(&Viewer,Qt::Key_Escape);
    QTRY_VERIFY(Viewer.Snapshot()["closeRequested"].toBool());
    QCOMPARE(Read(Path),QByteArray("original"));
    Viewer.hide();
    DocumentWorkspaceWidget Editor(Root.filePath("editor"),false);
    Editor.show(); Editor.activateWindow(); Editor.setFocus();
    QVERIFY(QTest::qWaitForWindowActive(&Editor));
    QTest::keyClick(&Editor,Qt::Key_Escape);
    QVERIFY(Editor.isVisible()); QVERIFY(!Editor.Snapshot()["closeRequested"].toBool());
    QVERIFY(!Editor.Handle({{"action","escape"}})["accepted"].toBool());
}
void largeImageEscapesFromImageViewport(){
    QTemporaryDir Root;
    auto Path = QStringLiteral("D:/Pictures/20260223_154916.jpg");
    if (!QFileInfo::exists(Path)) {
        Path = Root.filePath("large.jpg");
        QImage Image(4100, 4100, QImage::Format_RGB32); QVERIFY(!Image.isNull());
        Image.fill(Qt::blue); QVERIFY(Image.save(Path, "JPEG"));
    }
    {
        const auto Input = ReadDocumentFile(Path);
        QVERIFY2(Input.HasValue(), Input.HasValue() ? "" : Input.Failure().code_.c_str());
        QCOMPARE(Input.Value().viewId_, QString("image"));
        QVERIFY(qint64(Input.Value().image_.width()) * Input.Value().image_.height() > 16 * 1024 * 1024);
    }
    DocumentWorkspaceWidget Viewer(Root.filePath("viewer"), true);
    Viewer.show(); Viewer.activateWindow();
    const auto Id = Viewer.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
    QVERIFY(!Id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(Doc(Viewer.Snapshot(), Id)["lifecycle"] == "ready", 30000);
    auto* Scroll = Viewer.findChild<QScrollArea*>("canvasScrollArea"); QVERIFY(Scroll);
    Scroll->viewport()->setFocus();
    QTest::keyClick(Scroll->viewport(), Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(Viewer.Snapshot()["closeRequested"].toBool(), 30000);
}
void hostedPreferencesWindowLifecycle(){
    QTemporaryDir Root; QVERIFY(Put(Root.filePath("settings-test.txt"), "test"));
    Native Dll; QVERIFY(Dll.viewer_);
    QVERIFY(Wait([&]{return Dll.Call({{"action","state"}}).contains("documents");}));
    QVERIFY(!Dll.Call({{"action","open"},{"path",Root.filePath("settings-test.txt")}}).contains("error"));
    QVERIFY(Wait([&]{return Dll.Call({{"action","state"}})["idle"].toBool();}));
    const auto Pid=GetCurrentProcessId();
    struct Search { DWORD pid_; HWND dialog_=nullptr; };
    auto Find=[&]{Search State{Pid};EnumWindows([](HWND Window,LPARAM Param)->BOOL{
        auto* State=reinterpret_cast<Search*>(Param);DWORD Owner=0;GetWindowThreadProcessId(Window,&Owner);
        wchar_t Title[64]{};GetWindowTextW(Window,Title,64);
        if(Owner==State->pid_&&QString::fromWCharArray(Title)==QString::fromUtf8("설정")&&IsWindowVisible(Window)){State->dialog_=Window;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&State));return State.dialog_;};
    for(int Pass=0;Pass<2;++Pass){
        QVERIFY(Dll.Call({{"action","preferences"}})["accepted"].toBool());
        HWND Dialog=nullptr;QVERIFY(Wait([&]{Dialog=Find();return Dialog!=nullptr;}));
        RECT Bounds{};QVERIFY(GetWindowRect(Dialog,&Bounds));QVERIFY(Bounds.right>Bounds.left&&Bounds.bottom>Bounds.top);
        QVERIFY(MonitorFromRect(&Bounds,MONITOR_DEFAULTTONULL));QVERIFY(IsWindowEnabled(Dialog));QVERIFY(IsWindowEnabled(Dll.viewer_));
        QVERIFY(Dll.Call({{"action","preferences"}})["accepted"].toBool());QCOMPARE(Find(),Dialog);
        const auto Foreground=GetForegroundWindow();
        QVERIFY(!Dll.Call({{"action","focus"}})["accepted"].toBool());
        QCOMPARE(GetForegroundWindow(),Foreground);
        QVERIFY(PostMessageW(Dialog,WM_CLOSE,0,0));QVERIFY(Wait([&]{return !IsWindow(Dialog);}));
        QVERIFY(Dll.Call({{"action","state"}})["idle"].toBool());
    }
}
void protocolNegotiationAndFences(){
    QTemporaryDir Root;DocumentWorkspaceWidget Workspace(Root.path());quint64 Sequence=0;QString WorkspaceId;
    auto Send=[&](QString Type,QJsonObject Payload=QJsonObject{},QString Id=QString{},QString Generation=QString{}){
        QJsonObject Request{{"protocolVersion",int(HostVersion)},{"requestId",QString::number(++Sequence)},{"workspaceId",WorkspaceId},{"documentId",Id},{"generation",Generation},
            {"messageType",Type},{"payloadLength",QJsonDocument(Payload).toJson(QJsonDocument::Compact).size()},{"payload",Payload}};
        return Workspace.HandleMessage(Request);};
    QCOMPARE(Send("DocumentState")["messageType"].toString(),QString("Error"));
    auto Hello=Send("Hello");QCOMPARE(Hello["messageType"].toString(),QString("Ready"));WorkspaceId=Hello["workspaceId"].toString();QCOMPARE(WorkspaceId.size(),32);
    QCOMPARE(Send("Hello")["messageType"].toString(),QString("Error"));QCOMPARE(Send("Unknown")["payload"].toObject()["code"].toString(),QString("document-message-type"));
    auto Heart=Send("Heartbeat");QVERIFY(Heart["payload"].toObject()["alive"].toBool());
    auto Wrong=QJsonObject{{"protocolVersion",99},{"requestId",QString::number(++Sequence)},{"messageType","Hello"},{"payload",QJsonObject{}},{"payloadLength",2}};
    QCOMPARE(Workspace.HandleMessage(Wrong)["payload"].toObject()["code"].toString(),QString("document-protocol-version"));
    Wrong["protocolVersion"]=int(HostVersion);Wrong["messageType"]="DocumentState";Wrong["workspaceId"]="forged";
    QCOMPARE(Workspace.HandleMessage(Wrong)["payload"].toObject()["code"].toString(),QString("document-workspace-token"));
    Wrong["workspaceId"]=WorkspaceId;Wrong["payloadLength"]=3;QCOMPARE(Workspace.HandleMessage(Wrong)["messageType"].toString(),QString("Error"));
    QVERIFY(Put(Root.filePath("source.txt"),"old"));auto Id=Workspace.Handle({{"action","open"},{"path",Root.filePath("source.txt")}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Workspace.Snapshot(),Id)["lifecycle"].toString()=="ready";}));
    QCOMPARE(Send("Execute",{{"action","replaceText"},{"text","bad"}},Id,"999")["messageType"].toString(),QString("Error"));
    QVERIFY(!Doc(Workspace.Snapshot(),Id)["dirty"].toBool());
}
void immutableLargeSaveAndLeaseRetirement(){
    QTemporaryDir Root;const auto Path=Root.filePath("large.txt");QByteArray Original(2*1024*1024,'x');Original.replace(0,3,"old");QVERIFY(Put(Path,Original));
    auto Input=ReadDocumentFile(Path);QVERIFY(Input.HasValue());auto Live=std::make_shared<std::atomic<int>>(0);
    auto Pin=std::make_shared<Lease>(Input.Value().source_,Live);DocumentHostBroker Broker;const auto Token=Broker.Bind(Input.Value().source_,MakeNativeWritableStorage(LocatorKind::Local,MakeNativeReadProvider(LocatorKind::Local)),Pin,Input.Value().hash_);Pin.reset();QCOMPARE(Live->load(),1);
    DocumentWorkspaceWidget Workspace(Root.filePath("recovery"));auto Id=Workspace.Handle({{"action","open"},{"path",Path},{"sourceToken",Token},{"broker",true}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Workspace.Snapshot(),Id)["lifecycle"].toString()=="ready";}));
    QVERIFY(!Workspace.Handle({{"action","text.replaceAll"},{"documentId",Id},{"text","old"},{"replacement","new"}}).contains("error"));
    auto Prepared=Workspace.Handle({{"action","prepareSave"},{"documentId",Id}});QVERIFY(!Prepared.contains("bytes"));QCOMPARE(Prepared["byteLength"].toString(),QString::number(Original.size()));
    QVERIFY(!Workspace.Handle({{"action","text.replaceAll"},{"documentId",Id},{"text","new"},{"replacement","later"}}).contains("error"));
    QVERIFY(Workspace.Handle({{"action","saveRead"},{"documentId",Id},{"saveToken","stale"},{"offset","0"},{"count",10}}).contains("error"));
    QByteArray Bytes;while(Bytes.size()<Original.size()){auto Chunk=Workspace.Handle({{"action","saveRead"},{"documentId",Id},{"saveToken",Prepared["saveToken"]},{"offset",QString::number(Bytes.size())},{"count",int(HostChunkLimit)}});QVERIFY(!Chunk.contains("error"));Bytes+=QByteArray::fromBase64(Chunk["bytes"].toString().toLatin1());}
    auto Expected=Original;Expected.replace(0,3,"new");QCOMPARE(Bytes,Expected);
    auto Wrong=Prepared;Wrong["sha256"]="bad";QVERIFY(!Broker.CommitBytes(Wrong,Bytes)["committed"].toBool());
    auto Committed=Broker.CommitBytes(Prepared,Bytes);QVERIFY2(Committed["committed"].toBool(),qPrintable(Committed["error"].toString()));
    QVERIFY(!Workspace.Handle(Committed).contains("error"));QVERIFY(Doc(Workspace.Snapshot(),Id)["dirty"].toBool());QCOMPARE(Read(Path),Expected);
    QVERIFY(!Broker.CommitBytes(Prepared,Bytes)["committed"].toBool());Broker.Retain({});QCOMPARE(Live->load(),0);QVERIFY(!Broker.CommitBytes(Prepared,Bytes)["committed"].toBool());
}
void mixedExeAndDllEquivalence(){
    QTemporaryDir Root;DocumentWorkspaceWidget Exe(Root.filePath("recovery"));Native Dll;QVERIFY(Dll.viewer_);QVERIFY(Wait([&]{return Dll.Call({{"action","state"}}).contains("documents");}));
    for(int Mode=0;Mode<2;++Mode){
        QStringList Ids;for(const auto& Name:QStringList{"sample.cpp","basic.pdf","alpha.png"}){
            const auto Path=Root.filePath(QString::number(Mode)+"-"+Name);QVERIFY(QFile::copy(Fixture(Name),Path));
            auto Call=[&](QJsonObject Request){return Mode?Dll.Call(Request):Exe.Handle(Request);};
            auto Id=Call({{"action","open"},{"path",Path}})["documentId"].toString();QVERIFY(!Id.isEmpty());Ids<<Id;
        }
        auto Call=[&](QJsonObject Request){return Mode?Dll.Call(Request):Exe.Handle(Request);};
        QVERIFY(Wait([&]{return Call({{"action","state"}})["idle"].toBool();}));
        QVERIFY(!Call({{"action","text.find"},{"documentId",Ids[0]},{"text","main"}}).contains("error"));
        QVERIFY(!Call({{"action","replaceText"},{"documentId",Ids[0]},{"text","P3 edited\r\n"}}).contains("error"));
        QVERIFY(!Call({{"action","pdf.search"},{"documentId",Ids[1]},{"text","search"}}).contains("error"));
        QVERIFY(!Call({{"action","pdf.rotate"},{"documentId",Ids[1]},{"angle",90}}).contains("error"));
        QVERIFY(!Call({{"action","image.crop"},{"documentId",Ids[2]},{"x",1},{"y",1},{"width",12},{"height",8}}).contains("error"));
        for(int Index=0;Index<3;++Index){
            auto Id=Ids[Index];auto Printed=Root.filePath(QString("print-%1-%2.pdf").arg(Mode).arg(Index));
            QVERIFY(!Call({{"action","printToPdf"},{"documentId",Id},{"path",Printed}}).contains("error"));QVERIFY(Read(Printed).startsWith("%PDF-"));
            Call({{"action","save"},{"documentId",Id}});
        }
        QVERIFY(Wait([&]{return Call({{"action","state"}})["idle"].toBool();}));
        auto State=Call({{"action","state"}});for(auto Id:Ids)QVERIFY(!Doc(State,Id)["dirty"].toBool());
        QImage Image(Root.filePath(QString::number(Mode)+"-alpha.png"));QCOMPARE(Image.size(),QSize(12,8));
    }
    QCOMPARE(Read(Root.filePath("0-sample.cpp")),Read(Root.filePath("1-sample.cpp")));
    for(int Mode=0;Mode<2;++Mode){
        const auto Bytes=Read(Root.filePath(QString::number(Mode)+"-basic.pdf"));auto Document=FPDF_LoadMemDocument64(Bytes.constData(),Bytes.size(),nullptr);QVERIFY(Document);QCOMPARE(FPDF_GetPageCount(Document),2);
        auto Page=FPDF_LoadPage(Document,0);QCOMPARE(FPDFPage_GetRotation(Page),1);auto Text=FPDFText_LoadPage(Page);QVERIFY(FPDFText_CountChars(Text)>0);FPDFText_ClosePage(Text);FPDF_ClosePage(Page);
        Page=FPDF_LoadPage(Document,1);QCOMPARE(FPDFPage_GetRotation(Page),2);FPDF_ClosePage(Page);FPDF_CloseDocument(Document);
    }
    QCOMPARE(Read(Root.filePath("0-alpha.png")),Read(Root.filePath("1-alpha.png")));
}
void externalEditorOpensMultipleWindows(){
    QTemporaryDir Root;
    const auto First=Root.filePath("first.txt"),Second=Root.filePath("second.txt");
    QVERIFY(Put(First,"first"));QVERIFY(Put(Second,"second"));
    QProcess Editor;
    Editor.start(QCoreApplication::applicationDirPath()+"/HyperEditor.exe",
        {"--config-dir",Root.path(),"--",First,Second});
    QVERIFY(Editor.waitForStarted(10000));
    const auto Pid=DWORD(Editor.processId());QVERIFY(Pid&&Pid!=GetCurrentProcessId());
    QVERIFY(Wait([&]{
        struct Finder{DWORD pid_;int count_=0;};Finder State{Pid};
        EnumWindows([](HWND Window,LPARAM Context)->BOOL{
            auto* Search=reinterpret_cast<Finder*>(Context);DWORD Owner=0;GetWindowThreadProcessId(Window,&Owner);
            wchar_t Title[256]{};GetWindowTextW(Window,Title,256);
            if(Owner==Search->pid_&&IsWindowVisible(Window)&&QString::fromWCharArray(Title)=="HyperEditor")
                ++Search->count_;
            return TRUE;
        },reinterpret_cast<LPARAM>(&State));return State.count_==2;
    },15000));
    Editor.terminate();
    if(!Editor.waitForFinished(5000)){Editor.kill();QVERIFY(Editor.waitForFinished(5000));}
    QCOMPARE(Read(First),QByteArray("first"));QCOMPARE(Read(Second),QByteArray("second"));
}
void busySdkLoadKeepsLatestSlotAndInternalTabs(){
    QTemporaryDir Root;QVERIFY(Put(Root.filePath("one.txt"),QByteArray(1024*1024,'x')));QVERIFY(Put(Root.filePath("two.txt"),"two"));QVERIFY(Put(Root.filePath("three.txt"),"three"));QVERIFY(Put(Root.filePath("internal.txt"),"internal"));
    DocumentWorkspaceWidget Workspace(Root.filePath("recovery"));
    const auto Internal=Workspace.Handle({{"action","open"},{"path",Root.filePath("internal.txt")}})["documentId"].toString();
    const auto First=Workspace.Handle({{"action","openHost"},{"path",Root.filePath("one.txt")}})["documentId"].toString();
    QVERIFY(!Workspace.Handle({{"action","openHost"},{"path",Root.filePath("two.txt")}}).contains("error"));
    QVERIFY(!Workspace.Handle({{"action","openHost"},{"path",Root.filePath("three.txt")}}).contains("error"));
    QVERIFY(Wait([&]{auto State=Workspace.Snapshot();return State["idle"].toBool()&&Doc(State,First).isEmpty()&&State["documents"].toArray().size()==2;}));
    QVERIFY(!Doc(Workspace.Snapshot(),Internal).isEmpty());
    bool Latest=false;for(const auto& Item:Workspace.Snapshot()["documents"].toArray())if(Item.toObject()["hostSlot"].toBool())Latest=Item.toObject()["path"].toString()==Root.filePath("three.txt");QVERIFY(Latest);
}
void isolatedF3WindowOwnership(){
    QTemporaryDir Root;const auto Path=Root.filePath("tiny.txt");QVERIFY(Put(Path,QByteArray(369,'x')));
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtPluginViewerController Controller(nullptr,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
    QtPluginViewerSurface Surface(&Controller);Surface.Open(Options(),Content(Path));
    QVERIFY(Wait([&]{return Surface.IsReady()&&Surface.Details().documentState_["idle"].toBool();},30000));
    const auto Sequence=Surface.SendDocument({{"action","present"}});QVERIFY(Sequence.HasValue());
    QVERIFY(Wait([&]{return Surface.Details().commandSequence_>=Sequence.Value();}));
    QVERIFY(!Surface.Details().commandError_);
    const auto Pid=Surface.Details().host_.processId_;QVERIFY(Pid&&Pid!=GetCurrentProcessId());
    HWND Viewer=nullptr;
    QVERIFY(Wait([&]{
        struct Finder{DWORD pid_;HWND window_=nullptr;};Finder State{Pid};
        EnumWindows([](HWND Window,LPARAM Context)->BOOL{
            auto* Search=reinterpret_cast<Finder*>(Context);DWORD Owner=0;GetWindowThreadProcessId(Window,&Owner);
            wchar_t Title[256]{};GetWindowTextW(Window,Title,256);
            if(Owner==Search->pid_&&IsWindowVisible(Window)&&QString::fromWCharArray(Title).startsWith("HyperViewer"))
                {Search->window_=Window;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&State));Viewer=State.window_;return Viewer!=nullptr;
    }));
    QCOMPARE(GetAncestor(Viewer,GA_ROOT),Viewer);
    bool Parsed=false;const auto Native=Surface.Details().documentReply_["nativeWindow"].toString().toULongLong(&Parsed,16);
    QVERIFY(Parsed);QCOMPARE(quintptr(Native),quintptr(Viewer));
    QVERIFY(!IsChild(reinterpret_cast<HWND>(Surface.winId()),Viewer));
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();}));
}
void isolatedComicArchiveReading(){
    QTemporaryDir Root;
    QImage Page(40,60,QImage::Format_RGB32);Page.fill(Qt::green);
    QVERIFY(Page.save(Root.filePath("1.png")));QVERIFY(Page.save(Root.filePath("2.png")));
    QProcess Pack;Pack.setWorkingDirectory(Root.path());
    Pack.start(QCoreApplication::applicationDirPath()+"/../sevenzip-tools/official/7z.exe",{"a","-tzip","comic 01.cbz","1.png","2.png"});
    QVERIFY(Pack.waitForFinished(15000));QCOMPARE(Pack.exitCode(),0);
    QVERIFY(QFile::copy(Root.filePath("comic 01.cbz"),Root.filePath("comic 02.cbz")));
    const auto Private=Root.filePath("private");QVERIFY(QDir().mkpath(Private));
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtPluginViewerController Controller(nullptr,[Live,Private]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;Item->copyRoot_=Private;return std::shared_ptr<IMaterializer>(Item);});
    QtPluginViewerSurface Surface(&Controller);Surface.Open(Options(),Content(Root.filePath("comic 01.cbz")));
    const bool Ready=Wait([&]{return Surface.IsReady()&&Surface.Details().documentState_["idle"].toBool();},30000);
    QVERIFY2(Ready,qPrintable(QString::fromStdString(Surface.Snapshot().error_?Surface.Snapshot().error_->code_:std::string{})+" "+QString::fromUtf8(QJsonDocument(Surface.Details().documentState_).toJson(QJsonDocument::Compact))));
    auto State=Surface.Details().documentState_;const auto Id=State["activeDocumentId"].toString();
    const auto Document=Doc(State,Id);
    QVERIFY2(Document["lifecycle"]=="ready",qPrintable(QJsonDocument(State).toJson(QJsonDocument::Compact)));
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["view"].toObject()["reading"].toObject()["book"].toString()==Root.filePath("comic 01.cbz");}));
    QCOMPARE(Doc(Surface.Details().documentState_,Id)["view"].toObject()["reading"].toObject()["pageCount"].toInt(),2);
    const auto Sequence=Surface.SendDocument({{"action","reading.page"},{"page",1},{"documentId",Id}});QVERIFY(Sequence.HasValue());
    QVERIFY(Wait([&]{return Surface.Details().commandSequence_>=Sequence.Value();}));QVERIFY(!Surface.Details().commandError_);
    QCOMPARE(Doc(Surface.Details().documentState_,Id)["view"].toObject()["reading"].toObject()["page"].toInt(),1);
    const auto Next=Surface.SendDocument({{"action","reading.volume"},{"direction",1},{"documentId",Id}});QVERIFY(Next.HasValue());
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["view"].toObject()["reading"].toObject()["book"].toString()==Root.filePath("comic 02.cbz");}));
    QVERIFY(!Doc(Surface.Details().documentState_,Id)["dirty"].toBool());
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();}));
}
void isolatedImageSiblingKeyboardNavigation(){
    QTemporaryDir Root;
    QImage Page(40,60,QImage::Format_RGB32);Page.fill(Qt::green);
    for(int I=1;I<=3;++I)QVERIFY(Page.save(Root.filePath(QString::number(I)+".png")));
    const auto Private=Root.filePath("private");QVERIFY(QDir().mkpath(Private));
    QWidget Main;Main.resize(900,650);Main.show();Main.activateWindow();QVERIFY(QTest::qWaitForWindowActive(&Main));
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtViewerHost Host(&Main,[Live,Private]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;Item->copyRoot_=Private;return std::shared_ptr<IMaterializer>(Item);});
    const auto Drain=qScopeGuard([&]{Host.CloseAll();Wait([&]{return Host.IsIdle();});});
    bool Finished=false,Success=false;QString Failure;
    auto* Window=Host.OpenPlugin(Options(),{{Content(Root.filePath("2.png")),{}}},"Image keys",
        [&](bool Ready,const QString& Error){Finished=true;Success=Ready;Failure=Error;});
    QVERIFY(Window);QVERIFY(Wait([&]{return Finished;},30000));QVERIFY2(Success,qPrintable(Failure));
    auto* Surface=Window->findChild<QtPluginViewerSurface*>();QVERIFY(Surface);
    const auto Pid=Surface->Details().host_.processId_;
    QVERIFY(Wait([&]{DWORD ForegroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&ForegroundPid);return ForegroundPid==Pid;}));
    const auto Viewer=GetForegroundWindow();
    const auto Current=[&]{const auto State=Surface->Details().documentState_;return Doc(State,State["activeDocumentId"].toString());};
    const auto Id=Current()["documentId"].toString();
    QCOMPARE(Current()["path"].toString(),Private+"/2.png");
    QVERIFY(!Current()["sourceToken"].toString().isEmpty());
    const auto Press=[&](WORD Key){INPUT Keys[2]{};Keys[0].type=Keys[1].type=INPUT_KEYBOARD;Keys[0].ki.wVk=Keys[1].ki.wVk=Key;Keys[1].ki.dwFlags=KEYEVENTF_KEYUP;return SendInput(2,Keys,sizeof(INPUT));};
    QCOMPARE(Press(VK_SPACE),UINT(2));
    QVERIFY(Wait([&]{return Current()["path"].toString()==Root.filePath("3.png")&&Current()["lifecycle"]=="ready";},20000));
    QCOMPARE(Surface->Details().documentState_["documents"].toArray().size(),1);QCOMPARE(Current()["documentId"].toString(),Id);
    QVERIFY(Current()["sourceToken"].toString().isEmpty());
    QCOMPARE(Press(VK_BACK),UINT(2));
    QVERIFY(Wait([&]{return Current()["path"].toString()==Root.filePath("2.png")&&Current()["lifecycle"]=="ready";},20000));
    QCOMPARE(Surface->Details().documentState_["documents"].toArray().size(),1);QCOMPARE(Current()["documentId"].toString(),Id);
    QVERIFY(!Current()["dirty"].toBool());
    QVERIFY(PostMessageW(Viewer,WM_CLOSE,0,0));
    QVERIFY(Wait([&]{return !IsWindow(Viewer)||!IsWindowVisible(Viewer);},20000));
}
void f3WindowStaysRestoredAfterLoadingDialogHides(){
    QTemporaryDir Root;const auto Path=Root.filePath("visible.txt");QVERIFY(Put(Path,"visible"));
    QWidget Main;Main.resize(1100,800);Main.show();
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtViewerHost Host(&Main,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);
    });
    Host.PreparePlugin(Options());
    QVERIFY(Wait([&]{
        auto* Prepared=Main.findChild<QDialog*>("hc.document.prepared");
        auto* Surface=Prepared?Prepared->findChild<QtPluginViewerSurface*>():nullptr;
        return Surface&&Surface->IsReady();
    },30000));
    bool Finished=false,Success=false;QString Failure;
    const auto Window=Host.OpenPlugin(Options(),{{Content(Path),{}}},"HyperViewer",
        [&](bool Ready,const QString& Error){Finished=true;Success=Ready;Failure=Error;});
    QVERIFY(Window);
    QVERIFY2(Wait([&]{return Finished;},30000),qPrintable(Failure));
    QVERIFY2(Success,qPrintable(Failure));
    QVERIFY(Window->isHidden());
    const auto Surface=Window->findChild<QtPluginViewerSurface*>();QVERIFY(Surface);
    const auto Pid=Surface->Details().host_.processId_;QVERIFY(Pid);
    HWND Viewer=nullptr;
    QVERIFY(Wait([&]{
        struct Finder{DWORD pid_;HWND window_=nullptr;};Finder State{Pid};
        EnumWindows([](HWND Candidate,LPARAM Context)->BOOL{
            auto* Search=reinterpret_cast<Finder*>(Context);DWORD Owner=0;GetWindowThreadProcessId(Candidate,&Owner);
            wchar_t Title[256]{};GetWindowTextW(Candidate,Title,256);
            if(Owner==Search->pid_&&IsWindowVisible(Candidate)&&QString::fromWCharArray(Title).startsWith("HyperViewer"))
                {Search->window_=Candidate;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&State));Viewer=State.window_;return Viewer!=nullptr;
    }));
    QTest::qWait(1200);
    QVERIFY(IsWindow(Viewer));
    const auto Foreground=GetForegroundWindow();DWORD ForegroundPid=0;
    GetWindowThreadProcessId(Foreground,&ForegroundPid);wchar_t ForegroundTitle[256]{};
    GetWindowTextW(Foreground,ForegroundTitle,256);
    qInfo().noquote()<<QString("F3 viewer=%1 foreground=%2 pid=%3 title=%4 iconic=%5 dialogHidden=%6")
        .arg(quintptr(Viewer),0,16).arg(quintptr(GetForegroundWindow()),0,16)
        .arg(ForegroundPid).arg(QString::fromWCharArray(ForegroundTitle))
        .arg(bool(IsIconic(Viewer))).arg(Window->isHidden());
    if(Foreground)QVERIFY2(Foreground!=reinterpret_cast<HWND>(Main.winId()),
        "HyperCommander regained the foreground after presenting the F3 viewer");
    QVERIFY2(!IsIconic(Viewer),"F3 viewer minimized after the loading dialog was hidden");
    QVERIFY(IsWindowVisible(Viewer));
    Host.CloseAll();QVERIFY(Wait([&]{return Host.IsIdle();}));
}
void f3TextAcceptsKeyboardWithoutClick_data(){
    QTest::addColumn<bool>("Prepared");
    QTest::newRow("cold")<<false; QTest::newRow("prepared")<<true;
}
void f3TextAcceptsKeyboardWithoutClick(){
    QFETCH(bool,Prepared);
    QTemporaryDir Root; const auto Path=Root.filePath("keyboard.txt"); QVERIFY(Put(Path,"keyboard fixture"));
    QWidget Main; QVBoxLayout Layout(&Main); QLineEdit Input; Layout.addWidget(&Input);
    Main.resize(800,600); Main.show(); Main.activateWindow();
    SetForegroundWindow(reinterpret_cast<HWND>(Main.winId()));
    QVERIFY(QTest::qWaitForWindowActive(&Main)); Input.setFocus();
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtViewerHost Host(&Main,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);
    });
    const auto Drain=qScopeGuard([&]{Host.CloseAll();Wait([&]{return Host.IsIdle();});});
    if(Prepared){
        Host.PreparePlugin(Options());
        QVERIFY(Wait([&]{auto* Window=Main.findChild<QDialog*>("hc.document.prepared");
            auto* Surface=Window?Window->findChild<QtPluginViewerSurface*>():nullptr;return Surface&&Surface->IsReady();},30000));
        QCOMPARE(GetForegroundWindow(),reinterpret_cast<HWND>(Main.winId()));
        QVERIFY(Input.hasFocus());
    }
    // Real input to HC after prewarming revokes the helper's foreground grant.
    // A direct OpenPlugin call can leave that grant in place and hide this race.
    INPUT Keys[2]{}; Keys[0].type=Keys[1].type=INPUT_KEYBOARD;
    Keys[0].ki.wVk=Keys[1].ki.wVk='X'; Keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
    QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2)); QVERIFY(Wait([&]{return !Input.text().isEmpty();}));
    const auto MainText=Input.text();
    bool Finished=false,Success=false; QString Failure; QDialog* Window=nullptr;
    QShortcut Open(QKeySequence(Qt::Key_F3),&Main);
    connect(&Open,&QShortcut::activated,&Main,[&]{Window=Host.OpenPlugin(Options(),{{Content(Path),{}}},"Keyboard F3",
        [&](bool Ready,const QString& Error){Finished=true;Success=Ready;Failure=Error;});});
    Keys[0].ki.wVk=Keys[1].ki.wVk=VK_F3;
    QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2)); QVERIFY(Wait([&]{return Window!=nullptr;}));
    QVERIFY2(Wait([&]{return Finished;},30000),qPrintable(Failure)); QVERIFY2(Success,qPrintable(Failure));
    auto* Surface=Window->findChild<QtPluginViewerSurface*>(); QVERIFY(Surface);
    HWND Viewer=nullptr; const auto Pid=Surface->Details().host_.processId_; QVERIFY(Pid);
    QVERIFY(Wait([&]{
        struct Finder{DWORD pid_;HWND window_=nullptr;};Finder State{Pid};
        EnumWindows([](HWND Candidate,LPARAM Context)->BOOL{
            auto* Search=reinterpret_cast<Finder*>(Context);DWORD Owner=0;GetWindowThreadProcessId(Candidate,&Owner);
            wchar_t Title[256]{};GetWindowTextW(Candidate,Title,256);
            if(Owner==Search->pid_&&IsWindowVisible(Candidate)&&QString::fromWCharArray(Title).startsWith("HyperViewer"))
                {Search->window_=Candidate;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&State));Viewer=State.window_;return Viewer!=nullptr;
    }));
    QVERIFY(Wait([&]{return GetForegroundWindow()==Viewer;}));
    GUITHREADINFO Thread{sizeof(GUITHREADINFO)};
    QVERIFY(GetGUIThreadInfo(GetWindowThreadProcessId(Viewer,nullptr),&Thread));
    qInfo()<<"F3 native foreground/focus"<<Viewer<<Thread.hwndFocus;
    const auto State=Surface->Details().documentState_; const auto Id=State["activeDocumentId"].toString(); QVERIFY(!Id.isEmpty());
    const auto Revision=Doc(State,Id)["revision"].toString();
    // Do not repair focus or click the viewer: input must work on presentation.
    Keys[0].ki.wVk=Keys[1].ki.wVk='Z'; Keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
    QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2));
    QVERIFY2(Wait([&]{return Doc(Surface->Details().documentState_,Id)["revision"].toString()!=Revision;},1500),
        "The newly presented F3 text viewer must accept keyboard input without a click");
    QVERIFY(Doc(Surface->Details().documentState_,Id)["dirty"].toBool()); QCOMPARE(Input.text(),MainText);
}
void twoF3WindowsSwitchKeyboardInput(){
    QTemporaryDir Root;
    const auto FirstPath=Root.filePath("first.txt"), SecondPath=Root.filePath("second.txt");
    QVERIFY(Put(FirstPath,"first"));QVERIFY(Put(SecondPath,"second"));
    QWidget Main;Main.resize(1100,800);Main.show();
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtPluginViewerController Controller(&Main,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);
    });
    QDialog FirstDialog(&Main),SecondDialog(&Main);
    FirstDialog.resize(950,700);SecondDialog.resize(950,700);
    auto* FirstSurface=new QtPluginViewerSurface(&Controller,&FirstDialog);
    auto* SecondSurface=new QtPluginViewerSurface(&Controller,&SecondDialog);
    (new QVBoxLayout(&FirstDialog))->addWidget(FirstSurface);
    (new QVBoxLayout(&SecondDialog))->addWidget(SecondSurface);
    FirstDialog.show();SecondDialog.show();
    QSignalSpy FirstFailed(FirstSurface,&QtPluginViewerSurface::Failed);
    QSignalSpy SecondFailed(SecondSurface,&QtPluginViewerSurface::Failed);
    FirstSurface->Open(Options(),Content(FirstPath));
    SecondSurface->Open(Options(),Content(SecondPath));
    const auto Loaded=Wait([&]{return (FirstSurface->IsReady()&&SecondSurface->IsReady()
        && FirstSurface->Details().documentState_["idle"].toBool()
        && SecondSurface->Details().documentState_["idle"].toBool())
        || !FirstFailed.isEmpty() || !SecondFailed.isEmpty();},30000);
    QVERIFY2(Loaded && FirstFailed.isEmpty() && SecondFailed.isEmpty()
        && FirstSurface->IsReady() && SecondSurface->IsReady(),
        qPrintable(QString("F3 open: first=%1 second=%2 snapshot=%3/%4")
            .arg(FirstFailed.isEmpty()?QString():FirstFailed.first().first().toString(),
                SecondFailed.isEmpty()?QString():SecondFailed.first().first().toString(),
                FirstSurface->Snapshot().error_?QString::fromStdString(FirstSurface->Snapshot().error_->code_):QString(),
                SecondSurface->Snapshot().error_?QString::fromStdString(SecondSurface->Snapshot().error_->code_):QString())));
    const auto FindViewer=[](DWORD Pid){
        struct Finder{DWORD pid_;HWND window_=nullptr;};
        Finder State{Pid};
        EnumWindows([](HWND Window,LPARAM Context)->BOOL{
            auto* Search=reinterpret_cast<Finder*>(Context);DWORD Owner=0;GetWindowThreadProcessId(Window,&Owner);
            if(Owner!=Search->pid_||!IsWindowVisible(Window))return TRUE;
            wchar_t Title[256]{};GetWindowTextW(Window,Title,256);
            if(QString::fromWCharArray(Title).startsWith("HyperViewer")){Search->window_=Window;return FALSE;}
            return TRUE;
        },reinterpret_cast<LPARAM>(&State));
        return State.window_;
    };
    auto Present=[&](QtPluginViewerSurface* Surface){
        const auto Sequence=Surface->SendDocument({{"action","present"}});QVERIFY(Sequence.HasValue());
        QVERIFY(Wait([&]{return Surface->Details().commandSequence_>=Sequence.Value();}));
        QVERIFY(!Surface->Details().commandError_);
        const auto Pid=Surface->Details().host_.processId_;QVERIFY(Pid&&Pid!=GetCurrentProcessId());
        QVERIFY(Wait([&]{return FindViewer(Pid)!=nullptr;}));
    };
    Present(FirstSurface);Present(SecondSurface);
    FirstDialog.hide();SecondDialog.hide();
    const auto Screen=QGuiApplication::primaryScreen()->availableGeometry();
    const auto Width=std::max(400,Screen.width()/2-20);
    const auto Height=std::min(700,Screen.height()-20);
    QVERIFY(SetWindowPos(FindViewer(FirstSurface->Details().host_.processId_),HWND_TOP,
        Screen.left()+10,Screen.top()+10,Width,Height,SWP_SHOWWINDOW));
    QVERIFY(SetWindowPos(FindViewer(SecondSurface->Details().host_.processId_),HWND_TOP,
        Screen.left()+Screen.width()/2+10,Screen.top()+10,Width,Height,SWP_SHOWWINDOW));
    POINT Cursor{};
    if(!GetCursorPos(&Cursor)||!SetCursorPos(Cursor.x,Cursor.y))
        QSKIP("Interactive input desktop is unavailable to the test runner");
    auto FocusAndType=[&](QtPluginViewerSurface* Target,QtPluginViewerSurface* Other){
        const auto Pid=Target->Details().host_.processId_;
        const auto Viewer=FindViewer(Pid);QVERIFY(Viewer);
        RECT Bounds{};QVERIFY(GetWindowRect(Viewer,&Bounds));
        QVERIFY(SetCursorPos(Bounds.left+180,Bounds.top+240));
        INPUT Mouse[2]{};Mouse[0].type=Mouse[1].type=INPUT_MOUSE;
        Mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;Mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
        QCOMPARE(SendInput(2,Mouse,sizeof(INPUT)),UINT(2));
        const auto Focused=Wait([&]{return GetForegroundWindow()==Viewer;});
        if(!Focused){
            wchar_t ForegroundTitle[256]{};DWORD ForegroundPid=0;
            const auto Foreground=GetForegroundWindow();GetWindowTextW(Foreground,ForegroundTitle,256);
            GetWindowThreadProcessId(Foreground,&ForegroundPid);
            POINT Click{Bounds.left+180,Bounds.top+240};
            qWarning().noquote()<<QString("F3 focus target=%1 pid=%2 bounds=%3,%4,%5,%6 hit=%7 foreground=%8 pid=%9 title=%10")
                .arg(quintptr(Viewer),0,16).arg(Pid).arg(Bounds.left).arg(Bounds.top).arg(Bounds.right).arg(Bounds.bottom)
                .arg(quintptr(WindowFromPoint(Click)),0,16).arg(quintptr(Foreground),0,16).arg(ForegroundPid)
                .arg(QString::fromWCharArray(ForegroundTitle));
        }
        QVERIFY(Focused);
        const auto Before=Target->Details().documentState_;
        const auto Id=Before["activeDocumentId"].toString();QVERIFY(!Id.isEmpty());
        const auto Revision=Doc(Before,Id)["revision"].toString();
        const auto OtherBefore=Other->Details().documentState_;
        const auto OtherId=OtherBefore["activeDocumentId"].toString();QVERIFY(!OtherId.isEmpty());
        const auto OtherRevision=Doc(OtherBefore,OtherId)["revision"].toString();
        INPUT Keys[2]{};Keys[0].type=Keys[1].type=INPUT_KEYBOARD;
        Keys[0].ki.wVk=Keys[1].ki.wVk='Z';Keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2));
        QVERIFY2(Wait([&]{return Doc(Target->Details().documentState_,Id)["revision"].toString()!=Revision;},1500),
            "Active F3 window did not accept keyboard input promptly");
        QCOMPARE(Doc(Other->Details().documentState_,OtherId)["revision"].toString(),OtherRevision);
    };
    FocusAndType(FirstSurface,SecondSurface);
    FocusAndType(SecondSurface,FirstSurface);
    FocusAndType(FirstSurface,SecondSurface);
    FirstSurface->Close();SecondSurface->Close();
    QVERIFY(Wait([&]{return Controller.IsIdle();}));
}
void hcSurfaceBrokerSaveClipboardAndPerTabLeases(){
    QTemporaryDir Root;const auto Large=Root.filePath("large.txt");QByteArray Bytes(2*1024*1024,'x');Bytes.replace(0,3,"old");QVERIFY(Put(Large,Bytes));
    auto Live=std::make_shared<std::atomic<int>>(0);QtPluginViewerController Controller(nullptr,[Live]()->Result<std::shared_ptr<IMaterializer>>{auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
    QtPluginViewerSurface Surface(&Controller);Surface.setWindowTitle("F3 input verification");Surface.resize(1000,700);Surface.show();Surface.Open(Options(),Content(Large));
    auto* Loading=Surface.findChild<QLabel*>(QString{},Qt::FindDirectChildrenOnly);QVERIFY(Loading);
    auto* Stack=qobject_cast<QStackedLayout*>(Surface.layout());QVERIFY(Stack);
    QCOMPARE(Stack->currentWidget(),Loading);QCOMPARE(Loading->geometry(),Surface.rect());
    auto* Anchor=Surface.findChild<QWidget*>("hc.wlx.native-parent");QVERIFY(Anchor);QVERIFY(Anchor->isHidden());
    QVERIFY2(Wait([&]{return Surface.IsReady()&&!Surface.Details().documentState_["documents"].toArray().isEmpty();},30000),Surface.Snapshot().error_?Surface.Snapshot().error_->code_.c_str():"document-not-ready");
    QCOMPARE(Stack->currentWidget(),Anchor);QVERIFY(Loading->isHidden());QVERIFY(Anchor->isVisible());
    auto Initial=Surface.Details().documentState_;auto Id=Initial["documents"].toArray()[0].toObject()["documentId"].toString();
    QCOMPARE(Initial["documents"].toArray().size(),1);QVERIFY(!Doc(Initial,Id)["sourceToken"].toString().isEmpty());
    QCOMPARE(Doc(Initial,Id)["displayName"].toString(),QFileInfo(Large).fileName());
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["lifecycle"].toString()=="ready";}));
    auto Send=[&](QJsonObject Request){auto Result=Surface.SendDocument(Request);if(!Result.HasValue())return false;const auto Sequence=Result.Value();return Wait([&]{return Surface.Details().commandSequence_>=Sequence;})&&!Surface.Details().commandError_;};
    QVERIFY(Send({{"action","text.replaceAll"},{"documentId",Id},{"text","old"},{"replacement","new"}}));QVERIFY(Send({{"action","save"},{"documentId",Id}}));
    Bytes.replace(0,3,"new");QVERIFY2(Wait([&]{const auto State=Doc(Surface.Details().documentState_,Id);return !State["dirty"].toBool()&&!State["saving"].toBool();}),qPrintable(QJsonDocument(Surface.Details().documentState_).toJson(QJsonDocument::Compact)+" viewerError="+QString::fromStdString(Surface.Snapshot().error_?Surface.Snapshot().error_->code_:std::string{}).toUtf8()));QCOMPARE(Read(Large),Bytes);
    // The SDK anchor stays local to the isolated host; the viewer is a top-level window there.
    QVERIFY(Send({{"action","present"}}));
    const auto HostPid=Surface.Details().host_.processId_;QVERIFY(HostPid&&HostPid!=GetCurrentProcessId());
    QVERIFY(Send({{"action","replaceText"},{"documentId",Id},{"text","clipboard text"}}));QVERIFY(Send({{"action","selectAll"},{"documentId",Id}}));
    QVERIFY(Send({{"action","clipboard"},{"documentId",Id},{"mode","copy"}}));QVERIFY(Wait([&]{return QApplication::clipboard()->text()=="clipboard text";}));
    QApplication::clipboard()->setText("pasted text");QVERIFY(Send({{"action","clipboard"},{"documentId",Id},{"mode","paste"}}));
    QVERIFY(Wait([&]{return Surface.Details().documentState_["clipboard"].toObject().isEmpty();}));
    QVERIFY(Send({{"action","save"},{"documentId",Id}}));QVERIFY(Wait([&]{return !Doc(Surface.Details().documentState_,Id)["dirty"].toBool()&&!Doc(Surface.Details().documentState_,Id)["saving"].toBool();}));QCOMPARE(Read(Large),QByteArray("pasted text"));
    const auto Pdf=Root.filePath("doc.pdf"),Image=Root.filePath("image.png");QVERIFY(QFile::copy(Fixture("basic.pdf"),Pdf));QVERIFY(QFile::copy(Fixture("alpha.png"),Image));
    auto Added=Surface.OpenDocuments({{Content(Pdf),{}},{Content(Image),{}}});QVERIFY(Added.HasValue());
    QVERIFY(Wait([&]{return Surface.Details().documentState_["documents"].toArray().size()==3&&Surface.Details().documentState_["idle"].toBool();}));
    QCOMPARE(Live->load(),3);
    for(const auto& Item:Surface.Details().documentState_["documents"].toArray()) {
        const auto Record=Item.toObject();const auto View=Record["view"].toObject()["viewId"].toString();
        if(View=="pdf")QCOMPARE(Record["displayName"].toString(),QString("doc.pdf"));
        if(View=="image")QCOMPARE(Record["displayName"].toString(),QString("image.png"));
    }
    auto State=Surface.Details().documentState_;QString ImageId;for(const auto& Item:State["documents"].toArray())if(Item.toObject()["view"].toObject()["viewId"].toString()=="image")ImageId=Item.toObject()["documentId"].toString();
    QVERIFY(Send({{"action","clipboard"},{"documentId",ImageId},{"mode","copy"}}));QVERIFY(Wait([&]{return !QApplication::clipboard()->image().isNull();}));
    QVERIFY(Send({{"action","close"},{"documentId",ImageId},{"discard",true}}));QVERIFY(Wait([&]{return Live->load()==2;}));
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();}));QCOMPARE(Live->load(),0);
}
void pluginHostCrashAndTimeout_data(){QTest::addColumn<bool>("Hang");QTest::newRow("crash")<<false;QTest::newRow("timeout")<<true;}
void pluginHostCrashAndTimeout(){
    QFETCH(bool,Hang);QTemporaryDir Root;const auto Path=Root.filePath("crash.txt");QVERIFY(Put(Path,"crash test"));
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtPluginViewerController Controller(nullptr,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
    QtPluginViewerSurface Surface(&Controller);QSignalSpy Failed(&Surface,&QtPluginViewerSurface::Failed);
    Surface.Open(Options(),Content(Path));
    QVERIFY(Wait([&]{return Surface.IsReady()&&Surface.Details().documentState_["idle"].toBool();},30000));
    const auto Pid=Surface.Details().host_.processId_;QVERIFY(Pid&&Pid!=GetCurrentProcessId());
    auto Process=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE|PROCESS_SUSPEND_RESUME,FALSE,Pid);QVERIFY(Process);auto Cleanup=qScopeGuard([&]{CloseHandle(Process);});
    if(Hang){using Fn=LONG(WINAPI*)(HANDLE);auto Suspend=reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSuspendProcess"));QVERIFY(Suspend);QCOMPARE(Suspend(Process),LONG(0));}
    else QVERIFY(TerminateProcess(Process,0xe0000300));
    QElapsedTimer Timer;Timer.start();QVERIFY(Wait([&]{return !Failed.isEmpty();},16000));QVERIFY(Timer.elapsed()<17000);
    if(Hang&&WaitForSingleObject(Process,0)==WAIT_TIMEOUT)
        QVERIFY(TerminateProcess(Process,0xe0000301));
    QVERIFY(Wait([&]{return WaitForSingleObject(Process,0)==WAIT_OBJECT_0;},6000));
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();}));
}
void sdkReplacementAndForcedParentExit(){
    QTemporaryDir Root;const auto Report=Root.filePath("parent.json");QProcess Parent;Parent.start(QCoreApplication::applicationFilePath(),{"--parent-probe",Report});
    QVERIFY(Wait([&]{return QFile::exists(Report);},20000));auto State=QJsonDocument::fromJson(Read(Report)).object();const auto Pid=State["hostPid"].toInt();QVERIFY(Pid);
    auto Helper=OpenProcess(SYNCHRONIZE,FALSE,Pid);QVERIFY(Helper);auto Cleanup=qScopeGuard([&]{CloseHandle(Helper);});
    Parent.kill();QVERIFY(Parent.waitForFinished(5000));QVERIFY(Wait([&]{return WaitForSingleObject(Helper,0)==WAIT_OBJECT_0;},5000));
    const auto RecoveryRoot=State["recoveryRoot"].toString(),Id=State["documentId"].toString();
    QVERIFY(QFile::exists(RecoveryRoot+"/"+Id+".json"));DocumentRecoveryStore Recovery(RecoveryRoot);QVERIFY(Recovery.Remove(Id.toStdString(),UINT64_MAX).HasValue());
}
void memoryByDocumentSizeAndTabCount(){
    QTemporaryDir Root;for(const auto& Scenario:QList<QPair<int,int>>{{16*1024,1},{256*1024,8},{1024*1024,4},{4*1024*1024,1}}){
        Native Dll;QVERIFY(Wait([&]{return Dll.Call({{"action","state"}}).contains("documents");}));
        for(int Index=0;Index<Scenario.second;++Index){const auto Path=Root.filePath(QString("text-%1-%2.txt").arg(Scenario.first).arg(Index));QByteArray Text;while(Text.size()<Scenario.first)Text+="measured text line 0123456789\r\n";Text.truncate(Scenario.first);QVERIFY(Put(Path,Text));QVERIFY(!Dll.Call({{"action","open"},{"path",Path}}).contains("error"));}
        QJsonObject State;QVERIFY(Wait([&]{State=Dll.Call({{"action","state"}});return State["idle"].toBool() || State.contains("error");},30000));QVERIFY2(!State.contains("error"),qPrintable(QJsonDocument(State).toJson(QJsonDocument::Compact)));
        const auto Private=State["memoryPrivateBytes"].toString().toULongLong();QVERIFY(Private>0&&Private<ViewerMemoryLimit);
        qInfo().noquote()<<QString("P3_MEMORY bytes=%1 tabs=%2 private=%3 peak_working_set=%4 limit=%5").arg(Scenario.first).arg(Scenario.second).arg(Private).arg(State["memoryPeakWorkingSetBytes"].toString()).arg(ViewerMemoryLimit);
    }
    for(const auto& Scenario:QList<QPair<QSize,int>>{{QSize(512,512),4},{QSize(2048,2048),1}}){
        Native Dll;QVERIFY(Wait([&]{return Dll.Call({{"action","state"}}).contains("documents");}));
        QImage Image(Scenario.first,QImage::Format_RGBA8888);Image.fill(QColor(150,80,170,180));
        for(int Index=0;Index<Scenario.second;++Index){const auto Path=Root.filePath(QString("image-%1-%2.png").arg(Image.width()).arg(Index));QVERIFY(Image.save(Path,"PNG"));QVERIFY(!Dll.Call({{"action","open"},{"path",Path}}).contains("error"));}
        QJsonObject State;QVERIFY(Wait([&]{State=Dll.Call({{"action","state"}});return State["idle"].toBool()||State.contains("error");},90000));QVERIFY2(!State.contains("error"),qPrintable(QJsonDocument(State).toJson(QJsonDocument::Compact)));
        const auto Peak=State["memoryPeakPrivateBytes"].toString().toULongLong();QVERIFY(Peak>0&&Peak<ViewerMemoryLimit);
        qInfo().noquote()<<QString("P3_MEMORY image=%1x%2 tabs=%3 private=%4 peak_private=%5 limit=%6").arg(Image.width()).arg(Image.height()).arg(Scenario.second).arg(State["memoryPrivateBytes"].toString()).arg(Peak).arg(ViewerMemoryLimit);
    }
}
};
int main(int Argc,char** Argv){
    QApplication App(Argc,Argv);QTemporaryDir Config;App.setProperty("documentViewsConfigDir",Config.path());
    if(App.arguments().contains("--probe-dpr")){std::cout<<App.primaryScreen()->devicePixelRatio()<<std::endl;return 0;}
    if(App.arguments().contains("--parent-probe")){
        QTemporaryDir Root;const auto Path=Root.filePath("parent.txt");Put(Path,"parent");
        auto Live=std::make_shared<std::atomic<int>>(0);
        QtPluginViewerController Controller(nullptr,[Live]()->Result<std::shared_ptr<IMaterializer>>{
            auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
        QtPluginViewerSurface Surface(&Controller);Surface.Open(Options(),Content(Path));
        if(!Wait([&]{return Surface.IsReady()&&Surface.Details().documentState_["idle"].toBool();},30000))return 2;
        auto State=Surface.Details().documentState_;
        const auto Id=State["activeDocumentId"].toString();if(Id.isEmpty())return 3;
        auto Send=[&](QJsonObject Request){const auto Result=Surface.SendDocument(Request);
            return Result.HasValue()&&Wait([&]{return Surface.Details().commandSequence_>=Result.Value();})
                &&!Surface.Details().commandError_;};
        if(!Send({{"action","replaceText"},{"documentId",Id},{"text","parent crash recovery"}})
            ||!Send({{"action","checkpoint"}}))return 4;
        State=Surface.Details().documentState_;
        if(!Wait([&]{return QFile::exists(State["recoveryRoot"].toString()+"/"+Id+".json");}))return 4;
        State["hostPid"]=int(Surface.Details().host_.processId_);State["documentId"]=Id;
        if(!Put(App.arguments().last(),QJsonDocument(State).toJson(QJsonDocument::Compact)))return 5;
        return App.exec();
    }
    DocumentP3Tests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstDocumentP3.moc"
