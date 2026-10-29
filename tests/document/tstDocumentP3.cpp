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
#include <QAbstractNativeEventFilter>
#include <QToolButton>
#include <QToolBar>
#include <QUuid>
#include "UniqueLibs/document/hwpOpenWindow.hpp"
#include "text/QTextView.h"
#include "text/ITextEditorBackend.h"
#include "text/FindReplaceWidget.h"
#include <QStackedLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabBar>
#include "pdf/QPDFView.h"
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
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/epubSourceEditor.hpp"
#include "UniqueLibs/document/documentHostBroker.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "UniqueLibs/qtPluginViewerSurface.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "pluginHost/dopusAbi.hpp"
#include "pluginHost/pipeChannel.hpp"
using namespace hc::document;
using namespace hc::storage;
using namespace hc::plugins;
namespace {
bool Foreground(HWND Window){
    // Give the test the same activation permission as a real title-bar click.
    // Merely calling SetForegroundWindow does not create last-input ownership.
    const bool WasTopmost=(GetWindowLongPtrW(Window,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
    SetWindowPos(Window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    RECT Bounds{};POINT Cursor{};GetCursorPos(&Cursor);if(!GetWindowRect(Window,&Bounds))return false;
    SetCursorPos(Bounds.left+std::min<LONG>(100,(Bounds.right-Bounds.left)/2),Bounds.top+12);
    INPUT Click[2]{};Click[0].type=Click[1].type=INPUT_MOUSE;Click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;Click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    SendInput(2,Click,sizeof(INPUT));QTest::qWait(30);SetCursorPos(Cursor.x,Cursor.y);
    if(!WasTopmost)SetWindowPos(Window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    return GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(Window,GA_ROOT);
}
bool NativeKey(WORD Key,bool Control=false){
    std::vector<INPUT> Keys;const auto Add=[&](WORD Value,bool Up){INPUT I{};I.type=INPUT_KEYBOARD;I.ki.wVk=Value;I.ki.dwFlags=(Up?KEYEVENTF_KEYUP:0)|((Value>=VK_PRIOR&&Value<=VK_DOWN)?KEYEVENTF_EXTENDEDKEY:0);Keys.push_back(I);};
    if(Control)Add(VK_CONTROL,false);Add(Key,false);Add(Key,true);if(Control)Add(VK_CONTROL,true);
    return SendInput(UINT(Keys.size()),Keys.data(),sizeof(INPUT))==Keys.size();
}
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
void statusBarMeasuredLayout(){
    DocumentStatusBar Bar;Bar.resize(1400,32);Bar.show();
    QList<DocumentStatusField> Fields{{"position",QString::fromUtf8("줄 88 / 109 · 열 6"),"88:6 / 109",{},"text.goToLine"},{"encoding","UTF-8 (BOM)","UTF-8 BOM"},{"eol","CRLF"}};
    for(int I=0;I<8;++I)Fields.append(DocumentStatusField{QString("extra%1").arg(I),QString("metadata %1 with long text").arg(I),{},{},{},false});
    Fields.append(DocumentStatusField{"mode",QString::fromUtf8("편집"),{},{},{},true,true});
    Fields.append(DocumentStatusField{"state",QString::fromUtf8("저장 실패 · 미저장"),{},{},{},true,true});
    Bar.SetFields(Fields);QCoreApplication::processEvents();
    auto VisibleExtras=[&]{int Count=0;for(int I=0;I<8;++I)if(!Bar.findChild<QWidget*>(QString("status.extra%1").arg(I))->isHidden())++Count;return Count;};
    const auto Wide=VisibleExtras();QVERIFY(Wide>0);
    Bar.resize(640,Bar.height());QCoreApplication::processEvents();QVERIFY(VisibleExtras()<Wide);
    for(const auto& Field:Fields)if(Field.essential_){auto* Item=Bar.findChild<QWidget*>("status."+Field.id_);QVERIFY(!Item->isHidden());QVERIFY(Bar.rect().contains(Item->geometry()));}
    QString Command;Bar.activated_=[&](const QString& Id){Command=Id;};Bar.findChild<QToolButton*>("status.position")->click();QCOMPARE(Command,QString("text.goToLine"));
    Bar.resize(1400,Bar.height());QCoreApplication::processEvents();QCOMPARE(VisibleExtras(),Wide);
    Bar.grab().save(QCoreApplication::applicationDirPath()+"/statusbar-layout.png");
}
void statusBarWorkspace(){
    QTemporaryDir Root;const auto Path=Root.filePath("status.py");QVERIFY(Put(Path,QByteArray::fromHex("efbbbf")+"alpha\r\nbeta\r\n"));
    DocumentWorkspaceWidget Viewer(Root.filePath("recovery"));Viewer.resize(960,600);Viewer.show();
    Viewer.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
    const auto Id=Viewer.Handle({{"action","open"},{"path",Path}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Id)["lifecycle"]=="ready";}));
    auto* Bar=static_cast<DocumentStatusBar*>(Viewer.findChild<QWidget*>("documentStatusBar"));QVERIFY(Bar);
    auto Text=[&](const QString& Key){for(const auto& Field:Bar->Fields())if(Field.id_==Key)return Field.text_;return QString{};};
    QTRY_VERIFY(Text("encoding").contains("BOM"));QCOMPARE(Text("eol"),QString("CRLF"));
    auto* Pane=Viewer.findChild<DocumentViewPane*>();QVERIFY(Pane);auto* Input=Pane->findChild<QTextView*>();QVERIFY(Input);
    Input->goToPosition(2,3);QTRY_VERIFY(Text("position").contains(QString::fromUtf8("줄 2")));
    Pane->Execute({{"action","replaceText"},{"text","changed\r\n"}});QTRY_VERIFY(Text("state").contains(QString::fromUtf8("미저장")));
    Input->setLineEnding(QTextView::LF);QTRY_COMPARE(Text("eol"),QString("LF"));
    QTRY_VERIFY(Text("position").contains("/ 2"));
    Viewer.ShowShellError("status-test-error");QTRY_VERIFY(Text("state").contains(QString::fromUtf8("오류")));QVERIFY(Text("state").contains(QString::fromUtf8("미저장")));QVERIFY(!Text("position").isEmpty());
    Viewer.grab().save(QCoreApplication::applicationDirPath()+"/statusbar-workspace.png");
    Viewer.RequestClose();QVERIFY(Wait([&]{return Viewer.Snapshot()["documents"].toArray().isEmpty();}));
}
void statusBarEpubSource(){
    const auto Path=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Path.isEmpty())QSKIP("EPUB fixture required");QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));
    QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());EpubSourceEditor Editor(File.readAll(),Path);Editor.SetRecoveryEnabled(false);Editor.resize(960,640);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);
    QString Chapter;for(const auto& Value:Editor.Snapshot()["resources"].toArray()){const auto Row=Value.toObject();if(Row["mediaType"]=="application/xhtml+xml"){Chapter=Row["path"].toString();break;}}
    QVERIFY(!Chapter.isEmpty());QVERIFY(Editor.Execute({{"action","epub.edit.select"},{"resource",Chapter}})["accepted"].toBool());Editor.SetSourceVisible(true);
    QSignalSpy Changed(&Editor,&EpubSourceEditor::StatusChanged);QVERIFY(Editor.Execute({{"action","epub.edit.cursor"},{"position",0},{"anchor",0}})["accepted"].toBool());QVERIFY(Changed.count()>0);
    auto Fields=Editor.StatusFields();auto Find=[&](const QString& Id){for(const auto& Field:Fields)if(Field.id_==Id)return Field.text_;return QString{};};
    QVERIFY(Find("position").contains(QString::fromUtf8("줄 1")));QCOMPARE(Find("encoding"),QString("UTF-8"));QCOMPARE(Find("resource"),Chapter);QVERIFY(!Find("validation").isEmpty());
    Editor.SetSourceVisible(false);Fields=Editor.StatusFields();QVERIFY(Find("position").isEmpty());QVERIFY(!Find("problems").isEmpty());
}
void statusBarPdfAndImage(){
    auto Pdf=ReadDocumentFile(Fixture("basic.pdf"));QVERIFY(Pdf.HasValue());DocumentViewPane Pane;QVERIFY(Pane.Load(Pdf.Value(),false));
    auto Fields=Pane.StatusFields();auto Text=[&](const QString& Key){for(const auto& Field:Fields)if(Field.id_==Key)return Field.text_;return QString{};};
    QVERIFY(Text("page").contains("1 / 2"));Pane.Execute({{"action","pdf.page"},{"page",1}});Fields=Pane.StatusFields();QVERIFY(Text("page").contains("2 / 2"));
    TextInput Input;Input.viewId_="image";Input.path_="sample.png";Input.image_=QImage(320,200,QImage::Format_ARGB32);Input.image_.fill(Qt::blue);DocumentViewPane Image;QVERIFY(Image.Load(Input,false));Fields=Image.StatusFields();QVERIFY(Text("dimensions").contains("320"));QVERIFY(!Text("zoom").isEmpty());
}

void workspaceReactivation_data(){
    QTest::addColumn<bool>("Hosted");QTest::addColumn<QString>("Kind");QTest::addColumn<bool>("Minimize");
    for(bool Hosted:{false,true})for(const auto& Kind:{QString("text"),QString("pdf"),QString("image"),QString("hwp"),QString("hwpx")})for(bool Minimize:{false,true})
        QTest::newRow(qPrintable(QString("%1-%2-%3").arg(Hosted?"viewer":"editor",Kind,Minimize?"minimize":"switch")))<<Hosted<<Kind<<Minimize;
}
void workspaceReactivation(){
    QFETCH(bool,Hosted);QFETCH(QString,Kind);QFETCH(bool,Minimize);QTemporaryDir Root;
    const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");if((Kind=="hwp"||Kind=="hwpx")&&Corpus.isEmpty())QSKIP("RHWP corpus required");
    QString Path=Root.filePath("input.txt");QVERIFY(Put(Path,"body alpha"));
    if(Kind=="pdf")Path=Fixture("basic.pdf");
    if(Kind=="image"){for(int I=0;I<3;++I){QImage Image(80,80,QImage::Format_RGB32);Image.fill(QColor(30+I*60,70,100));QVERIFY(Image.save(Root.filePath(QString("image%1.png").arg(I))));}Path=Root.filePath("image0.png");}
    if(Kind=="hwp"||Kind=="hwpx")Path=Corpus+(Kind=="hwp"?"/c02.hwp":"/c07.hwpx");
    DocumentWorkspaceWidget Viewer(Root.filePath("recovery"),Hosted);Viewer.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
    if(Kind=="hwp"||Kind=="hwpx"){QFile Inventory(QCoreApplication::applicationDirPath()+"/runtime-inventory.json");QVERIFY(Inventory.open(QIODevice::ReadOnly));Viewer.ConfigureHwp(MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),QCryptographicHash::hash(Inventory.readAll(),QCryptographicHash::Sha256).toHex()));}
    Viewer.resize(900,700);Viewer.show();const auto Window=reinterpret_cast<HWND>(Viewer.winId());Foreground(Window);QTRY_COMPARE(GetForegroundWindow(),Window);
    const auto Id=Viewer.Handle({{"action","open"},{"path",Path}})["documentId"].toString();QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Id)["lifecycle"]=="ready";},45000));
    auto* Pane=Viewer.findChild<DocumentViewPane*>();QVERIFY(Pane);
    auto* ToolbarField=new QLineEdit;Viewer.findChild<QToolBar*>("mainToolbar")->addWidget(ToolbarField);
    int Acks=0;if(auto* Hwp=Viewer.findChild<HwpOpenWindow*>())for(auto* W:Hwp->findChildren<QWidget*>())if(auto* B=dynamic_cast<HwpProcessBackend*>(W))
        connect(B,&HwpSdiBackend::EditorReply,&Viewer,[&](quint64,QJsonObject R){if(R["requestType"]=="DocumentNavigate"&&R["ok"].toBool())++Acks;});
    QProcess Peer;Peer.start(QCoreApplication::applicationFilePath(),{"--focus-peer"});QVERIFY(Wait([&]{return Peer.canReadLine();}));
    const auto Other=reinterpret_cast<HWND>(Peer.readLine().trimmed().toULongLong());QVERIFY(IsWindow(Other));
    const auto Cleanup=qScopeGuard([&]{SendMessageW(Other,WM_APP+117,reinterpret_cast<WPARAM>(Window),0);Peer.kill();Peer.waitForFinished();Viewer.RequestClose();Wait([&]{return Viewer.Snapshot()["documents"].toArray().isEmpty();},30000);});
    ShowWindow(Other,SW_SHOWNOACTIVATE);Foreground(Window);QTRY_COMPARE(GetForegroundWindow(),Window);
    for(int Cycle=0;Cycle<2;++Cycle){
        QTest::qWait(50);ToolbarField->setFocus();QCOMPARE(QApplication::focusWidget(),ToolbarField);
        QVERIFY(AllowSetForegroundWindow(DWORD(Peer.processId())));
        if(Minimize)Viewer.showMinimized();SendMessageW(Other,WM_APP+117,reinterpret_cast<WPARAM>(Other),0);QTRY_COMPARE(GetForegroundWindow(),Other);
        ToolbarField->setFocus(Qt::ActiveWindowFocusReason);QTest::qWait(60);QCOMPARE(GetForegroundWindow(),Other);
        if(Minimize)ShowWindow(Window,SW_RESTORE);SendMessageW(Other,WM_APP+117,reinterpret_cast<WPARAM>(Window),0);QTRY_COMPARE(GetForegroundWindow(),Window);
        QWidget* Input=Pane->InputWidget();while(Input&&Input->focusProxy())Input=Input->focusProxy();QVERIFY(Input);QTRY_COMPARE(QApplication::focusWidget(),Input);
        QVERIFY(GetFocus()&&(GetFocus()==Window||IsChild(Window,GetFocus())));
        if(Kind=="text"){QVERIFY(NativeKey('X'));auto* Text=Pane->findChild<QTextView*>();QVERIFY(Text);QTRY_COMPARE(Text->Content().count('x'),Cycle+1);}
        else if(Kind=="pdf"){QVERIFY(NativeKey(Cycle?VK_HOME:VK_END));QTRY_COMPARE(Pane->ViewState()["page"].toInt(),Cycle?0:1);}
        else if(Kind=="image"){QVERIFY(NativeKey(VK_SPACE));QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Id)["path"].toString()==Root.filePath(QString("image%1.png").arg(Cycle+1))&&Viewer.IsIdle();}));}
        else {QVERIFY(NativeKey(Cycle?VK_HOME:VK_NEXT));QTRY_COMPARE_WITH_TIMEOUT(Acks,Cycle+1,10000);}
        QVERIFY(ToolbarField->text().isEmpty());
    }
}
void nativeHwpEditorReactivation_data(){QTest::addColumn<QString>("Name");QTest::newRow("hwp")<<QString("c02.hwp");QTest::newRow("hwpx")<<QString("c05.hwpx");}
void nativeHwpEditorReactivation(){
    QFETCH(QString,Name);const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");if(Corpus.isEmpty())QSKIP("RHWP corpus required");
    QTemporaryDir Root;const auto Original=Read(Corpus+'/'+Name);const auto FileName="focus-"+QUuid::createUuid().toString(QUuid::Id128)+'.'+QFileInfo(Name).suffix();
    const auto Path=Root.filePath(FileName);QVERIFY(Put(Path,Original));
    QProcess Editor;Editor.start(QCoreApplication::applicationDirPath()+"/HyperEditor.exe",{"--rhwp-edit","--config-dir",Root.filePath("config"),"--",Path});QVERIFY(Editor.waitForStarted());
    struct Search{QString name_;DWORD pid_;HWND window_=nullptr;} Target{FileName,DWORD(Editor.processId())};
    const auto Title=[&]{wchar_t Text[1024]{};if(Target.window_)GetWindowTextW(Target.window_,Text,1024);return QString::fromWCharArray(Text);};
    const auto Stop=qScopeGuard([&]{if(Target.window_)PostMessageW(Target.window_,WM_CLOSE,0,0);if(!Editor.waitForFinished(2000)){Editor.kill();Editor.waitForFinished();}});
    QVERIFY(Wait([&]{EnumWindows([](HWND W,LPARAM P)->BOOL{auto& S=*reinterpret_cast<Search*>(P);DWORD Pid=0;GetWindowThreadProcessId(W,&Pid);wchar_t Text[1024]{};GetWindowTextW(W,Text,1024);if(Pid==S.pid_&&QString::fromWCharArray(Text).startsWith(S.name_)){S.window_=W;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&Target));return Target.window_&&Title().contains(QString::fromUtf8("한글 문서 편집"));},45000));
    QProcess Peer;Peer.start(QCoreApplication::applicationFilePath(),{"--focus-peer"});QVERIFY(Wait([&]{return Peer.canReadLine();}));const auto Other=reinterpret_cast<HWND>(Peer.readLine().trimmed().toULongLong());QVERIFY(IsWindow(Other));
    const auto StopPeer=qScopeGuard([&]{Peer.kill();Peer.waitForFinished();});
    ShowWindow(Other,SW_SHOWNOACTIVATE);ShowWindow(Target.window_,SW_RESTORE);Foreground(Target.window_);
    QVERIFY2(Wait([&]{return GetAncestor(GetForegroundWindow(),GA_ROOT)==Target.window_;},5000),qPrintable(QString("editor foreground=%1 expected=%2 visible=%3").arg(quintptr(GetForegroundWindow())).arg(quintptr(Target.window_)).arg(IsWindowVisible(Target.window_))));
    for(int Cycle=0;Cycle<2;++Cycle){
        const auto Before=QCryptographicHash::hash(Read(Path),QCryptographicHash::Sha256);
        if(Cycle)ShowWindow(Target.window_,SW_MINIMIZE);Foreground(Other);QTRY_COMPARE(GetForegroundWindow(),Other);
        QTest::qWait(60);QCOMPARE(GetForegroundWindow(),Other);if(Cycle)ShowWindow(Target.window_,SW_RESTORE);
        SendMessageW(Other,WM_APP+117,reinterpret_cast<WPARAM>(Target.window_),0);QTRY_COMPARE(GetAncestor(GetForegroundWindow(),GA_ROOT),Target.window_);
        const auto Thread=GetWindowThreadProcessId(Target.window_,nullptr);
        QVERIFY(Wait([&]{GUITHREADINFO Info{sizeof(GUITHREADINFO)};DWORD Pid=0;if(!GetGUIThreadInfo(Thread,&Info)||!Info.hwndFocus||!IsChild(Target.window_,Info.hwndFocus))return false;GetWindowThreadProcessId(Info.hwndFocus,&Pid);return Pid!=Target.pid_;},10000));
        QVERIFY(NativeKey('X'));QVERIFY(Wait([&]{return Title().contains('*');},10000));
        QVERIFY(NativeKey('S',true));QVERIFY(Wait([&]{return !Title().contains('*')&&QCryptographicHash::hash(Read(Path),QCryptographicHash::Sha256)!=Before;},30000));
    }
    QCOMPARE(Read(Corpus+'/'+Name),Original);QTest::qWait(200);PostMessageW(Target.window_,WM_CLOSE,0,0);
    QVERIFY2(Wait([&]{return Editor.state()==QProcess::NotRunning;},20000),qPrintable(QString("close title=%1 visible=%2 stderr=%3").arg(Title()).arg(IsWindowVisible(Target.window_)).arg(QString::fromUtf8(Editor.readAllStandardError()))));
}
void pdfNavigationKeys_data(){
    QTest::addColumn<bool>("Hosted");QTest::addColumn<int>("Target");
    for(bool Hosted:{false,true})for(int Target=0;Target<4;++Target)
        QTest::newRow(qPrintable(QString("%1-%2").arg(Hosted?"viewer":"editor").arg(Target)))<<Hosted<<Target;
}
void pdfNavigationKeys(){
    QFETCH(bool,Hosted);QFETCH(int,Target);QTemporaryDir Root;
    DocumentWorkspaceWidget Viewer(Root.path(),Hosted);Viewer.resize(900,650);Viewer.show();
    const auto Id=Viewer.Handle({{"action","open"},{"path",Fixture("basic.pdf")}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Id)["lifecycle"]=="ready";}));
    auto* Pdf=Viewer.findChild<QPDFView*>();QVERIFY(Pdf);QCOMPARE(Pdf->totalPageCount(),2);
    auto* Scroll=Pdf->findChild<QScrollArea*>("canvasScrollArea");QVERIFY(Scroll);
    QWidget* Input=Target==0?static_cast<QWidget*>(Pdf):Target==1?Scroll->viewport():Target==2?static_cast<QWidget*>(Scroll):Scroll->verticalScrollBar();
    QCOMPARE(Pdf->focusPolicy(),Qt::StrongFocus);QCOMPARE(Scroll->focusProxy(),Pdf);
    for(const auto Step:{std::pair{Qt::Key_PageDown,1},std::pair{Qt::Key_PageUp,0},std::pair{Qt::Key_End,1},std::pair{Qt::Key_Home,0},std::pair{Qt::Key_PageUp,0}}){
        QTest::keyClick(Input,Step.first);QTRY_COMPARE(Pdf->currentPage(),Step.second);
    }
    QTest::keyClick(Input,Qt::Key_End,Qt::ControlModifier);QTRY_COMPARE(Pdf->currentPage(),1);
    QTest::keyClick(Input,Qt::Key_Home,Qt::ControlModifier);QTRY_COMPARE(Pdf->currentPage(),0);
    QTest::keyClick(Input,Qt::Key_End,Qt::KeypadModifier);QTRY_COMPARE(Pdf->currentPage(),1);QTest::keyClick(Input,Qt::Key_Home,Qt::KeypadModifier);QTRY_COMPARE(Pdf->currentPage(),0);
    Pdf->setZoomLevel(200);QTRY_VERIFY(Scroll->verticalScrollBar()->maximum()>0);
    Scroll->verticalScrollBar()->setValue(0);QTest::keyClick(Input,Qt::Key_Down);QTRY_VERIFY(Scroll->verticalScrollBar()->value()>0);
    QTest::keyClick(Input,Qt::Key_Up);QTRY_COMPARE(Scroll->verticalScrollBar()->value(),0);
    auto* PageBox=Viewer.findChild<QSpinBox*>();QVERIFY(PageBox);auto* Field=PageBox->findChild<QLineEdit*>();QVERIFY(Field);
    Pdf->goToPage(1);QTest::keyClick(Field,Qt::Key_Home);QCOMPARE(Pdf->currentPage(),1);QCOMPARE(Field->cursorPosition(),0);
    QTest::keyClick(Field,Qt::Key_End);QCOMPARE(Pdf->currentPage(),1);QCOMPARE(Field->cursorPosition(),Field->text().size());
}
void hwpNavigationKeys_data(){QTest::addColumn<QString>("Name");QTest::newRow("hwp")<<QString("c02.hwp");QTest::newRow("hwpx")<<QString("c07.hwpx");}
void pdfTabNavigationFocus(){
    QTemporaryDir Root;const auto SecondPath=Root.filePath("second.pdf");QVERIFY(QFile::copy(Fixture("basic.pdf"),SecondPath));
    DocumentWorkspaceWidget Viewer(Root.path(),true);Viewer.show();QApplication::setActiveWindow(&Viewer);
    const auto First=Viewer.Handle({{"action","open"},{"path",Fixture("basic.pdf")}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),First)["lifecycle"]=="ready";}));
    auto* Pdf=Viewer.findChild<QPDFView*>();QVERIFY(Pdf);
    const auto Second=Viewer.Handle({{"action","open"},{"path",SecondPath}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Second)["lifecycle"]=="ready";}));
    auto* Tabs=Viewer.findChild<QTabWidget*>("documentTabs");QVERIFY(Tabs);QCOMPARE(Tabs->count(),2);
    QTest::mouseClick(Tabs->tabBar(),Qt::LeftButton,Qt::NoModifier,Tabs->tabBar()->tabRect(0).center());
    QTRY_VERIFY(Pdf->hasFocus());QTest::keyClick(QApplication::focusWidget(),Qt::Key_End);QTRY_COMPARE(Pdf->currentPage(),1);
    QCOMPARE(Doc(Viewer.Snapshot(),Second)["view"].toObject()["page"].toInt(),0);
}
void hwpNavigationKeys(){
    QFETCH(QString,Name);const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");if(Corpus.isEmpty())QSKIP("Pinned RHWP corpus required");
    QTemporaryDir Root;QFile Inventory(QCoreApplication::applicationDirPath()+"/runtime-inventory.json");QVERIFY(Inventory.open(QIODevice::ReadOnly));
    DocumentWorkspaceWidget Viewer(Root.path(),true);Viewer.ConfigureHwp(MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),QCryptographicHash::hash(Inventory.readAll(),QCryptographicHash::Sha256).toHex()));
    Viewer.show();Foreground(reinterpret_cast<HWND>(Viewer.winId()));QApplication::setActiveWindow(&Viewer);
    const auto Pdf=Viewer.Handle({{"action","open"},{"path",Fixture("basic.pdf")}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Pdf)["lifecycle"]=="ready";}));
    const auto Path=Corpus+'/'+Name;const auto Before=Read(Path);
    const auto Id=Viewer.Handle({{"action","open"},{"path",Path}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Id)["lifecycle"]=="ready";},45000));
    auto* Window=Viewer.findChild<HwpOpenWindow*>();QVERIFY(Window);QCOMPARE(Window->focusProxy(),Window->Document());
    HwpProcessBackend* Backend=nullptr;for(auto* W:Window->findChildren<QWidget*>())if(auto* B=dynamic_cast<HwpProcessBackend*>(W))Backend=B;
    QVERIFY(Backend);QCOMPARE(Window->Document()->focusProxy(),Backend);QCOMPARE(Backend->focusPolicy(),Qt::StrongFocus);
    auto* Tabs=Viewer.findChild<QTabWidget*>("documentTabs");QVERIFY(Tabs);
    Tabs->setCurrentIndex(0);Tabs->setCurrentIndex(1);QTRY_VERIFY(Backend->hasFocus());
    int Acks=0;connect(Backend,&HwpSdiBackend::EditorReply,&Viewer,[&](quint64,QJsonObject Reply){if(Reply["requestType"]=="DocumentNavigate"&&Reply["ok"].toBool())++Acks;});
    for(const auto Key:{Qt::Key_PageDown,Qt::Key_PageUp,Qt::Key_End,Qt::Key_Home,Qt::Key_Down,Qt::Key_Up})QTest::keyClick(QApplication::focusWidget(),Key);
    QTRY_COMPARE_WITH_TIMEOUT(Acks,6,10000);
    QTest::keyClick(Backend,Qt::Key_End,Qt::ControlModifier);QTest::keyClick(Backend,Qt::Key_Home,Qt::ControlModifier);QTRY_COMPARE_WITH_TIMEOUT(Acks,8,10000);
    QTest::keyClick(Backend,Qt::Key_End,Qt::KeypadModifier);QTest::keyClick(Backend,Qt::Key_Home,Qt::KeypadModifier);QTRY_COMPARE_WITH_TIMEOUT(Acks,10,10000);
    QVERIFY(!Doc(Viewer.Snapshot(),Id)["dirty"].toBool());QCOMPARE(Read(Path),Before);
    Viewer.RequestClose();QVERIFY(Wait([&]{return Viewer.Snapshot()["documents"].toArray().isEmpty()&&HwpProcessBackend::IsDrained();},30000));
}
void viewerUnboundedTabsAndFiles(){
    QTemporaryDir Root;DocumentWorkspaceWidget Viewer(Root.filePath("recovery"),true);QStringList Ids;
    for(int Index=0;Index<20;++Index){
        const auto Path=Root.filePath(QString::number(Index)+".txt");QVERIFY(Put(Path,"tab\n"));
        const auto Reply=Viewer.Handle({{"action","open"},{"path",Path}});QVERIFY2(!Reply.contains("error"),qPrintable(Reply["error"].toString()));Ids<<Reply["documentId"].toString();
    }
    QVERIFY(Wait([&]{return Viewer.IsIdle();},30000));QCOMPARE(Viewer.Snapshot()["documents"].toArray().size(),20);
    for(const auto& Id:Ids)QCOMPARE(Doc(Viewer.Snapshot(),Id)["lifecycle"].toString(),QString("ready"));
    const auto TextPath=Root.filePath("large.txt");QByteArray Text;Text.reserve(17*1024*1024);while(Text.size()<=16*1024*1024)Text+="line with bounded paragraph layout 0123456789\r\n";
    QVERIFY(Put(TextPath,Text));const auto TextId=Viewer.Handle({{"action","open"},{"path",TextPath}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),TextId)["lifecycle"]!="opening";},45000));QCOMPARE(Doc(Viewer.Snapshot(),TextId)["lifecycle"].toString(),QString("ready"));
    const auto Original=Read(Fixture("basic.pdf"));QVERIFY(Original.contains("startxref"));
    auto Pdf=Original;Pdf+=QByteArray(65*1024*1024,' ');Pdf+='\n';Pdf+=Original.mid(Original.lastIndexOf("startxref"));
    const auto PdfPath=Root.filePath("large.pdf");QVERIFY(Put(PdfPath,Pdf));
    const auto PdfId=Viewer.Handle({{"action","open"},{"path",PdfPath}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),PdfId)["lifecycle"]!="opening";},45000));QCOMPARE(Doc(Viewer.Snapshot(),PdfId)["lifecycle"].toString(),QString("ready"));
    QCOMPARE(Doc(Viewer.Snapshot(),PdfId)["view"].toObject()["pageCount"].toInt(),2);
    Viewer.RequestClose();QVERIFY(Wait([&]{return Viewer.Snapshot()["documents"].toArray().isEmpty();},30000));
}
void mixedPdfHwpTabs(){
    const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
    if(Corpus.isEmpty())QSKIP("Pinned RHWP corpus required");
    QTemporaryDir Root;const auto Previous=qApp->property("documentViewsConfigDir");
    qApp->setProperty("documentViewsConfigDir",Root.path());
    const auto Restore=qScopeGuard([&]{qApp->setProperty("documentViewsConfigDir",Previous);});
    QFile Inventory(QCoreApplication::applicationDirPath()+"/runtime-inventory.json");QVERIFY(Inventory.open(QIODevice::ReadOnly));
    DocumentWorkspaceWidget Viewer(Root.filePath("recovery"),true);
    Viewer.ConfigureHwp(MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),QCryptographicHash::hash(Inventory.readAll(),QCryptographicHash::Sha256).toHex()));
    Viewer.show();
    const auto Pdf=Viewer.Handle({{"action","open"},{"path",Fixture("basic.pdf")}})["documentId"].toString();
    QVERIFY(Wait([&]{return Doc(Viewer.Snapshot(),Pdf)["lifecycle"]=="ready";}));
    auto* Tabs=Viewer.findChild<QTabWidget*>("documentTabs");auto* Edit=Viewer.findChild<QAction*>("editHwpDocument");
    QVERIFY(Tabs);QVERIFY(Edit);QVERIFY(!Edit->isVisible());QStringList Ids;
    for(const auto& Name:{QString("c02.hwp"),QString("c05.hwpx")}){
        const auto Path=Corpus+'/'+Name;const auto Before=Read(Path);QVERIFY(!Before.isEmpty());
        const auto Id=Viewer.Handle({{"action","open"},{"path",Path}})["documentId"].toString();QVERIFY(!Id.isEmpty());Ids<<Id;
        QVERIFY2(Wait([&]{return Doc(Viewer.Snapshot(),Id)["lifecycle"]!="opening";},45000),"RHWP load timed out");
        const auto State=Doc(Viewer.Snapshot(),Id);QVERIFY2(State["lifecycle"]=="ready",QJsonDocument(State).toJson().constData());
        QCOMPARE(Tabs->count(),int(Ids.size()+1));QVERIFY(Edit->isVisible());QVERIFY(Edit->isEnabled());
        QVERIFY(Viewer.findChild<QToolBar*>("mainToolbar")->actions().contains(Edit));
        QVERIFY(!State["canSave"].toBool());QVERIFY(!State["canSaveAs"].toBool());
        auto* Window=Tabs->currentWidget()->findChild<HwpOpenWindow*>();QVERIFY(Window);QVERIFY(!Window->isWindow());
        int Prints=0;Window->SetPrintDialogFactory([&](QPrinter&,QWidget*){++Prints;return false;});
        Viewer.findChild<QAction*>("printDocument")->trigger();QTRY_COMPARE(Prints,1);QVERIFY(!Doc(Viewer.Snapshot(),Id)["dirty"].toBool());
        Edit->trigger();const auto Request=Viewer.Snapshot()["editRequest"].toObject();QCOMPARE(Request["documentId"].toString(),Id);
        QVERIFY(!Viewer.Handle({{"action","editResult"},{"token",Request["token"]},{"accepted",true}}).contains("error"));
        QCOMPARE(Read(Path),Before);QCOMPARE(Viewer.Handle({{"action","saveAs"},{"documentId",Id},{"path",Root.filePath("forbidden.hwp")}})["error"].toString(),QString("document-edit-in-hypereditor"));
        QVERIFY(!QFile::exists(Root.filePath("forbidden.hwp")));
    }
    Viewer.Handle({{"action","activate"},{"documentId",Pdf}});QVERIFY(!Edit->isVisible());QCOMPARE(Doc(Viewer.Snapshot(),Pdf)["view"].toObject()["viewId"].toString(),QString("pdf"));
    Viewer.Handle({{"action","activate"},{"documentId",Ids.last()}});
    const auto Evidence=qEnvironmentVariable("HC_RHWP_TEST_EVIDENCE");if(!Evidence.isEmpty()){
        Viewer.raise();Viewer.activateWindow();QTest::qWait(300);
        const auto Top=Viewer.mapToGlobal(QPoint());
        QVERIFY(Viewer.screen()->grabWindow(0,Top.x(),Top.y(),Viewer.width(),Viewer.height()).save(Evidence+"/mixed-tabs-screen.png"));
    }
    Viewer.RequestClose();QVERIFY(Wait([&]{return Viewer.Snapshot()["documents"].toArray().isEmpty()&&HwpProcessBackend::IsDrained();},30000));
}
void hostedMixedHwpTabs(){
    const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");if(Corpus.isEmpty())QSKIP("Pinned RHWP corpus required");
    QtPluginViewerController Controller(nullptr,[] {return MakeNativeMaterializer(QDir::tempPath().toStdString());});
    QtPluginViewerSurface Surface(&Controller);Surface.resize(1000,760);Surface.show();Surface.Open(Options(),Content(Fixture("basic.pdf")));
    QVERIFY2(Wait([&]{return Surface.IsReady();},30000),qPrintable(Surface.Snapshot().error_?QString::fromStdString(Surface.Snapshot().error_->code_):QString("not-ready")));
    for(const auto& Name:{QString("c02.hwp"),QString("c05.hwpx")}){
        auto Sent=Surface.SendDocument({{"action","open"},{"path",Corpus+'/'+Name}});QVERIFY(Sent.HasValue());
        QVERIFY(Wait([&]{return Surface.Details().commandSequence_>=Sent.Value();},10000));
        QVERIFY2(!Surface.Details().commandError_,"Open command failed");
        const auto Ready=[&]{const auto S=Surface.Details().documentState_;const auto D=Doc(S,S["activeDocumentId"].toString());return D["lifecycle"]=="ready"||D["lifecycle"]=="failed";};
        QVERIFY(Wait(Ready,45000));const auto S=Surface.Details().documentState_;const auto D=Doc(S,S["activeDocumentId"].toString());
        QVERIFY2(D["lifecycle"]=="ready",QJsonDocument(D).toJson().constData());
    }
    QCOMPARE(Surface.Details().documentState_["documents"].toArray().size(),3);
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();},30000));
}
void hostedHwpEditLaunch(){
    const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");if(Corpus.isEmpty())QSKIP("Pinned RHWP corpus required");
    QTemporaryDir Root;const auto Name="edit-"+QUuid::createUuid().toString(QUuid::Id128)+".hwp";const auto Path=Root.filePath(Name);
    QVERIFY(QFile::copy(Corpus+"/c02.hwp",Path));const auto Before=Read(Path);
    QtPluginViewerController Controller(nullptr,[]{return MakeNativeMaterializer(QDir::tempPath().toStdString());});
    QtPluginViewerSurface Surface(&Controller);Surface.resize(1000,760);Surface.show();Surface.Open(Options(),Content(Path));
    QVERIFY2(Wait([&]{return Surface.IsReady();},45000),qPrintable(Surface.Snapshot().error_?QString::fromStdString(Surface.Snapshot().error_->code_):QString("not-ready")));
    const auto Id=Surface.Details().documentState_["activeDocumentId"].toString();
    QVERIFY(Surface.SendDocument({{"action","editHwp"},{"documentId",Id}}).HasValue());
    struct EditorSearch {QString name_;HWND window_=nullptr;DWORD pid_=0;} Editor{Name};
    const auto Find=[&]{EnumWindows([](HWND Window,LPARAM Data)->BOOL{
        auto* Search=reinterpret_cast<EditorSearch*>(Data);wchar_t Title[1024]{};GetWindowTextW(Window,Title,1024);
        const auto Text=QString::fromWCharArray(Title);
        if(Text.startsWith(Search->name_)&&Text.contains(QString::fromUtf8("한글 문서 편집"))){Search->window_=Window;GetWindowThreadProcessId(Window,&Search->pid_);return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&Editor));return Editor.window_!=nullptr;};
    const auto Cleanup=qScopeGuard([&]{if(Editor.window_&&IsWindow(Editor.window_))PostMessageW(Editor.window_,WM_CLOSE,0,0);});
    QVERIFY(Wait(Find,45000));HANDLE Process=OpenProcess(SYNCHRONIZE,FALSE,Editor.pid_);QVERIFY(Process);
    const auto Release=qScopeGuard([&]{CloseHandle(Process);});
    Surface.Close();QVERIFY(Wait([&]{return Controller.IsIdle();},30000));
    QCOMPARE(WaitForSingleObject(Process,0),DWORD(WAIT_TIMEOUT));QVERIFY(IsWindow(Editor.window_));
    PostMessageW(Editor.window_,WM_CLOSE,0,0);QVERIFY(Wait([&]{return WaitForSingleObject(Process,0)==WAIT_OBJECT_0;},30000));
    QCOMPARE(Read(Path),Before);
}
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
void textSearchEscapeTakesPriority_data(){
    QTest::addColumn<bool>("Hosted");
    QTest::addColumn<bool>("CloseOnEscape");
    QTest::addColumn<bool>("ReplaceMode");
    QTest::addColumn<int>("FocusTarget");
    for (const bool Hosted : {false, true})
        for (const bool CloseOnEscape : {false, true})
            for (const bool ReplaceMode : {false, true})
                for (int FocusTarget = 0; FocusTarget < 3; ++FocusTarget) {
                    const auto Name = QString("%1-close%2-replace%3-focus%4")
                        .arg(Hosted ? "viewer" : "editor").arg(CloseOnEscape).arg(ReplaceMode).arg(FocusTarget);
                    QTest::newRow(qPrintable(Name)) << Hosted << CloseOnEscape << ReplaceMode << FocusTarget;
                }
}
void textSearchEscapeTakesPriority(){
    QFETCH(bool, Hosted); QFETCH(bool, CloseOnEscape); QFETCH(bool, ReplaceMode); QFETCH(int, FocusTarget);
    QTemporaryDir Root; const auto Previous = qApp->property("documentViewsConfigDir");
    qApp->setProperty("documentViewsConfigDir", Root.path());
    const auto Restore = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); });
    DocumentSettingsDialog Settings(hc::settings::UnifiedConfig::FileName(Root.path()), {});
    auto* Option = Settings.findChild<QCheckBox*>("documentViews/common/closeViewerOnEscape");
    QVERIFY(Option); Option->setChecked(CloseOnEscape); QVERIFY(Settings.Apply());
    const auto Path = Root.filePath("search.txt"); const QByteArray Content("search text\nsearch again\n");
    QVERIFY(Put(Path, Content));
    DocumentWorkspaceWidget Window(Root.filePath("recovery"), Hosted);
    Window.show(); Window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Window));
    const auto Id = Window.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
    QVERIFY(!Id.isEmpty()); QTRY_COMPARE(Doc(Window.Snapshot(), Id)["lifecycle"].toString(), QString("ready"));
    auto* Text = Window.findChild<QTextView*>(); QVERIFY(Text);
    auto* Input = Text->Backend()->widget(); QVERIFY(Input); Input->setFocus();
    QTest::keyClick(Input, ReplaceMode ? Qt::Key_H : Qt::Key_F, Qt::ControlModifier);
    auto* Search = Text->findChild<FindReplaceWidget*>(); QVERIFY(Search);
    QVERIFY(Search->isVisible()); QCOMPARE(Search->isReplaceMode(), ReplaceMode);
    QWidget* Target = QApplication::focusWidget(); QVERIFY(qobject_cast<QLineEdit*>(Target));
    if (FocusTarget == 1) Target = Input;
    if (FocusTarget == 2) Target = Search->findChild<QToolButton*>();
    QVERIFY(Target); Target->setFocus(); QTRY_VERIFY(Target->hasFocus());
    QTest::keyClick(Target, Qt::Key_Escape);
    QVERIFY(!Search->isVisible()); QVERIFY(Window.isVisible());
    QVERIFY(!Window.Snapshot()["closeRequested"].toBool());
    QCOMPARE(Window.Snapshot()["documents"].toArray().size(), 1);
    QVERIFY(!Doc(Window.Snapshot(), Id)["dirty"].toBool());
    QCOMPARE(Text->Content().toUtf8(), Content); QTRY_VERIFY(Input->hasFocus());
    QTest::keyClick(Input, Qt::Key_Escape);
    QTRY_COMPARE(Window.Snapshot()["closeRequested"].toBool(), Hosted && CloseOnEscape);
    QCOMPARE(Read(Path), Content);
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
        for(const auto& Id:Ids)
            QCOMPARE(Call({{"action","captureProtection"},{"documentId",Id},{"enabled",true}})["error"].toString(), QString("document-view-command-unsupported"));
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
void optionalIsolatedComicLandscapeReading(){
    const auto Path=qEnvironmentVariable("HC_COMIC_BENCH_PATH");
    if(Path.isEmpty()) QSKIP("Set HC_COMIC_BENCH_PATH for the real archive integration check");
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtPluginViewerController Controller(nullptr,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
    QtPluginViewerSurface Surface(&Controller);Surface.resize(1200,800);Surface.show();
    Surface.Open(Options(),Content(Path));
    const auto Close=qScopeGuard([&]{Surface.Close();Wait([&]{return Controller.IsIdle();});});
    QVERIFY(Wait([&]{return Surface.IsReady() && !Surface.Details().documentState_["activeDocumentId"].toString().isEmpty();},30000));
    const auto Id=Surface.Details().documentState_["activeDocumentId"].toString();QVERIFY(!Id.isEmpty());
    const auto Reading=[&]{return Doc(Surface.Details().documentState_,Id)["view"].toObject()["reading"].toObject();};
    QVERIFY(Wait([&]{return Reading()["pageCount"].toInt()==101;},30000));
    const auto Present=Surface.SendDocument({{"action","present"}});QVERIFY(Present.HasValue());
    QVERIFY(Wait([&]{return Surface.Details().commandSequence_>=Present.Value();}));
    for(const int Page:{13,14,13,14}){
        const auto Sequence=Surface.SendDocument({{"action","reading.page"},{"page",Page},{"documentId",Id}});
        QVERIFY(Sequence.HasValue());
        QVERIFY(Wait([&]{return Reading()["presentedPage"].toInt(-1)==Page && !Reading()["transitioning"].toBool();},30000));
        QCOMPARE(Reading()["presentedLastPage"].toInt(-1),Page);
        QVERIFY(!Surface.Details().commandError_);
    }
    QVERIFY(!Doc(Surface.Details().documentState_,Id)["dirty"].toBool());
}
void isolatedFileDialogsRemainResponsive_data(){
    QTest::addColumn<bool>("SaveAs");QTest::addColumn<bool>("Modal");QTest::addColumn<bool>("CancelHost");QTest::addColumn<bool>("Preview");
    QTest::newRow("open")<<false<<false<<false<<false;QTest::newRow("save-as")<<true<<false<<false<<false;
    QTest::newRow("dirty-close")<<false<<true<<false<<false;QTest::newRow("cancel-modal")<<false<<true<<true<<false;
    QTest::newRow("print-preview")<<false<<true<<false<<true;
}
void isolatedFileDialogsRemainResponsive(){
    QFETCH(bool,SaveAs);QFETCH(bool,Modal);QFETCH(bool,CancelHost);QFETCH(bool,Preview);
    QTemporaryDir Root;const auto Path=Root.filePath(Modal&&!Preview?"dialog.txt":"dialog.png");
    QImage Page(40,60,QImage::Format_RGB32);Page.fill(Qt::green);
    if(Modal&&!Preview)QVERIFY(Put(Path,"original"));else QVERIFY(Page.save(Path));
    QWidget Main;Main.resize(900,650);Main.show();Main.activateWindow();QVERIFY(QTest::qWaitForWindowActive(&Main));
    auto Live=std::make_shared<std::atomic<int>>(0);
    QtViewerHost Host(&Main,[Live]()->Result<std::shared_ptr<IMaterializer>>{
        auto Item=std::make_shared<Materializer>();Item->live_=Live;return std::shared_ptr<IMaterializer>(Item);});
    const auto Drain=qScopeGuard([&]{Host.CloseAll();Wait([&]{return Host.IsIdle();});});
    bool Finished=false,Success=false;QString Failure;
    auto* Window=Host.OpenPlugin(Options(),{{Content(Path),{}}},"File dialog",
        [&](bool Ready,const QString& Error){Finished=true;Success=Ready;Failure=Error;});
    QVERIFY(Window);QVERIFY(Wait([&]{return Finished;},30000));QVERIFY2(Success,qPrintable(Failure));
    QPointer<QtPluginViewerSurface> Surface=Window->findChild<QtPluginViewerSurface*>();QVERIFY(Surface);
    const auto Connection=connect(Surface,&QtPluginViewerSurface::Failed,this,[&](const QString& Code){Failure=Code;});
    const auto Disconnect=qScopeGuard([&]{disconnect(Connection);});
    const auto Pid=Surface->Details().host_.processId_;
    WinHandle Process(OpenProcess(SYNCHRONIZE,FALSE,Pid));QVERIFY(Process.Valid());
    QVERIFY(Wait([&]{DWORD ForegroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&ForegroundPid);return ForegroundPid==Pid;}));
    const auto Viewer=GetForegroundWindow();
    QVERIFY(Wait([&]{const auto State=Surface->Details().documentState_;return !State["activeDocumentId"].toString().isEmpty()
        &&Doc(State,State["activeDocumentId"].toString())["lifecycle"]=="ready";}));
    if(Modal&&!Preview){const auto Id=Surface->Details().documentState_["activeDocumentId"].toString();
        const auto Changed=Surface->SendDocument({{"action","replaceText"},{"documentId",Id},{"text","changed"}});QVERIFY(Changed.HasValue());
        QVERIFY(Wait([&]{return Surface->Details().commandSequence_>=Changed.Value();}));
        const auto Details=Surface->Details();QVERIFY2(!Details.commandError_,Details.commandError_?Details.commandError_->code_.c_str():"");
        QVERIFY2(Wait([&]{return Doc(Surface->Details().documentState_,Id)["dirty"].toBool();}),qPrintable(QJsonDocument(Surface->Details().documentState_).toJson()));}
    const auto Press=[](WORD Key,WORD Modifier=0,WORD SecondModifier=0){
        std::vector<INPUT> Keys;const auto Append=[&](WORD Code,DWORD Flags){INPUT Input{};Input.type=INPUT_KEYBOARD;Input.ki.wVk=Code;Input.ki.dwFlags=Flags;Keys.push_back(Input);};
        if(Modifier)Append(Modifier,0);if(SecondModifier)Append(SecondModifier,0);Append(Key,0);Append(Key,KEYEVENTF_KEYUP);
        if(SecondModifier)Append(SecondModifier,KEYEVENTF_KEYUP);if(Modifier)Append(Modifier,KEYEVENTF_KEYUP);
        return SendInput(UINT(Keys.size()),Keys.data(),sizeof(INPUT))==Keys.size();};
    const auto Type=[](const QString& Text){std::vector<INPUT> Keys;for(const auto Character:Text){INPUT Input{};Input.type=INPUT_KEYBOARD;
        Input.ki.wScan=Character.unicode();Input.ki.dwFlags=KEYEVENTF_UNICODE;Keys.push_back(Input);
        Input.ki.dwFlags|=KEYEVENTF_KEYUP;Keys.push_back(Input);}return SendInput(UINT(Keys.size()),Keys.data(),sizeof(INPUT))==Keys.size();};
    const auto Launch=[&]{if(Preview)return Surface->SendDocument({{"action","printPreview"},{"documentId",Surface->Details().documentState_["activeDocumentId"]}}).HasValue();
        return Modal?Press('W',VK_CONTROL):SaveAs?Press('S',VK_CONTROL,VK_SHIFT):Press('O',VK_CONTROL);};
    const auto FindDialog=[&]{struct Search{DWORD pid_;QString title_;HWND found_=nullptr;}State{Pid,Preview?QString{}:Modal?QString::fromUtf8("변경 내용 저장"):SaveAs?QString::fromUtf8("다른 이름 저장"):QString::fromUtf8("문서 열기")};
        EnumWindows([](HWND Candidate,LPARAM Context)->BOOL{auto* Search=reinterpret_cast<struct Search*>(Context);
            DWORD Owner=0;GetWindowThreadProcessId(Candidate,&Owner);wchar_t Title[256]{};GetWindowTextW(Candidate,Title,256);
            const auto Parent=GetWindow(Candidate,GW_OWNER);
            const bool Match=Search->title_.isEmpty()?Parent&&!IsWindowEnabled(Parent):QString::fromWCharArray(Title)==Search->title_;
            if(Owner==Search->pid_&&IsWindowVisible(Candidate)&&Match)
                {Search->found_=Candidate;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&State));
        return State.found_;};
    QVERIFY(Launch());HWND Dialog=nullptr;QVERIFY(Wait([&]{Dialog=FindDialog();return Dialog!=nullptr;}));
    const auto Closed=qScopeGuard([&]{if(IsWindow(Dialog))PostMessageW(Dialog,WM_CLOSE,0,0);});
    // Exercise native directory browsing, including a redirected drive when
    // explicitly supplied by a connected RDP test session.
    if(!Modal){const auto Browse=qEnvironmentVariable("HC_FILE_DIALOG_TEST_DIR",Root.path());
        QVERIFY(Press('D',VK_MENU));QVERIFY(Type(QDir::toNativeSeparators(Browse)));QVERIFY(Press(VK_RETURN));}
    QTest::qWait(18500);
    QCOMPARE(WaitForSingleObject(Process.Get(),0),DWORD(WAIT_TIMEOUT));
    QVERIFY2(Surface,qPrintable(Failure));
    const auto Details=Surface->Details();
    QVERIFY2(Details.host_.running_,Details.host_.failure_?Details.host_.failure_->code_.c_str():"Host exited while browsing files");
    QVERIFY(IsWindow(Dialog));QVERIFY(IsWindowVisible(Dialog));
    if(CancelHost){Host.CloseAll();QVERIFY(Wait([&]{return Host.IsIdle();},5000));
        QCOMPARE(WaitForSingleObject(Process.Get(),0),DWORD(WAIT_OBJECT_0));QCOMPARE(Read(Path),QByteArray("original"));return;}
    if(Modal){QVERIFY(PostMessageW(Dialog,WM_CLOSE,0,0));QVERIFY(Wait([&]{return !IsWindow(Dialog)||!IsWindowVisible(Dialog);}));}
    const auto Sequence=Surface->SendDocument({{"action","state"}});QVERIFY(Sequence.HasValue());
    QVERIFY(Wait([&]{return Surface&&Surface->Details().commandSequence_>=Sequence.Value();},3000));
    QVERIFY(!Surface->Details().commandError_);
    if(Modal){QCOMPARE(Surface->Details().documentState_["documents"].toArray().size(),1);
        if(!Preview)QCOMPARE(Read(Path),QByteArray("original"));else QVERIFY(!QImage(Path).isNull());return;}
    QVERIFY(PostMessageW(Dialog,WM_CLOSE,0,0));
    QVERIFY(Wait([&]{return !IsWindow(Dialog)||!IsWindowVisible(Dialog);}));
    QVERIFY(Surface->IsReady());
    QCOMPARE(Surface->Details().documentState_["documents"].toArray().size(),1);
    const auto Selected=Root.filePath(SaveAs?"saved.png":"picked.png");
    if(!SaveAs)QVERIFY(Page.save(Selected));
    QVERIFY(Wait([&]{return IsWindowEnabled(Viewer);}));
    SetForegroundWindow(Viewer);QVERIFY(Wait([&]{return GetForegroundWindow()==Viewer;}));QTest::qWait(200);
    QVERIFY(Launch());QVERIFY(Wait([&]{Dialog=FindDialog();return Dialog!=nullptr;}));
    QVERIFY(Press('N',VK_MENU));QVERIFY(Press('A',VK_CONTROL));QVERIFY(Type(QDir::toNativeSeparators(Selected)));QVERIFY(Press(VK_RETURN));
    QVERIFY(Wait([&]{return !IsWindow(Dialog)||!IsWindowVisible(Dialog);}));
    QVERIFY(Wait([&]{if(!Surface)return false;for(const auto& Item:Surface->Details().documentState_["documents"].toArray()){
        const auto Document=Item.toObject();if(Document["path"].toString()==Selected&&Document["lifecycle"]=="ready"&&!Document["loading"].toBool())return true;}return false;}));
    QVERIFY(!QImage(Selected).isNull());
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
    Foreground(reinterpret_cast<HWND>(Main.winId()));QTRY_COMPARE(GetForegroundWindow(),reinterpret_cast<HWND>(Main.winId()));
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
    for(int I=1;I<Argc;++I)if(QByteArray(Argv[I])=="--focus-peer"){
        WNDCLASSW Class{};Class.hInstance=GetModuleHandleW(nullptr);Class.lpszClassName=L"HC.DocumentFocusPeer";Class.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        Class.lpfnWndProc=[](HWND Window,UINT Message,WPARAM W,LPARAM L)->LRESULT{
            if(Message==WM_APP+117)return SetForegroundWindow(reinterpret_cast<HWND>(W));
            if(Message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(Window,Message,W,L);
        };
        if(!RegisterClassW(&Class))return 20;
        const auto Window=CreateWindowExW(0,Class.lpszClassName,L"Document focus peer",WS_OVERLAPPEDWINDOW,GetSystemMetrics(SM_CXSCREEN)-440,80,400,200,nullptr,nullptr,Class.hInstance,nullptr);
        if(!Window)return 21;ShowWindow(Window,SW_SHOWNOACTIVATE);std::cout<<quintptr(Window)<<std::endl;
        MSG Message{};while(GetMessageW(&Message,nullptr,0,0)>0){TranslateMessage(&Message);DispatchMessageW(&Message);}return 0;
    }
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
