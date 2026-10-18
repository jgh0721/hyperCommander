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
    DWORD Helper(){DWORD Pid=0;GetWindowThreadProcessId(FindWindowExW(viewer_,nullptr,nullptr,nullptr),&Pid);return Pid;}
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
    EntrySnapshot entry_;std::shared_ptr<std::atomic<int>> live_;
    Lease(EntrySnapshot Entry,std::shared_ptr<std::atomic<int>> Live):entry_(Entry),live_(Live){++*live_;}
    ~Lease(){--*live_;}
    std::string LocalPath()const override{return entry_.locator_.root_;}
    EntrySnapshot Describe()const override{return entry_;}
    std::size_t Depth()const override{return 0;}
};
class Materializer final:public IMaterializer{
public:
    std::shared_ptr<std::atomic<int>> live_;
    Task<std::shared_ptr<IMaterializedFileLease>> Materialize(MaterializationRequest Request,OperationContext Context)override{
        if(auto Stop=Context.Check({"test"}))return Ready<std::shared_ptr<IMaterializedFileLease>>(*Stop);
        return Ready<std::shared_ptr<IMaterializedFileLease>>(std::shared_ptr<IMaterializedFileLease>(std::make_shared<Lease>(Request.source_->Describe(),live_)));}
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
    const auto Pid=Dll.Helper(); QVERIFY(Pid);
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
        SetForegroundWindow(Dialog); QVERIFY(Wait([&]{return GetForegroundWindow()==Dialog;}));
        QVERIFY(!Dll.Call({{"action","focus"}})["accepted"].toBool()); QCOMPARE(GetForegroundWindow(),Dialog);
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
void nestedF3Input(){
    QTemporaryDir Root;const auto Path=Root.filePath("tiny.txt");QVERIFY(Put(Path,QByteArray(369,'x')));
    QWidget Main;Main.resize(1100,800);Main.show();
    auto Live=std::make_shared<std::atomic<int>>(0);QtPluginViewerController Controller(&Main,[Live]()->Result<std::shared_ptr<IMaterializer>>{auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
    QDialog Window(&Main);Window.resize(1000,700);auto* Layout=new QVBoxLayout(&Window);
    QtPluginViewerSurface Surface(&Controller,&Window);Layout->addWidget(&Surface);
    auto Config=Options();QElapsedTimer Timer;Timer.start();Surface.Open(Config,Content(Path));Window.show();Window.activateWindow();SetActiveWindow(reinterpret_cast<HWND>(Window.winId()));
    QVERIFY(Wait([&]{return Surface.IsReady();},30000));qInfo()<<"F3 ready ms"<<Timer.elapsed();
    auto Id=Surface.Details().documentState_["activeDocumentId"].toString();
    // The product intentionally refuses background focus requests. Establish
    // the same foreground precondition as a user opening F3 before asserting focus.
    SetForegroundWindow(reinterpret_cast<HWND>(Window.winId()));
    QVERIFY(Wait([&]{return GetForegroundWindow()==reinterpret_cast<HWND>(Window.winId());}));
    Surface.clearFocus();Surface.setFocus();QTest::qWait(500);
    auto Sdk=reinterpret_cast<HWND>(Surface.Details().child_.toULongLong(nullptr,16));auto Helper=GetWindow(Sdk,GW_CHILD);
    if(qEnvironmentVariableIsSet("HC_F3_NESTED_SHELL_FOCUS")){SetFocus(Helper);QTest::qWait(100);}
    GUITHREADINFO Gui{sizeof(GUITHREADINFO)};GetGUIThreadInfo(GetWindowThreadProcessId(Helper,nullptr),&Gui);
    qInfo()<<"foreground"<<quintptr(GetForegroundWindow())<<"dialog"<<Window.winId()<<"root"<<quintptr(GetAncestor(Helper,GA_ROOT))<<"focus"<<quintptr(Gui.hwndFocus)<<"active"<<quintptr(Gui.hwndActive)<<"helper"<<quintptr(Helper)<<"qtFocus"<<Doc(Surface.Details().documentState_,Id)["view"];
    // Qt may keep native focus on the workspace shell while delivering keys
    // to its logical Scintilla focus widget. Both must belong to this helper.
    QVERIFY(Gui.hwndFocus==Helper||IsChild(Helper,Gui.hwndFocus));
    QCOMPARE(quintptr(Gui.hwndActive),quintptr(Window.winId()));
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["view"].toObject()["inputFocused"].toBool();}));
    if(qEnvironmentVariableIsSet("HC_F3_NESTED_INTERACTIVE_TEST")){
        SetWindowLongPtrW(reinterpret_cast<HWND>(Window.winId()),GWLP_HWNDPARENT,0);
        QFile Evidence(qEnvironmentVariable("HC_F3_NESTED_INTERACTIVE_TEST"));QVERIFY(Evidence.open(QIODevice::WriteOnly));
        Evidence.write(QJsonDocument(QJsonObject{{"windowId",qint64(Window.winId())},{"pid",QCoreApplication::applicationPid()},{"nativeFocusIsShell",Gui.hwndFocus==Helper}}).toJson());Evidence.close();
        QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["dirty"].toBool();},90000));
        const auto Save=Surface.SendDocument({{"action","save"},{"documentId",Id}});QVERIFY(Save.HasValue());
        QVERIFY(Wait([&]{return Surface.Details().commandSequence_>=Save.Value()&&!Doc(Surface.Details().documentState_,Id)["dirty"].toBool();}));
        const auto Saved=Read(Path);QVERIFY(Saved.contains('z')||Saved.contains('Z'));
        QVERIFY(Evidence.open(QIODevice::WriteOnly));Evidence.write(QJsonDocument(QJsonObject{{"keyboardAndSave",true},{"bytes",Saved.size()},{"nativeFocusIsShell",Gui.hwndFocus==Helper}}).toJson());Evidence.close();
    }
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();}));
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
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["lifecycle"].toString()=="ready";}));
    auto Send=[&](QJsonObject Request){auto Result=Surface.SendDocument(Request);if(!Result.HasValue())return false;const auto Sequence=Result.Value();return Wait([&]{return Surface.Details().commandSequence_>=Sequence;})&&!Surface.Details().commandError_;};
    QVERIFY(Send({{"action","text.replaceAll"},{"documentId",Id},{"text","old"},{"replacement","new"}}));QVERIFY(Send({{"action","save"},{"documentId",Id}}));
    Bytes.replace(0,3,"new");QVERIFY2(Wait([&]{const auto State=Doc(Surface.Details().documentState_,Id);return !State["dirty"].toBool()&&!State["saving"].toBool();}),qPrintable(QJsonDocument(Surface.Details().documentState_).toJson(QJsonDocument::Compact)+" viewerError="+QString::fromStdString(Surface.Snapshot().error_?Surface.Snapshot().error_->code_:std::string{}).toUtf8()));QCOMPARE(Read(Large),Bytes);
    // This path runs beneath the HC isolation job, not just the SDK DLL fixture.
    Surface.activateWindow();SetForegroundWindow(reinterpret_cast<HWND>(Surface.winId()));
    QVERIFY(Wait([&]{return GetForegroundWindow()==reinterpret_cast<HWND>(Surface.winId());}));
    SetActiveWindow(reinterpret_cast<HWND>(Surface.winId()));Surface.clearFocus();Surface.setFocus();
    QVERIFY(Send({{"action","focus"},{"documentId",Id}}));
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["view"].toObject()["inputFocused"].toBool();}));
    bool WindowValid=false;const auto ViewerValue=Surface.Details().child_.toULongLong(&WindowValid,16);QVERIFY(WindowValid);
    const auto Helper=GetWindow(reinterpret_cast<HWND>(quintptr(ViewerValue)),GW_CHILD);QVERIFY(Helper);
    RECT ChildBounds{};const auto Sdk=reinterpret_cast<HWND>(quintptr(ViewerValue));
    QVERIFY(Wait([&]{GetWindowRect(Helper,&ChildBounds);MapWindowPoints(nullptr,Sdk,reinterpret_cast<POINT*>(&ChildBounds),2);return ChildBounds.left==0&&ChildBounds.top==0;}));
    const auto HelperThread=GetWindowThreadProcessId(Helper,nullptr);
    GUITHREADINFO Gui{sizeof(GUITHREADINFO)};
    QVERIFY(Wait([&]{return GetGUIThreadInfo(HelperThread,&Gui)&&(Gui.hwndFocus==Helper||IsChild(Helper,Gui.hwndFocus));},5000));
    // Qt's Windows key mapper expects the key-down/translated-character path;
    // injecting only WM_CHAR is not equivalent to native keyboard input.
    INPUT Keys[2]{}; Keys[0].type=Keys[1].type=INPUT_KEYBOARD;
    Keys[0].ki.wVk=Keys[1].ki.wVk='Z'; Keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
    QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2));
    QVERIFY(Wait([&]{return Doc(Surface.Details().documentState_,Id)["dirty"].toBool();}));
    QVERIFY(Send({{"action","save"},{"documentId",Id}}));
    QVERIFY(Wait([&]{const auto State=Doc(Surface.Details().documentState_,Id);return !State["dirty"].toBool()&&!State["saving"].toBool();}));
    QVERIFY(Read(Large).contains('z'));
    QVERIFY(Send({{"action","replaceText"},{"documentId",Id},{"text","clipboard text"}}));QVERIFY(Send({{"action","selectAll"},{"documentId",Id}}));
    QVERIFY(Send({{"action","clipboard"},{"documentId",Id},{"mode","copy"}}));QVERIFY(Wait([&]{return QApplication::clipboard()->text()=="clipboard text";}));
    QApplication::clipboard()->setText("pasted text");QVERIFY(Send({{"action","clipboard"},{"documentId",Id},{"mode","paste"}}));
    QVERIFY(Wait([&]{return Surface.Details().documentState_["clipboard"].toObject().isEmpty();}));
    QVERIFY(Send({{"action","save"},{"documentId",Id}}));QVERIFY(Wait([&]{return !Doc(Surface.Details().documentState_,Id)["dirty"].toBool()&&!Doc(Surface.Details().documentState_,Id)["saving"].toBool();}));QCOMPARE(Read(Large),QByteArray("pasted text"));
    const auto Pdf=Root.filePath("doc.pdf"),Image=Root.filePath("image.png");QVERIFY(QFile::copy(Fixture("basic.pdf"),Pdf));QVERIFY(QFile::copy(Fixture("alpha.png"),Image));
    auto Added=Surface.OpenDocuments({{Content(Pdf),{}},{Content(Image),{}}});QVERIFY(Added.HasValue());
    QVERIFY(Wait([&]{return Surface.Details().documentState_["documents"].toArray().size()==3&&Surface.Details().documentState_["idle"].toBool();}));
    QCOMPARE(Live->load(),3);
    auto State=Surface.Details().documentState_;QString ImageId;for(const auto& Item:State["documents"].toArray())if(Item.toObject()["view"].toObject()["viewId"].toString()=="image")ImageId=Item.toObject()["documentId"].toString();
    QVERIFY(Send({{"action","clipboard"},{"documentId",ImageId},{"mode","copy"}}));QVERIFY(Wait([&]{return !QApplication::clipboard()->image().isNull();}));
    QVERIFY(Send({{"action","close"},{"documentId",ImageId},{"discard",true}}));QVERIFY(Wait([&]{return Live->load()==2;}));
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();}));QCOMPARE(Live->load(),0);
}
void helperCrashAndTimeout_data(){QTest::addColumn<bool>("Hang");QTest::newRow("crash")<<false;QTest::newRow("timeout")<<true;}
void helperCrashAndTimeout(){
    QFETCH(bool,Hang);Native Dll;QVERIFY(Wait([&]{return Dll.Call({{"action","state"}}).contains("documents");}));const auto Pid=Dll.Helper();QVERIFY(Pid&&Pid!=GetCurrentProcessId());
    auto Process=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE|PROCESS_SUSPEND_RESUME,FALSE,Pid);QVERIFY(Process);auto Cleanup=qScopeGuard([&]{CloseHandle(Process);});
    if(Hang){using Fn=LONG(WINAPI*)(HANDLE);auto Suspend=reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSuspendProcess"));QVERIFY(Suspend);QCOMPARE(Suspend(Process),LONG(0));}
    else QVERIFY(TerminateProcess(Process,0xe0000300));
    QElapsedTimer Timer;Timer.start();QVERIFY(Wait([&]{return Dll.Call({{"action","state"}}).contains("error");},16000));QVERIFY(Timer.elapsed()<17000);
    QVERIFY(Wait([&]{return WaitForSingleObject(Process,0)==WAIT_OBJECT_0;},6000));
}
void sdkReplacementAndForcedParentExit(){
    QTemporaryDir Root;const auto Report=Root.filePath("parent.json");QProcess Parent;Parent.start(QCoreApplication::applicationFilePath(),{"--parent-probe",Report});
    QVERIFY(Wait([&]{return QFile::exists(Report);},20000));auto State=QJsonDocument::fromJson(Read(Report)).object();const auto Pid=State["helperPid"].toInt();QVERIFY(Pid);
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
        Native Dll;QTemporaryDir Root;const auto Path=Root.filePath("parent.txt");Put(Path,"parent");
        if(!Wait([&]{return Dll.Call({{"action","state"}}).contains("documents");}))return 2;
        auto Id=Dll.Call({{"action","openHost"},{"path",Path}})["documentId"].toString();
        if(!Wait([&]{return Doc(Dll.Call({{"action","state"}}),Id)["lifecycle"].toString()=="ready";}))return 3;
        Dll.Call({{"action","replaceText"},{"documentId",Id},{"text","parent crash recovery"}});
        Dll.Call({{"action","checkpoint"}});auto State=Dll.Call({{"action","state"}});
        if(!Wait([&]{return QFile::exists(State["recoveryRoot"].toString()+"/"+Id+".json");}))return 4;
        State["helperPid"]=int(Dll.Helper());State["documentId"]=Id;
        if(!Put(App.arguments().last(),QJsonDocument(State).toJson(QJsonDocument::Compact)))return 5;
        return App.exec();
    }
    DocumentP3Tests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstDocumentP3.moc"
