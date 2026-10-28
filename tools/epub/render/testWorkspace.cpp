#include "stdafx.h"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/epubProcessView.hpp"
#include "UniqueLibs/document/epubSourceEditor.hpp"
#include "commonLibs/document/epub/epubDraftStore.hpp"
#include <QProcess>
#include <QLayout>
#include <thread>
#include <QComboBox>
#include <QScopeGuard>
#include <QUuid>
#include <QFile>
#include <Windows.h>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QMessageBox>
#include <QPdfWriter>
#include <QPainter>
#include <QtTest>
#include <QCryptographicHash>
#include <QBuffer>
#include <QImage>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextLayout>
#include <QWindow>
#include <QDialog>
#include <QApplication>
#include <QTimer>
#include <QSignalBlocker>
#include "commonLibs/document/epub/epubSourceMap.hpp"
#include <QLineEdit>
#include <QCheckBox>
#include <QListWidget>
#include <QTreeWidget>
#include <QDialogButtonBox>
#include "UniqueLibs/document/documentThemeAdapter.hpp"
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QAccessible>
#include <imm.h>
#include <QAction>
#include <QMenuBar>
#include <QMenu>
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include <QJsonDocument>
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "commonLibs/document/epub/epubEditModel.hpp"

using namespace hc::document;
namespace {
bool foreground(QWidget& Window) {
    const auto DpiContext=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto RestoreDpi=qScopeGuard([DpiContext]{if(DpiContext)SetThreadDpiAwarenessContext(DpiContext);});
    const auto Handle=GetAncestor(reinterpret_cast<HWND>(Window.winId()),GA_ROOT);
    if(GetAncestor(GetForegroundWindow(),GA_ROOT)==Handle)return true;
    Window.raise();Window.activateWindow();SetForegroundWindow(Handle);QTest::qWait(120);
    if(GetAncestor(GetForegroundWindow(),GA_ROOT)==Handle)return true;
    SetWindowPos(Handle,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    const auto RestoreOrder=qScopeGuard([Handle]{SetWindowPos(Handle,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);});
    QTest::qWait(100);RECT Bounds{};POINT Cursor{};GetCursorPos(&Cursor);if(!GetWindowRect(Handle,&Bounds))return false;
    const int X=Bounds.left+qMin(150,(Bounds.right-Bounds.left)/2);int First=-1,Last=-1;
    for(int Y=Bounds.top;Y<qMin(Bounds.top+120,Bounds.bottom);++Y)if(SendMessageW(Handle,WM_NCHITTEST,0,MAKELPARAM(X,Y))==HTCAPTION){if(First<0)First=Y;Last=Y;}
    POINT Target{X,(First+Last)/2};
    if(First<0 || GetAncestor(WindowFromPoint(Target),GA_ROOT)!=Handle) {
        RECT Client{};if(!GetClientRect(Handle,&Client))return false;Target={Client.right/2,8};if(!ClientToScreen(Handle,&Target))return false;
    }
    const auto Hit=WindowFromPoint(Target);if(GetAncestor(Hit,GA_ROOT)!=Handle){qWarning()<<"EPUB activation target outside owned window"<<quintptr(Handle)<<quintptr(Hit)<<First<<Last<<Bounds.left<<Bounds.top<<Target.x<<Target.y;return false;}
    if(!SetCursorPos(Target.x,Target.y))return false;
    INPUT Click[2]{};Click[0].type=Click[1].type=INPUT_MOUSE;Click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;Click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    const auto Sent=SendInput(2,Click,sizeof(INPUT));QTest::qWait(80);SetCursorPos(Cursor.x,Cursor.y);
    qInfo()<<"EPUB caption activation"<<First-Bounds.top<<Last-Bounds.top<<Sent;
    SetWindowPos(Handle,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    const auto Foreground=GetForegroundWindow();const bool Active=GetAncestor(Foreground,GA_ROOT)==GetAncestor(Handle,GA_ROOT);
    if(!Active){DWORD Other=0;GetWindowThreadProcessId(Foreground,&Other);qWarning()<<"EPUB focus evidence"<<quintptr(Handle)<<quintptr(Foreground)<<quintptr(GetAncestor(Foreground,GA_ROOT))<<Other<<Bounds.left<<Bounds.top<<Bounds.right<<Bounds.bottom<<IsWindowVisible(Handle)<<IsIconic(Handle);}
    return Active;
}
WebProcessRuntime testEpubRuntime(const QString& Application) {
    const auto Pin=qgetenv("HC_EPUB_TEST_RUNTIME_PIN");
    return Pin.isEmpty()?MakeProductEpubRuntime(Application):MakeProductEpubRuntime(Application,Pin);
}
QJsonObject document(DocumentWorkspaceWidget& Window, const QString& Id) {
    for(const auto& Value:Window.Snapshot()["documents"].toArray())
        if(Value.toObject()["documentId"]==Id) {
            const auto Doc=Value.toObject(),State=Doc["view"].toObject()["epub"].toObject();
            const auto Error=State["status"]=="error"?State:State["readerAction"].toObject();
            static QSet<QByteArray> Seen;
            if(State["status"]=="error" || Error.contains("error")) {
                const auto Json=QJsonDocument(Error).toJson(QJsonDocument::Compact);
                if(!Seen.contains(Json)){Seen.insert(Json);qWarning().noquote()<<"EPUB test state error"<<QJsonDocument(State).toJson(QJsonDocument::Compact);}
            }
            return Doc;
        }
    return {};
}
}
class EpubWorkspaceTests final : public QObject {
    Q_OBJECT
private slots:
    void propertyDialogsPreserveLongValues() {
        const auto Path=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Path.isEmpty())QSKIP("Owned EPUB corpus required");QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));epub::EditModel Model;QVERIFY(Model.Load(File.readAll()));const auto Root=Model.Publication()["rootfile"].toString();auto Opf=QString::fromUtf8(Model.Read(Root));QVERIFY(Opf.contains("</metadata>"));const QString Creator(1500,QChar('z')),Alt(3000,QChar('x'));Opf.replace("</metadata>","<dc:creator>"+Creator+"</dc:creator></metadata>");QVERIFY(Model.Replace(Root,Opf.toUtf8()).isEmpty());const auto Uses=Model.InspectImage("EPUB/Images/street.svg")["occurrences"].toArray();QVERIFY(!Uses.isEmpty());const auto Row=Uses.first().toObject();auto Html=QString::fromUtf8(Model.Read(Row["document"].toString()));const auto Previous="alt=\""+Row["alt"].toString()+"\"";QVERIFY(Html.contains(Previous));Html.replace(Previous,"alt=\""+Alt+"\" width=\"100%\" height=\"1234567\"");QVERIFY(Model.Replace(Row["document"].toString(),Html.toUtf8()).isEmpty());const auto Input=Model.Export();QVERIFY(Input);
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());EpubSourceEditor Editor(Input.bytes_,Temp.filePath("source.epub"));Editor.SetRecoveryEnabled(false);Editor.resize(800,600);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);bool CreatorReadOnly=false,AltReadOnly=false,ImageDone=false;QTimer Poll;Poll.setInterval(30);connect(&Poll,&QTimer::timeout,&Editor,[&]{auto* Dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!Dialog)return;if(Dialog->objectName()=="epubMetadataDialog"){const auto* Author=Dialog->findChild<QLineEdit*>("epubMetadata_creator");CreatorReadOnly=Author->isReadOnly() && Author->text()==Creator;Dialog->findChild<QLineEdit*>("epubMetadata_title")->setText(QStringLiteral("긴 값 보존 검증"));Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();}else if(Dialog->objectName()=="epubImagePropertiesDialog"){auto* Text=Dialog->findChild<QLineEdit*>("epubImageAlt");AltReadOnly=!Text->isEnabled();auto* Caption=Dialog->findChild<QPlainTextEdit*>("epubImageCaption");if(Caption->isEnabled())Caption->setPlainText(QStringLiteral("긴 대체 텍스트를 보존한 캡션"));ImageDone=true;Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();}});Poll.start();Editor.findChild<QAction*>("epubMetadataAction")->trigger();QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["structuring"].toBool(),15000);QVERIFY(CreatorReadOnly);QCOMPARE(Editor.Execute({{"action","epub.edit.metadata-inspect"}})["values"].toObject()["creator"].toString(),Creator);
        QVERIFY(Editor.Execute({{"action","epub.edit.select"},{"resource","EPUB/Images/street.svg"}})["accepted"].toBool());Editor.findChild<QAction*>("epubImagePropertiesAction")->trigger();QTRY_VERIFY_WITH_TIMEOUT(ImageDone && !Editor.Snapshot()["structuring"].toBool(),15000);QVERIFY(AltReadOnly);QVERIFY(Editor.Snapshot()["structureError"].toString().isEmpty());const auto After=Editor.Execute({{"action","epub.edit.image-inspect"},{"resource","EPUB/Images/street.svg"}});bool Preserved=false;for(const auto& Use:After["occurrences"].toArray())Preserved|=Use.toObject()["alt"].toString()==Alt && Use.toObject()["width"]=="100%" && Use.toObject()["height"]=="1234567";QVERIFY(Preserved);
    }
    void precisePreviewSource() {
        const auto Book=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Book.isEmpty())QSKIP("Owned EPUB corpus required");QFile File(Book);QVERIFY(File.open(QIODevice::ReadOnly));const auto Prepared=epub::Prepare(File.readAll());QVERIFY(Prepared);QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1100,760);Window.show();QVERIFY(foreground(Window));const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();QVERIFY(QTest::qWaitFor([&]{return document(Window,Id)["view"].toObject()["epub"].toObject()["status"]=="ready";},60000));QVERIFY(!Window.Handle({{"action","epub.edit.begin"},{"documentId",Id}}).contains("error"));
        EpubSourceEditor* Editor=nullptr;QVERIFY(QTest::qWaitFor([&]{Editor=Window.findChild<EpubSourceEditor*>();return Editor && Editor->IsReady();},15000));QVERIFY(foreground(Window));QApplication::setActiveWindow(&Window);Editor->findChild<QPlainTextEdit*>("epubEditSource")->setFocus();QVERIFY2(QTest::qWaitFor([&]{return Editor->Snapshot()["preview"].toObject()["current"].toBool();},60000),qPrintable(QString::fromUtf8(QJsonDocument(Editor?Editor->Snapshot():QJsonObject{}).toJson(QJsonDocument::Compact))));const auto Path=Prepared.publication_["spine"].toArray()[0].toObject()["path"].toString();QVERIFY(Editor->Execute({{"action","epub.edit.select"},{"resource",Path}})["accepted"].toBool());auto* Text=Editor->findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Text);const int Position=Text->toPlainText().indexOf(QStringLiteral("나무"));QVERIFY(Position>0);auto Cursor=Text->textCursor();Cursor.setPosition(Position);Text->setTextCursor(Cursor);QVERIFY(Editor->Execute({{"action","epub.edit.source-locate"}})["accepted"].toBool());
        QVERIFY(QTest::qWaitFor([&]{return Editor->PreviewView()->ViewState()["sourceSelection"].toObject()["serial"].toInteger()>0;},15000));QVERIFY(Editor->PreviewView()->ViewState()["readerAction"].toObject()["error"].toString().isEmpty());{const QSignalBlocker Block(Text);Cursor.setPosition(0);Text->setTextCursor(Cursor);}QVERIFY(Editor->Execute({{"action","epub.edit.preview-source"}})["accepted"].toBool());QCOMPARE(Text->textCursor().position(),Position);QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void propertyDialogs() {
        const auto Path=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Path.isEmpty())QSKIP("Owned EPUB corpus required");QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());EpubSourceEditor Editor(File.readAll(),Path);Editor.SetRecoveryEnabled(false);Editor.resize(QSize(800,600).boundedTo(Editor.screen()->availableGeometry().size()-QSize(40,80)));Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);
        QTimer Poll;Poll.setInterval(30);bool MetadataAccepted=false,ImageAccepted=false;connect(&Poll,&QTimer::timeout,&Editor,[&]{auto* Dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!Dialog)return;const auto Evidence=qEnvironmentVariable("HC_EPUB_UI_EVIDENCE");if(Dialog->objectName()=="epubMetadataDialog"){Dialog->findChild<QLineEdit*>("epubMetadata_title")->setText(QStringLiteral("입력 폼 검증 제목"));if(!Evidence.isEmpty()){QDir().mkpath(Evidence);Dialog->grab().save(QDir(Evidence).filePath("metadata.png"));}MetadataAccepted=true;Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();}else if(Dialog->objectName()=="epubImagePropertiesDialog"){Dialog->findChild<QLineEdit*>("epubImageWidth")->setText("320");Dialog->findChild<QLineEdit*>("epubImageHeight")->setText("180");Dialog->findChild<QLineEdit*>("epubImageAlt")->setText(QStringLiteral("선택 위치 전용 대체 텍스트"));auto* Caption=Dialog->findChild<QPlainTextEdit*>("epubImageCaption");if(Caption->isEnabled())Caption->setPlainText(QStringLiteral("입력 폼 캡션"));if(!Evidence.isEmpty()){QDir().mkpath(Evidence);Dialog->grab().save(QDir(Evidence).filePath("image-properties.png"));}ImageAccepted=true;Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();}});Poll.start();auto* Metadata=Editor.findChild<QAction*>("epubMetadataAction");QVERIFY(Metadata);Metadata->trigger();QTRY_VERIFY_WITH_TIMEOUT(MetadataAccepted && !Editor.Snapshot()["structuring"].toBool(),15000);QCOMPARE(Editor.Execute({{"action","epub.edit.metadata-inspect"}})["values"].toObject()["title"].toString(),QStringLiteral("입력 폼 검증 제목"));
        QVERIFY(Editor.Execute({{"action","epub.edit.select"},{"resource","EPUB/Images/street.svg"}})["accepted"].toBool());auto* Properties=Editor.findChild<QAction*>("epubImagePropertiesAction");QVERIFY(Properties);Properties->trigger();QTRY_VERIFY_WITH_TIMEOUT(ImageAccepted && !Editor.Snapshot()["structuring"].toBool(),15000);QVERIFY(Editor.Snapshot()["structureError"].toString().isEmpty());const auto Info=Editor.Execute({{"action","epub.edit.image-inspect"},{"resource","EPUB/Images/street.svg"}});bool Found=false;for(const auto& Row:Info["occurrences"].toArray())Found|=Row.toObject()["alt"]==QStringLiteral("선택 위치 전용 대체 텍스트") && Row.toObject()["width"]=="320" && Row.toObject()["height"]=="180";QVERIFY(Found);
    }
    void sourceMapCoordinates() {
        const auto Source=QStringLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>책</title></head><body>\n<p title=\"&gt;\">가&amp;😀<em>강조</em>끝</p><!--주석--><![CDATA[x<y]]></body></html>");
        epub::SourceMap Map;QVERIFY(Map.Load(Source));
        for(const auto& Needle:{QStringLiteral("가"),QStringLiteral("😀"),QStringLiteral("강조"),QStringLiteral("끝"),QStringLiteral("x<y")}){const int Index=Source.indexOf(Needle);const auto Anchor=Map.Anchor(Index);QVERIFY(!Anchor.isEmpty());QCOMPARE(Map.Position(Anchor),Index);}
        const auto Entity=Map.Anchor(Source.indexOf("&amp;"));QCOMPARE(Entity["offset"].toInt(),1);QCOMPARE(Map.Position(Entity),Source.indexOf("&amp;"));
        QVERIFY(!Map.Load("<html><p>broken</html>"));QVERIFY(Map.Anchor(0).isEmpty());
    }
    void sourceResourceState() {
        const auto Path=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Path.isEmpty())QSKIP("Owned EPUB corpus required");QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());EpubSourceEditor Editor(File.readAll(),Path);Editor.SetRecoveryEnabled(false);Editor.resize(800,600);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);
        QStringList Chapters;for(const auto& Value:Editor.Snapshot()["resources"].toArray()){const auto Row=Value.toObject();if(Row["mediaType"]=="application/xhtml+xml")Chapters.append(Row["path"].toString());}QVERIFY(Chapters.size()>=2);
        const auto Select=[&](const QString& Resource){return Editor.Execute({{"action","epub.edit.select"},{"resource",Resource}})["accepted"].toBool();};QVERIFY(Select(Chapters[0]));auto* Text=Editor.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Text);auto* First=Text->document();const auto Original=Text->toPlainText();const int Position=Original.indexOf("</body>");QVERIFY(Position>0);auto Cursor=Text->textCursor();Cursor.setPosition(Position);Text->setTextCursor(Cursor);Text->insertPlainText("<p>첫 리소스 입력</p>");const auto Edited=Text->toPlainText();const int Caret=Text->textCursor().position();
        QVERIFY(Select(Chapters[1]));auto Other=Text->textCursor();Other.setPosition(Text->toPlainText().indexOf("</body>"));Text->setTextCursor(Other);Text->insertPlainText("<p>다른 리소스 입력</p>");QVERIFY(Select(Chapters[0]));QCOMPARE(Text->document(),First);QCOMPARE(Text->toPlainText(),Edited);QCOMPARE(Text->textCursor().position(),Caret);QVERIFY(Editor.CanUndo());Editor.Undo();QCOMPARE(Text->toPlainText(),Original);Editor.Redo();QCOMPARE(Text->toPlainText(),Edited);QTRY_VERIFY(!Text->document()->firstBlock().layout()->formats().isEmpty());
        const auto Metadata=Editor.Execute({{"action","epub.edit.metadata-inspect"}});const auto OldTitle=Metadata["values"].toObject()["title"].toString();QVERIFY(Editor.Execute({{"action","epub.edit.structure"},{"operation","metadata"},{"expectedSha256",Metadata["sha256"]},{"values",QJsonObject{{"title",QStringLiteral("구조 실행 취소 순서")}}}})["accepted"].toBool());QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["structuring"].toBool(),15000);QVERIFY(Editor.Snapshot()["structureError"].toString().isEmpty());Editor.Undo();QCOMPARE(Editor.Execute({{"action","epub.edit.metadata-inspect"}})["values"].toObject()["title"].toString(),OldTitle);QCOMPARE(Text->toPlainText(),Edited);Editor.Redo();QCOMPARE(Editor.Execute({{"action","epub.edit.metadata-inspect"}})["values"].toObject()["title"].toString(),QStringLiteral("구조 실행 취소 순서"));
        Editor.Execute({{"action","epub.edit.validate"}});QTRY_VERIFY_WITH_TIMEOUT(Editor.IsValidated(),15000);const auto Copy=Editor.Export(Temp.filePath("saved.epub"));QVERIFY(Copy.HasValue());Editor.Commit(Temp.filePath("saved.epub"),Copy.Value());QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);QCOMPARE(Text->document(),First);QVERIFY(Editor.CanUndo());Editor.Undo();QVERIFY(Editor.IsModified());
    }
    void occurrenceAndMetadataTransactions() {
        const auto Path=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Path.isEmpty())QSKIP("Owned EPUB corpus required");QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));const auto Bytes=File.readAll();epub::EditModel Model;QVERIFY(Model.Load(Bytes));const auto Before=Model.Fingerprint();const auto Image=QStringLiteral("EPUB/Images/street.svg");const auto ImageBytes=Model.Read(Image);const auto Info=Model.InspectImage(Image);QVERIFY(!Info.contains("error"));QVERIFY(!Info["width"].toString().isEmpty());QCOMPARE(Model.InspectImage(Image,[]{return true;})["error"].toString(),QString("cancelled"));QJsonArray Editable;for(const auto& Row:Info["occurrences"].toArray())if(Row.toObject()["editable"].toBool())Editable.append(Row);QVERIFY(Editable.size()>=2);const auto First=Editable[0].toObject(),Second=Editable[1].toObject();const auto OtherBytes=Model.Read(Second["document"].toString());
        QJsonObject Request{{"operation","image-attributes"},{"resource",Image},{"document",First["document"]},{"elementPath",First["elementPath"]},{"expectedSha256",First["sha256"]},{"width","320"},{"height","180"},{"alt",QStringLiteral("선택 위치 & 대체 텍스트")},{"caption",QStringLiteral("선택 위치 캡션")}};
        QVERIFY2(Model.Structure(Request).isEmpty(),qPrintable(Model.Error()));const auto After=Model.Fingerprint();QVERIFY(After!=Before);QCOMPARE(Model.Read(Second["document"].toString()),OtherBytes);QCOMPARE(Model.Read(Image),ImageBytes);QVERIFY(Model.Export());QVERIFY(!Model.Structure(Request).isEmpty());QCOMPARE(Model.Fingerprint(),After);Model.Undo();QCOMPARE(Model.Fingerprint(),Before);Model.Redo();QCOMPARE(Model.Fingerprint(),After);
        QJsonObject Current;for(const auto& Use:Model.InspectImage(Image)["occurrences"].toArray())if(Use.toObject()["document"]==First["document"] && Use.toObject()["alt"]==Request["alt"])Current=Use.toObject();QCOMPARE(Current["width"].toString(),QString("320"));QCOMPARE(Current["height"].toString(),QString("180"));auto SizeRequest=Request;SizeRequest["elementPath"]=Current["elementPath"];SizeRequest["expectedSha256"]=Current["sha256"];SizeRequest["width"]="20px";QVERIFY(!Model.Structure(SizeRequest).isEmpty());QCOMPARE(Model.Fingerprint(),After);SizeRequest["width"]="";SizeRequest["height"]="";QVERIFY(Model.Structure(SizeRequest).isEmpty());QVERIFY(Model.Fingerprint()!=After);Model.Undo();QCOMPARE(Model.Fingerprint(),After);
        const auto Metadata=Model.Metadata();const auto Identifier=Metadata["values"].toObject()["identifier"];QVERIFY(Model.Structure({{"operation","metadata"},{"expectedSha256",Metadata["sha256"]},{"values",QJsonObject{{"title",QStringLiteral("새 제목 & <보존>")},{"creator",QStringLiteral("테스트 저자")},{"language","ko-KR"}}}}).isEmpty());const auto Copy=Model.Export();QVERIFY(Copy);const auto Prepared=epub::Prepare(Copy.bytes_);QVERIFY(Prepared);QCOMPARE(Prepared.publication_["title"].toString(),QStringLiteral("새 제목 & <보존>"));QCOMPARE(Model.Metadata()["values"].toObject()["identifier"],Identifier);const auto Token=Model.Fingerprint();QVERIFY(!Model.Structure({{"operation","metadata"},{"expectedSha256",Model.Metadata()["sha256"]},{"values",QJsonObject{{"language","bad language"}}}}).isEmpty());QCOMPARE(Model.Fingerprint(),Token);
    }
    void editorScaleLayout() {
        const auto Path=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Path.isEmpty())QSKIP("Owned EPUB corpus required");QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());EpubSourceEditor Editor(File.readAll(),Path);Editor.SetRecoveryEnabled(false);Editor.resize(QSize(680,520).boundedTo(Editor.screen()->availableGeometry().size()-QSize(40,80)));Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);auto* Scroll=Editor.findChild<QScrollArea*>("epubSourceScroll");auto* Save=Editor.findChild<QPushButton*>("epubSaveCopy");QVERIFY(Scroll && Save);Scroll->ensureWidgetVisible(Save);QTest::qWait(50);QVERIFY(Scroll->viewport()->rect().intersects(QRect(Save->mapTo(Scroll->viewport(),QPoint{}),Save->size())));qInfo()<<"EPUB DPI evidence"<<Editor.devicePixelRatioF()<<Editor.screen()->name()<<Editor.size()<<Scroll->viewport()->size();
        const auto Expected=qEnvironmentVariable("HC_EPUB_EXPECT_DPR");if(!Expected.isEmpty())QVERIFY(qAbs(Editor.devicePixelRatioF()-Expected.toDouble())<0.05);
    }
    void mixedDpiPreviewMovement() {
        const auto Screens=QGuiApplication::screens();bool Different=false;for(auto* Screen:Screens)Different|=qAbs(Screen->devicePixelRatio()-Screens.first()->devicePixelRatio())>0.05;if(Screens.size()<2 || !Different)QSKIP("Physical mixed-DPI monitors are not connected; scale-factor tests do not replace this acceptance");
        const auto Book=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Book.isEmpty())QSKIP("Owned EPUB corpus required");QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(800,600);Window.show();const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};QVERIFY(QTest::qWaitFor([&]{return State()["status"]=="ready" && !State()["readerBusy"].toBool();},60000));
        for(auto* Screen:Screens){Window.windowHandle()->setScreen(Screen);Window.move(Screen->availableGeometry().topLeft()+QPoint(30,30));QTRY_COMPARE(Window.screen(),Screen);QVERIFY(QTest::qWaitFor([&]{return State()["status"]=="ready" && !State()["readerBusy"].toBool();},15000));Window.showMaximized();QTest::qWait(200);Window.showNormal();QVERIFY(State()["error"].toString().isEmpty());qInfo()<<"Physical EPUB monitor movement"<<Screen->name()<<Window.devicePixelRatioF();}
        QVERIFY(!Window.Handle({{"action","close"},{"documentId",Id},{"policy","discard"}}).contains("error"));
    }
    void scrollPageCommands_data() {
        QTest::addColumn<QString>("Kind");for(const auto* Kind:{"ltr","rtl","vertical"})QTest::newRow(Kind)<<QString(Kind);
    }
    void scrollPageCommands() {
        QFETCH(QString,Kind);const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB"),Content=qEnvironmentVariable("HC_EPUB_CONTENT_CORPUS");if(Corpus.isEmpty() || Content.isEmpty())QSKIP("Reader and content fixtures required");
        const auto Book=Kind=="ltr"?QFileInfo(Corpus).dir().filePath("epub3-toc-fragments.epub"):QDir(Content).filePath(Kind+".epub");QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1200,800);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};const auto Do=[&](const QString& Action,QJsonValue Value=QJsonValue{}){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id},{"value",Value}});};
        QVERIFY(QTest::qWaitFor([&]{return State()["readingStateLoaded"].toBool() && !State()["readerBusy"].toBool();},60000));QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"layout","scroll"},{"size",26},{"columns",2}}}}).contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["profile"].toObject()["size"]==QJsonValue(26) && !State()["readerBusy"].toBool();},15000));
        const auto ChapterSerial=State()["readerAction"].toObject()["serial"].toInteger();QVERIFY(!Do("chapter",0).contains("error"));QVERIFY(QTest::qWaitFor([&]{const auto S=State();return S["readerAction"].toObject()["serial"].toInteger()>ChapterSerial && S["chapterIndex"]==QJsonValue(0) && !S["readerBusy"].toBool();},15000));const auto Initial=State();const auto Frame=Initial["frameId"].toString();const auto Before=Initial["scrollState"].toObject();qInfo()<<"Scroll initial"<<Kind<<Before<<Initial["startup"].toObject()["layoutState"];QVERIFY2(Before["extent"].toDouble()>50,qPrintable(QString::fromUtf8(QJsonDocument(Initial).toJson(QJsonDocument::Compact))));
        for(int I=0;I<3;++I){QVERIFY(!Do("next").contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["scrollState"].toObject()["offset"].toDouble()>Before["offset"].toDouble()+10 && !State()["readerBusy"].toBool();},15000));QCOMPARE(State()["chapterIndex"].toInt(),0);QCOMPARE(State()["frameId"].toString(),Frame);QVERIFY(!Do("previous").contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["scrollState"].toObject()["offset"].toDouble()<Before["offset"].toDouble()+2 && !State()["readerBusy"].toBool();},15000));QCOMPARE(State()["frameId"].toString(),Frame);}
        if(Kind!="vertical")QVERIFY(!State()["readerHorizontalScrollbar"].toBool());
        if(Kind=="vertical"){QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"layout","paginated"},{"columns",2}}}}).contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["profile"].toObject()["layout"]=="paginated" && !State()["readerBusy"].toBool();},15000));QCOMPARE(State()["toolbarColumns"].toInt(),1);QVERIFY(!State()["columnsControlEnabled"].toBool());QCOMPARE(State()["profile"].toObject()["columns"].toInt(),2);}
        if(Kind=="ltr") {
            QVERIFY(foreground(Window));auto* Process=Window.findChild<EpubProcessView*>();QVERIFY(Process && Process->FocusInput());QVERIFY(QTest::qWaitFor([&]{return State()["readerFocused"].toBool();},10000));HWND Reader=nullptr;EnumChildWindows(reinterpret_cast<HWND>(Window.winId()),[](HWND Child,LPARAM Data)->BOOL{wchar_t Title[128]{};GetWindowTextW(Child,Title,128);if(QString::fromWCharArray(Title)==QStringLiteral("EPUB 읽기"))*reinterpret_cast<HWND*>(Data)=Child;return TRUE;},reinterpret_cast<LPARAM>(&Reader));QVERIFY(Reader);const auto Thread=GetWindowThreadProcessId(Reader,nullptr);
            const auto Key=[&](WORD Code,int Scan,bool Extended){GUITHREADINFO Info{sizeof(Info)};if(!GetGUIThreadInfo(Thread,&Info))return false;const auto Target=Info.hwndFocus?Info.hwndFocus:Reader;const LPARAM Bits=1|(Scan<<16)|(Extended?(1<<24):0);return PostMessageW(Target,WM_KEYDOWN,Code,Bits) && PostMessageW(Target,WM_KEYUP,Code,Bits|(1LL<<30)|(1LL<<31));};
            for(const bool Extended:{true,false}){QVERIFY(Key(VK_NEXT,0x51,Extended));QVERIFY(QTest::qWaitFor([&]{return State()["scrollState"].toObject()["offset"].toDouble()>10 && !State()["readerBusy"].toBool();},15000));QCOMPARE(State()["frameId"].toString(),Frame);QVERIFY(Key(VK_PRIOR,0x49,Extended));QVERIFY(QTest::qWaitFor([&]{return State()["scrollState"].toObject()["offset"].toDouble()<2 && !State()["readerBusy"].toBool();},15000));}
            QVERIFY(!Do("chapter",1).contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["chapterIndex"]==QJsonValue(1) && !State()["readerBusy"].toBool();},15000));QVERIFY(!Do("previous").contains("error"));QVERIFY(QTest::qWaitFor([&]{const auto S=State(),M=S["scrollState"].toObject();return S["chapterIndex"]==QJsonValue(0) && M["offset"].toDouble()>=M["extent"].toDouble()-2 && !S["readerBusy"].toBool();},15000));
        }
        for(auto* Screen:QGuiApplication::screens())qInfo()<<"Acceptance screen"<<Screen->name()<<Screen->geometry()<<Screen->availableGeometry()<<Screen->devicePixelRatio();
        const auto Evidence=qEnvironmentVariable("HC_EPUB_ACCEPTANCE_EVIDENCE");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);QVERIFY(foreground(Window));const auto Point=Window.mapToGlobal(QPoint{});QVERIFY(Window.screen()->grabWindow(0,Point.x(),Point.y(),Window.width(),Window.height()).save(QDir(Evidence).filePath("scroll-"+Kind+".png")));}
        qInfo()<<"Scroll viewport navigation"<<Kind<<Before;QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void sourceCaretAfterSaveCopy() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QFile Input(Corpus);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());EpubSourceEditor Editor(Original,Corpus);Editor.resize(900,650);Editor.show();QVERIFY(QTest::qWaitFor([&]{return Editor.IsReady();},15000));auto* Resources=Editor.findChild<QComboBox*>("epubEditResources");auto* Text=Editor.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Resources && Text);int Index=-1;for(int I=0;I<Resources->count();++I)if(Resources->itemData(I)=="text/css")Index=I;QVERIFY(Index>=0);Resources->setCurrentIndex(Index);const auto Selected=Resources->currentText();QVERIFY(Text->toPlainText().size()>40);QVERIFY(foreground(Editor));Text->setFocus();auto Cursor=Text->textCursor();Cursor.setPosition(10);Cursor.setPosition(30,QTextCursor::KeepAnchor);Text->setTextCursor(Cursor);QVERIFY(!Editor.Execute({{"action","epub.edit.validate"}}).contains("error"));QVERIFY(QTest::qWaitFor([&]{return Editor.IsValidated();},15000));const auto Output=Editor.Export(Temp.filePath("copy.epub"));QVERIFY(Output.HasValue());Editor.Commit(Temp.filePath("copy.epub"),Output.Value());QVERIFY(QTest::qWaitFor([&]{return Editor.IsReady();},15000));QCOMPARE(Resources->currentText(),Selected);QCOMPARE(Text->textCursor().anchor(),10);QCOMPARE(Text->textCursor().position(),30);QVERIFY(QTest::qWaitFor([&]{return Text->hasFocus();},5000));auto* Accessible=QAccessible::queryAccessibleInterface(Text);QVERIFY(Accessible);QCOMPARE(Accessible->role(),QAccessible::EditableText);QVERIFY(!Accessible->text(QAccessible::Name).isEmpty());const auto Evidence=qEnvironmentVariable("HC_EPUB_ACCEPTANCE_EVIDENCE");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);const auto Point=Editor.mapToGlobal(QPoint{});QVERIFY(Editor.screen()->grabWindow(0,Point.x(),Point.y(),Editor.width(),Editor.height()).save(QDir(Evidence).filePath("source-small.png")));}
    }
    void nativeKoreanSourceInput() {
        if(!qEnvironmentVariableIsSet("HC_EPUB_NATIVE_IME"))QSKIP("Interactive Korean IME opt-in required");const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");QFile Input(Corpus);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.resize(1400,900);Window.show();QVERIFY(foreground(Window));const auto Id=Window.Handle({{"action","open"},{"path",Corpus}})["documentId"].toString();QVERIFY(QTest::qWaitFor([&]{return document(Window,Id)["view"].toObject()["epub"].toObject()["readingStateLoaded"].toBool();},60000));auto* Begin=Window.findChild<QAction*>("epubEdit");QVERIFY(Begin);Begin->trigger();QVERIFY(QTest::qWaitFor([&]{return Window.findChild<EpubSourceEditor*>() && Window.findChild<EpubSourceEditor*>()->IsReady() && Window.findChild<EpubSourceEditor*>()->PreviewView() && Window.findChild<EpubSourceEditor*>()->PreviewView()->IsLoaded();},60000));auto& Editor=*Window.findChild<EpubSourceEditor*>();auto* Resources=Editor.findChild<QComboBox*>("epubEditResources");auto* Text=Editor.findChild<QPlainTextEdit*>("epubEditSource");int Index=-1;for(int I=0;I<Resources->count();++I)if(Resources->itemData(I)=="text/css")Index=I;QVERIFY(Index>=0);Resources->setCurrentIndex(Index);Text->moveCursor(QTextCursor::End);Text->insertPlainText("\n/*  */");Text->moveCursor(QTextCursor::Left,QTextCursor::MoveAnchor);Text->moveCursor(QTextCursor::Left,QTextCursor::MoveAnchor);Text->moveCursor(QTextCursor::Left,QTextCursor::MoveAnchor);Text->setFocus();QVERIFY(foreground(Window));QApplication::setActiveWindow(&Window);Text->setFocus();
        const auto PreviousLayout=GetKeyboardLayout(0);const auto RestoreLayout=qScopeGuard([&]{ActivateKeyboardLayout(PreviousLayout,0);});const auto Korean=LoadKeyboardLayoutW(L"00000412",KLF_NOTELLSHELL);QVERIFY(Korean);QVERIFY(ActivateKeyboardLayout(Korean,0));QTest::qWait(100);Text->setFocus();QTest::qWait(200);QVERIFY(Text->hasFocus());const auto Target=GetFocus();QVERIFY(Target);const auto Imm=LoadLibraryW(L"imm32.dll");QVERIFY(Imm);const auto ReleaseImm=qScopeGuard([&]{FreeLibrary(Imm);});
        const auto GetContext=reinterpret_cast<HIMC(WINAPI*)(HWND)>(GetProcAddress(Imm,"ImmGetContext"));const auto ReleaseContext=reinterpret_cast<BOOL(WINAPI*)(HWND,HIMC)>(GetProcAddress(Imm,"ImmReleaseContext"));const auto SetOpen=reinterpret_cast<BOOL(WINAPI*)(HIMC,BOOL)>(GetProcAddress(Imm,"ImmSetOpenStatus"));const auto SetConversion=reinterpret_cast<BOOL(WINAPI*)(HIMC,DWORD,DWORD)>(GetProcAddress(Imm,"ImmSetConversionStatus"));const auto GetOpen=reinterpret_cast<BOOL(WINAPI*)(HIMC)>(GetProcAddress(Imm,"ImmGetOpenStatus"));const auto GetConversion=reinterpret_cast<BOOL(WINAPI*)(HIMC,LPDWORD,LPDWORD)>(GetProcAddress(Imm,"ImmGetConversionStatus"));QVERIFY(GetContext && ReleaseContext && SetOpen && SetConversion && GetOpen && GetConversion);HWND ContextWindow=Target;auto Context=GetContext(ContextWindow);if(!Context){ContextWindow=reinterpret_cast<HWND>(Window.winId());Context=GetContext(ContextWindow);}qInfo()<<"Korean IME context"<<quintptr(Target)<<quintptr(ContextWindow)<<quintptr(Context)<<quintptr(Text->winId())<<GetKeyboardLayout(0);QVERIFY(Context);const auto OldOpen=GetOpen(Context);DWORD OldConversion=0,OldSentence=0;QVERIFY(GetConversion(Context,&OldConversion,&OldSentence));const auto RestoreIme=qScopeGuard([&]{const auto C=GetContext(ContextWindow);if(C){SetOpen(C,OldOpen);SetConversion(C,OldConversion,OldSentence);ReleaseContext(ContextWindow,C);}});QVERIFY(SetOpen(Context,TRUE));QVERIFY(SetConversion(Context,IME_CMODE_NATIVE,0));ReleaseContext(ContextWindow,Context);
        const auto Key=[&](WORD Value){INPUT Events[2]{};Events[0].type=Events[1].type=INPUT_KEYBOARD;Events[0].ki.wVk=Events[1].ki.wVk=Value;Events[1].ki.dwFlags=KEYEVENTF_KEYUP;return GetAncestor(GetForegroundWindow(),GA_ROOT)==reinterpret_cast<HWND>(Window.winId()) && SendInput(2,Events,sizeof(INPUT))==2;};int At=0;for(const WORD Value:{'G','K','S','R','M','F'}){QVERIFY(Key(Value));QTest::qWait(80);if(++At==3){QVERIFY(Text->property("epubSourceComposing").toBool());auto* Preview=Editor.PreviewView();QTest::qWait(1100);QVERIFY(Text->hasFocus());QCOMPARE(Editor.PreviewView(),Preview);QVERIFY(Text->property("epubSourceComposing").toBool());}}
        QVERIFY(Key(VK_SPACE));QVERIFY(QTest::qWaitFor([&]{return Text->toPlainText().contains(QStringLiteral("한글 ")) && !Text->property("epubSourceComposing").toBool();},5000));qInfo()<<"Native Korean source composition and focus verified";const auto Evidence=qEnvironmentVariable("HC_EPUB_ACCEPTANCE_EVIDENCE");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);const auto Point=Editor.mapToGlobal(QPoint{});QVERIFY(Editor.screen()->grabWindow(0,Point.x(),Point.y(),Editor.width(),Editor.height()).save(QDir(Evidence).filePath("native-korean.png")));}QVERIFY(Editor.RequestPreviewClose() || QTest::qWaitFor([&]{return Editor.IsPreviewClosed();},30000));QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readerAnnotations() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB"),Application=qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION");if(Corpus.isEmpty() || Application.isEmpty())QSKIP("EPUB runtime and corpus required");QFile Input(Corpus);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();Input.close();QTemporaryDir Temp;const auto Config=Temp.filePath("settings");qApp->setProperty("documentViewsConfigDir",Config);QJsonObject Saved;
        {
            DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(Application));Window.resize(1450,960);Window.move(60,60);Window.show();const auto Id=Window.Handle({{"action","open"},{"path",Corpus}})["documentId"].toString();const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};const auto Do=[&](const QString& Action,QJsonValue Value=QJsonValue{}){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id},{"value",Value}});};
            QVERIFY(QTest::qWaitFor([&]{const auto S=State();return S["readingStateLoaded"].toBool() && S["status"]=="ready" && !S["readerBusy"].toBool();},60000));QVERIFY(!Do("search","reader-control needle").contains("error"));QVERIFY(QTest::qWaitFor([&]{return !State()["readerAction"].toObject()["hits"].toArray().isEmpty();},15000));const QString Locator=State()["readerAction"].toObject()["hits"].toArray().first().toObject()["cfi"].toString();QVERIFY(!Locator.isEmpty());
            QVERIFY(!Do("annotation-add",QJsonObject{{"locator",Locator},{"note",QStringLiteral("그림과 본문 사이의 메모")},{"color","#ffde59"}}).contains("error"));const bool Added=QTest::qWaitFor([&]{const auto S=State();return S["annotations"].toArray().size()==1 && S["annotationState"].toObject()["rendered"].toInt()>0 && !S["readerBusy"].toBool();},20000);QVERIFY2(Added,qPrintable(QString::fromUtf8(QJsonDocument(QJsonObject{{"action",State()["annotationAction"]},{"render",State()["annotationState"]},{"native",State()["annotations"]}}).toJson())));
            Saved=State()["annotations"].toArray().first().toObject();QCOMPARE(Saved["note"].toString(),QStringLiteral("그림과 본문 사이의 메모"));QVERIFY(Saved["quote"].toString().contains("reader-control needle"));QVERIFY(!Do("focus-notes").contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["openPanel"]=="notes" && !State()["readerBusy"].toBool();},15000));QVERIFY(!Do("annotation-go",Saved["id"]).contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["annotationState"].toObject()["rendered"].toInt()>0 && !State()["readerBusy"].toBool();},15000));
            const auto Evidence=qEnvironmentVariable("HC_EPUB_ANNOTATION_EVIDENCE");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);for(const auto& Theme:QStringList{"light","dark"}){auto Appearance=Window.ChromeSettings();Appearance["themeMode"]=Theme;QVERIFY(Window.ApplyChromeSettings(Appearance));QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"theme",Theme}}}}).contains("error"));QVERIFY(QTest::qWaitFor([&]{const auto S=State();return S["profile"].toObject()["theme"]==Theme && S["annotationState"].toObject()["rendered"].toInt()>0 && !S["readerBusy"].toBool();},15000));QVERIFY(foreground(Window));QTest::qWait(180);const auto Point=Window.mapToGlobal(QPoint{});QVERIFY(Window.screen()->grabWindow(0,Point.x(),Point.y(),Window.width(),Window.height()).save(QDir(Evidence).filePath("annotations-"+Theme+".png")));QFile Json(QDir(Evidence).filePath("annotations-"+Theme+".json"));QVERIFY(Json.open(QIODevice::WriteOnly));Json.write(QJsonDocument(State()).toJson());}}
            QVERIFY(!Do("next-chapter").contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["chapterIndex"]==QJsonValue(1) && !State()["readerBusy"].toBool();},15000));QVERIFY(!Do("annotation-go",Saved["id"]).contains("error"));QVERIFY(QTest::qWaitFor([&]{const auto S=State();return S["chapterIndex"]==QJsonValue(0) && S["annotationState"].toObject()["rendered"].toInt()>0 && !S["readerBusy"].toBool();},15000));QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
        {
            DocumentWorkspaceWidget Reopened(Temp.filePath("recovery2"),true);Reopened.ConfigureEpub(testEpubRuntime(Application));Reopened.show();const auto Id=Reopened.Handle({{"action","open"},{"path",Corpus}})["documentId"].toString();const auto State=[&]{return document(Reopened,Id)["view"].toObject()["epub"].toObject();};QVERIFY(QTest::qWaitFor([&]{const auto S=State();return S["annotations"].toArray().size()==1 && S["annotationState"].toObject()["rendered"].toInt()>0 && !S["readerBusy"].toBool();},60000));QCOMPARE(State()["annotations"].toArray().first().toObject(),Saved);QTRY_VERIFY_WITH_TIMEOUT(Reopened.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
        QVERIFY(Input.open(QIODevice::ReadOnly));QCOMPARE(Input.readAll(),Original);
    }
    void readerAnnotationEditionTransfer() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB"),Application=qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION");if(Corpus.isEmpty() || Application.isEmpty())QSKIP("EPUB runtime and corpus required");QFile Input(Corpus);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();QTemporaryDir Temp;QJsonObject Bundle;QString SourceLocator;
        {
            qApp->setProperty("documentViewsConfigDir",Temp.filePath("source-settings"));DocumentWorkspaceWidget Source(Temp.filePath("source-recovery"),true);Source.ConfigureEpub(testEpubRuntime(Application));Source.show();const auto Id=Source.Handle({{"action","open"},{"path",Corpus}})["documentId"].toString();const auto State=[&]{return document(Source,Id)["view"].toObject()["epub"].toObject();};QVERIFY(QTest::qWaitFor([&]{return State()["readingStateLoaded"].toBool() && !State()["readerBusy"].toBool();},60000));QVERIFY(!Source.Handle({{"action","epub.reader.search"},{"documentId",Id},{"value",QStringLiteral("나무 그늘에서 잠시")}}).contains("error"));QVERIFY(QTest::qWaitFor([&]{return !State()["readerAction"].toObject()["hits"].toArray().isEmpty();},15000));SourceLocator=State()["readerAction"].toObject()["hits"].toArray().first().toObject()["cfi"].toString();QVERIFY(!Source.Handle({{"action","epub.reader.annotation-add"},{"documentId",Id},{"value",QJsonObject{{"locator",SourceLocator},{"note",QStringLiteral("다른 판본에서도 보존")},{"color","#63d8ad"}}}}).contains("error"));QVERIFY(QTest::qWaitFor([&]{return State()["annotations"].toArray().size()==1 && !State()["annotationsBusy"].toBool();},15000));Bundle={{"schemaVersion",1},{"kind","hypercommander.epub.annotations"},{"book",QString::fromLatin1(QCryptographicHash::hash(Original,QCryptographicHash::Sha256).toHex())},{"annotations",State()["annotations"]}};QTRY_VERIFY_WITH_TIMEOUT(Source.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
        for(const bool Ambiguous:{false,true}) {
            epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Spine=Model.Publication()["spine"].toArray();const auto First=Spine[0].toObject()["path"].toString(),Second=Spine[1].toObject()["path"].toString();auto Html=QString::fromUtf8(Model.Read(First));
            if(Ambiguous)QVERIFY(Model.Replace(Second,Model.Read(First)).isEmpty());
            else {QVERIFY(Html.contains("<p id=\"hc-reading-profile\">"));Html.replace("<p id=\"hc-reading-profile\">",QStringLiteral("<p>새 판본에 추가한 서문</p><p id=\"hc-reading-profile\">"));QVERIFY(Model.Replace(First,Html.toUtf8()).isEmpty());auto Other=QString::fromUtf8(Model.Read(Second));Other.replace(QStringLiteral("나무 그늘에서 잠시"),QStringLiteral("다른 이야기의 시작"));QVERIFY(Model.Replace(Second,Other.toUtf8()).isEmpty());}
            const auto Output=Model.Export();QVERIFY2(Output,qPrintable(Output.error_));const auto Path=Temp.filePath(Ambiguous?"ambiguous.epub":"shifted.epub");QFile Book(Path);QVERIFY(Book.open(QIODevice::WriteOnly));Book.write(Output.bytes_);Book.close();qApp->setProperty("documentViewsConfigDir",Temp.filePath(Ambiguous?"ambiguous-settings":"shifted-settings"));DocumentWorkspaceWidget Target(Temp.filePath(Ambiguous?"ambiguous-recovery":"shifted-recovery"),true);Target.ConfigureEpub(testEpubRuntime(Application));Target.show();const auto Id=Target.Handle({{"action","open"},{"path",Path}})["documentId"].toString();const auto State=[&]{return document(Target,Id)["view"].toObject()["epub"].toObject();};QVERIFY(QTest::qWaitFor([&]{return State()["readingStateLoaded"].toBool() && !State()["readerBusy"].toBool();},60000));QVERIFY(!Target.Handle({{"action","epub.reader.annotations-import"},{"documentId",Id},{"value",Bundle}}).contains("error"));const bool Complete=QTest::qWaitFor([&]{const auto S=State();return S["annotations"].toArray().size()==1 && !S["annotationsBusy"].toBool() && !S["readerBusy"].toBool() && (Ambiguous || S["annotationState"].toObject()["rendered"].toInt()>0);},30000);QVERIFY2(Complete,qPrintable(QString::fromUtf8(QJsonDocument(QJsonObject{{"action",State()["annotationAction"]},{"native",State()["annotations"]}}).toJson())));const auto Row=State()["annotations"].toArray().first().toObject();QCOMPARE(Row["unresolved"].toBool(),Ambiguous);QCOMPARE(Row["note"].toString(),QStringLiteral("다른 판본에서도 보존"));if(Ambiguous){QVERIFY(Row["locator"].toString().isEmpty());QCOMPARE(State()["annotationState"].toObject()["rendered"].toInt(),0);}else {QVERIFY(Row["locator"].toString()!=SourceLocator);QVERIFY(State()["annotationState"].toObject()["rendered"].toInt()>0);}QTRY_VERIFY_WITH_TIMEOUT(Target.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
    }
    void contentLayouts_data() {
        QTest::addColumn<QString>("Kind");for(const auto* Kind:{"fixed","mixed","rtl","vertical","ruby","legacy-deflate-flags","legacy-container","fixed-missing-viewport","conflicting-layout"})QTest::newRow(Kind)<<QString(Kind);
    }
    void contentLayouts() {
        QFETCH(QString,Kind);const auto Corpus=qEnvironmentVariable("HC_EPUB_CONTENT_CORPUS");if(Corpus.isEmpty())QSKIP("Content-layout fixtures required");QFile File(QDir(Corpus).filePath(Kind+".epub"));QVERIFY(File.open(QIODevice::ReadOnly));const auto Original=File.readAll();File.close();const auto Prepared=epub::Prepare(Original);
        if(Kind=="fixed-missing-viewport" || Kind=="conflicting-layout"){QVERIFY(!Prepared);QCOMPARE(Prepared.error_,Kind=="fixed-missing-viewport"?QString("fixed-layout-viewport"):QString("conflicting-rendition-layout"));return;}
        QVERIFY2(Prepared,qPrintable(Prepared.error_));const bool Fixed=Kind=="fixed" || Kind=="mixed";QCOMPARE(Prepared.publication_["hasFixedLayout"].toBool(),Fixed);QCOMPARE(Prepared.publication_["mixedLayout"].toBool(),Kind=="mixed");
        epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Exported=Model.Export();QVERIFY2(Exported,qPrintable(Exported.error_));QVERIFY(epub::Prepare(Exported.bytes_));QCOMPARE(Model.OriginalBytes(),Original);
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1400,960);Window.move(60,60);Window.show();const auto Id=Window.Handle({{"action","open"},{"path",File.fileName()}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};const auto Ready=[&]{return State()["status"]=="ready" && State()["readingStateLoaded"].toBool() && !State()["readerBusy"].toBool();};const auto Do=[&](const QString& Action,QJsonValue Value=QJsonValue{}){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id},{"value",Value}});};
        QTRY_VERIFY2_WITH_TIMEOUT(Ready(),qPrintable(QString::fromUtf8(QJsonDocument(State()).toJson())),60000);
        for(const auto& Layout:QStringList{"paginated","scroll"}) {
            QElapsedTimer ProfileTime;ProfileTime.start();QTimer ProfileTrace;ProfileTrace.setInterval(1000);connect(&ProfileTrace,&QTimer::timeout,&ProfileTrace,[&]{const auto S=State();qInfo().noquote()<<"Content profile progress"<<Kind<<Layout<<ProfileTime.elapsed()<<QJsonDocument(QJsonObject{{"startup",S["startup"]},{"settingsBusy",S["settingsBusy"]},{"readingStateRestoring",S["readingStateRestoring"]},{"readerBusy",S["readerBusy"]},{"profileLayout",S["profile"].toObject()["layout"]},{"settingsResult",S["settingsResult"]}}).toJson(QJsonDocument::Compact);});ProfileTrace.start();
            const auto Theme=Layout=="paginated"?QString("light"):QString("dark");auto Appearance=Window.ChromeSettings();Appearance["themeMode"]=Theme;QVERIFY(Window.ApplyChromeSettings(Appearance));
            QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"theme",Theme},{"layout",Layout},{"columns",2},{"size",30},{"line",2.4}}}}).contains("error"));qint64 StableSince=-1;const bool ProfileReady=QTest::qWaitFor([&]{const auto S=State();const bool Ready=S["status"]=="ready" && S["readingStateLoaded"].toBool() && !S["readerBusy"].toBool() && S["profile"].toObject()["layout"]==Layout;if(!Ready){StableSince=-1;return false;}if(StableSince<0)StableSince=ProfileTime.elapsed();return ProfileTime.elapsed()-StableSince>=100;},15000);ProfileTrace.stop();qInfo()<<"Content profile ready ms"<<Kind<<Layout<<ProfileTime.elapsed();QVERIFY2(ProfileReady,qPrintable(QString::fromUtf8(QJsonDocument(State()["startup"].toObject()).toJson(QJsonDocument::Compact))));
            QVERIFY(!Do("chapter",0).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Ready() && State()["chapterIndex"]==QJsonValue(0),15000);
            QCOMPARE(State()["contentLayout"].toString(),Fixed?QString("pre-paginated"):QString("reflowable"));
            if(Fixed){QVERIFY(State()["pageCanvasColors"].toArray().contains("rgb(255, 255, 255)"));QCOMPARE(State()["columnCount"].toInt(),1);QVERIFY(!State()["columnsControlEnabled"].toBool());QVERIFY(!State()["paragraphs"].toArray().isEmpty());QVERIFY(State()["paragraphs"].toArray().first().toObject()["size"].toString()!="30px");}
            if(Kind=="ruby" || Kind=="vertical"){const auto Ruby=State()["ruby"].toArray();QVERIFY(!Ruby.isEmpty());const auto Annotations=Ruby.first().toObject()["annotations"].toArray();QVERIFY(!Annotations.isEmpty());QCOMPARE(Annotations.first().toObject()["display"].toString(),QString("ruby-text"));QCOMPARE(Annotations.first().toObject()["text"].toString(),QStringLiteral("にほんご"));}
            if(Kind=="vertical")QVERIFY(State()["writingModes"].toArray().contains("vertical-rl"));
            if(Kind=="rtl" || Kind=="vertical")QCOMPARE(State()["readingProgression"].toString(),QString("rtl"));
            for(int I=0;I<4;++I){QVERIFY(!Do("chapter",1).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Ready() && State()["chapterIndex"]==QJsonValue(1),15000);QCOMPARE(State()["contentLayout"].toString(),Kind=="fixed"?QString("pre-paginated"):QString("reflowable"));QVERIFY(!Do("previous-chapter").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Ready() && State()["chapterIndex"]==QJsonValue(0),15000);}
            if(Kind=="fixed"){QVERIFY(!Do("next").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Ready() && State()["chapterIndex"]==QJsonValue(1),15000);QVERIFY(!Do("previous").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Ready() && State()["chapterIndex"]==QJsonValue(0),15000);}
            for(const auto& Value:State()["images"].toArray()){QVERIFY(Value.toObject()["complete"].toBool());QVERIFY(Value.toObject()["naturalWidth"].toInt()>0);}
            const auto Evidence=qEnvironmentVariable("HC_EPUB_CONTENT_EVIDENCE");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);QVERIFY(foreground(Window));QTest::qWait(180);const auto Point=Window.mapToGlobal(QPoint{});QVERIFY(Window.screen()->grabWindow(0,Point.x(),Point.y(),Window.width(),Window.height()).save(QDir(Evidence).filePath(Kind+"-"+Layout+".png")));QFile Json(QDir(Evidence).filePath(Kind+"-"+Layout+".json"));QVERIFY(Json.open(QIODevice::WriteOnly));Json.write(QJsonDocument(State()).toJson());}
        }
        QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Original);File.close();QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void epubStructureEditing_data(){QTest::addColumn<QString>("Name");for(const auto* Name:{"epub2-ncx.epub","epub3-images.epub","epub3-compressed.epub"})QTest::newRow(Name)<<QString(Name);}
    void epubStructureEditing() {
        QFETCH(QString,Name);const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());QFile Input(QFileInfo(Corpus).dir().filePath(Name));QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Baseline=Model.Fingerprint();const auto First=Model.Publication()["spine"].toArray().first().toObject()["path"].toString();QString Png,Css;
        for(const auto& Value:Model.Publication()["resources"].toArray()){const auto Row=Value.toObject();if(Row["mediaType"]=="image/png")Png=Row["path"].toString();if(Row["mediaType"]=="text/css")Css=Row["path"].toString();}QVERIFY(!Png.isEmpty() && !Css.isEmpty());const auto Pixels=Model.Read(Png),OriginalCss=Model.Read(Css);
        const auto Run=[&](QJsonObject Request){const auto Error=Model.Structure(Request);if(!Error.isEmpty())qWarning().noquote()<<Error;return Error.isEmpty();};
        const QString Added="EPUB/Images/새 그림.png";QVERIFY(Run({{"operation","image-add"},{"resource",Added},{"base64",QString::fromLatin1(Pixels.toBase64())},{"chapter",First},{"alt",QStringLiteral("새 이미지")}}));QCOMPARE(Model.Read(Added),Pixels);QVERIFY(Model.Fingerprint()!=Baseline);Model.Undo();QCOMPARE(Model.Fingerprint(),Baseline);QVERIFY(!Model.IsEditable(Added));Model.Redo();QVERIFY(Model.IsEditable(Added));
        const auto BeforeRejected=Model.Fingerprint();QVERIFY(!Run({{"operation","image-add"},{"resource",Added},{"base64",QString::fromLatin1(Pixels.toBase64())}}));QCOMPARE(Model.Fingerprint(),BeforeRejected);QVERIFY(!Run({{"operation","image-add"},{"resource","../escape.png"},{"base64",QString::fromLatin1(Pixels.toBase64())}}));QCOMPARE(Model.Fingerprint(),BeforeRejected);
        const auto Relative=[&](const QString& Base,const QString& Path){QUrl Url;Url.setPath(QDir(QDir::current().absoluteFilePath(QFileInfo(Base).path())).relativeFilePath(QDir::current().absoluteFilePath(Path)));return Url.toString(QUrl::FullyEncoded);};
        const auto HtmlUri=Relative(First,Png),CssUri=Relative(Css,Png);auto Html=QString::fromUtf8(Model.Read(First));Html.replace("</body>",QString("<img src=\"%1\" srcset=\"%1 1x, %1 2x\" style=\"background:url('%1')\" alt=\"ref test\"/></body>").arg(HtmlUri));QVERIFY(Model.Replace(First,Html.toUtf8()).isEmpty());
        const auto CssText=QString::fromUtf8(OriginalCss)+QString("\n.ref {background-image:u\\72l('%1');}\n.set {background-image:image-set(\"%1\" 1x,url('%1') 2x);}\n").arg(CssUri);QVERIFY(Model.Replace(Css,CssText.toUtf8()).isEmpty());
        const QString Renamed="EPUB/ImagesMoved/이름 변경.png";QVERIFY(Run({{"operation","rename"},{"resource",Png},{"destination",Renamed}}));QVERIFY(!Model.IsEditable(Png));QCOMPARE(Model.Read(Renamed),Pixels);QVERIFY(QString::fromUtf8(Model.Read(Css)).contains(Relative(Css,Renamed)));QVERIFY(QString::fromUtf8(Model.Read(First)).contains("srcset=\""+Relative(First,Renamed)));
        const auto WithReferences=Model.Fingerprint();QVERIFY(!Run({{"operation","remove"},{"resource",Renamed}}));QCOMPARE(Model.Fingerprint(),WithReferences);
        const QString Jpeg="EPUB/ImagesMoved/변환.jpg";QVERIFY(!Run({{"operation","image-convert"},{"resource",Renamed},{"destination",Jpeg},{"format","jpeg"}}));QCOMPARE(Model.Fingerprint(),WithReferences);QVERIFY(Run({{"operation","image-convert"},{"resource",Renamed},{"destination",Jpeg},{"format","jpeg"},{"flattenAlpha",true},{"quality",95}}));QVERIFY(!QImage::fromData(Model.Read(Jpeg)).isNull());QVERIFY(!Model.IsEditable(Renamed));
        QVERIFY(Model.Replace(Css,OriginalCss).isEmpty());const auto BeforeDelete=Model.Fingerprint();QVERIFY(Run({{"operation","remove"},{"resource",Jpeg},{"removeImageReferences",true}}));QVERIFY(!Model.IsEditable(Jpeg));Model.Undo();QCOMPARE(Model.Fingerprint(),BeforeDelete);QVERIFY(Run({{"operation","cover"},{"resource",Added}}));
        const QString Chapter="EPUB/Text/추가 장.xhtml";QVERIFY(Run({{"operation","chapter-add"},{"resource",Chapter},{"title",QStringLiteral("새 & 장")},{"after",0}}));QCOMPARE(Model.Publication()["spine"].toArray().size(),3);QCOMPARE(Model.Publication()["spine"].toArray()[1].toObject()["path"].toString(),Chapter);
        const auto Second=Model.Publication()["spine"].toArray()[2].toObject()["path"].toString();QJsonArray Toc{QJsonObject{{"label","group"},{"path",""},{"fragment",""},{"depth",0}},QJsonObject{{"label","first"},{"path",First},{"fragment",""},{"depth",1}},QJsonObject{{"label",QStringLiteral("새 & 장")},{"path",Chapter},{"fragment",""},{"depth",1}},QJsonObject{{"label","last"},{"path",Second},{"fragment",""},{"depth",0}}};
        QVERIFY(Run({{"operation","toc"},{"entries",Toc}}));QCOMPARE(Model.Publication()["navigation"].toArray().size(),4);QVERIFY(Run({{"operation","chapter-move"},{"resource",Chapter},{"delta",1}}));QCOMPARE(Model.Publication()["spine"].toArray().last().toObject()["path"].toString(),Chapter);
        const QString Moved="EPUB/NewText/Deep/바뀐 장.xhtml";QVERIFY(Run({{"operation","rename"},{"resource",First},{"destination",Moved}}));QCOMPARE(Model.Publication()["spine"].toArray().first().toObject()["path"].toString(),Moved);QVERIFY(Run({{"operation","remove"},{"resource",Chapter}}));QCOMPARE(Model.Publication()["spine"].toArray().size(),2);
        const auto BeforeInvalidToc=Model.Fingerprint();QVERIFY(!Run({{"operation","toc"},{"entries",QJsonArray{QJsonObject{{"label","bad"},{"path","missing.xhtml"},{"depth",0}}}}}));QCOMPARE(Model.Fingerprint(),BeforeInvalidToc);const auto Output=Model.Export();QVERIFY2(Output,qPrintable(Output.error_+Output.location_));QCOMPARE(Model.OriginalBytes(),Original);QVERIFY(!Model.StructuralBytes().isEmpty());epub::EditModel Reopened;QVERIFY(Reopened.Load(Output.bytes_));QCOMPARE(Reopened.Read(Added),Pixels);QVERIFY(!Reopened.Read(Jpeg).isEmpty());
        const auto Saved=Temp.filePath("structural.epub");QFile File(Saved);QVERIFY(File.open(QIODevice::WriteOnly));QCOMPARE(File.write(Output.bytes_),Output.bytes_.size());File.close();DocumentWorkspaceWidget Viewer(Temp.filePath("reader"),true);Viewer.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Viewer.resize(1280,900);Viewer.show();const auto Id=Viewer.Handle({{"action","open"},{"path",Saved}})["documentId"].toString();const auto State=[&]{return document(Viewer,Id)["view"].toObject();};QTRY_VERIFY_WITH_TIMEOUT(State()["ready"].toBool(),60000);QVERIFY(State()["readOnly"].toBool());for(const auto& Value:State()["epub"].toObject()["images"].toArray()){QVERIFY(Value.toObject()["complete"].toBool());QVERIFY(Value.toObject()["naturalWidth"].toInt()>0);}QTRY_VERIFY_WITH_TIMEOUT(Viewer.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        const auto Java=qEnvironmentVariable("HC_EPUBCHECK_JAVA"),Jar=qEnvironmentVariable("HC_EPUBCHECK_JAR");
        if(Name!="epub2-ncx.epub" && !Java.isEmpty() && !Jar.isEmpty()) {
            QFile Config(Temp.filePath("HyperEditor.json"));QVERIFY(Config.open(QIODevice::WriteOnly));Config.write(QJsonDocument(QJsonObject{{"schemaVersion",2},{"documentViews",QJsonObject{{"epubEditor",QJsonObject{{"javaPath",Java},{"epubcheckJar",Jar},{"requireEpubCheck",true}}}}}}).toJson());Config.close();EpubSourceEditor Check(Output.bytes_,Saved);Check.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(Check.IsReady(),10000);Check.Execute({{"action","epub.edit.validate"}});QTRY_VERIFY_WITH_TIMEOUT(!Check.Snapshot()["validating"].toBool(),30000);QVERIFY2(Check.IsValidated(),qPrintable(QJsonDocument(Check.Snapshot()["standardValidation"].toObject()).toJson()));
        }
    }
    void structureEditorRecovery() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());const auto Path=Temp.filePath("original.epub");QVERIFY(QFile::copy(Corpus,Path));QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));const auto Original=File.readAll();File.close();epub::EditModel Model;QVERIFY(Model.Load(Original));QString Png;for(const auto& Value:Model.Publication()["resources"].toArray())if(Value.toObject()["mediaType"]=="image/png")Png=Value.toObject()["path"].toString();QVERIFY(!Png.isEmpty());const auto First=Model.Publication()["spine"].toArray().first().toObject()["path"].toString();
        QFile Pixels(Temp.filePath("import.png"));QVERIFY(Pixels.open(QIODevice::WriteOnly));Pixels.write(Model.Read(Png));Pixels.close();EpubSourceEditor Editor(Original,Path);Editor.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),10000);const auto Reply=Editor.Execute({{"action","epub.edit.structure"},{"operation","image-add"},{"resource","EPUB/Images/ui-added.png"},{"imageFile",Pixels.fileName()},{"chapter",First},{"alt","UI image"}});QVERIFY(!Reply.contains("error"));QVERIFY(Editor.Snapshot()["structuring"].toBool());QVERIFY(Editor.Execute({{"action","epub.edit.replace"},{"resource",First},{"text","<html> rejected during structural work"}}).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["structuring"].toBool(),30000);QVERIFY2(Editor.Snapshot()["structureError"].toString().isEmpty(),qPrintable(Editor.Snapshot()["structureError"].toString()));QCOMPARE(Editor.Snapshot()["current"].toString(),QString("EPUB/Images/ui-added.png"));QVERIFY(Editor.IsModified());Editor.Undo();QVERIFY(!Editor.IsModified());Editor.Redo();QVERIFY(Editor.IsModified());
        bool NavigationEdited=false;QTimer::singleShot(0,&Editor,[&]{auto* Dialog=Editor.findChild<QDialog*>("epubNavigationEditor");if(!Dialog)return;auto Close=qScopeGuard([Dialog]{Dialog->reject();});auto* Tree=Dialog->findChild<QTreeWidget*>("epubNavigationTree");if(!Tree || !Tree->topLevelItemCount())return;auto* Caption=Dialog->findChild<QLineEdit*>("epubNavigationLabel");Caption->setFocus();Caption->setText("abc");Caption->setCursorPosition(1);QTest::keyClicks(Caption,"xy");if(Caption->text()!="axybc" || Caption->cursorPosition()!=3)return;Tree->topLevelItem(0)->setText(0,QStringLiteral("목차 UI 검증"));const auto Evidence=qEnvironmentVariable("HC_EPUB_STRUCTURE_OUTPUT");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);DocumentThemeAdapter::Install(*qApp);for(const auto& Mode:QStringList{"light","dark"}){DocumentThemeAdapter::SetMode(Mode);QTest::qWait(100);Dialog->grab().save(QDir(Evidence).filePath("navigation-"+Mode+".png"));}DocumentThemeAdapter::SetMode("system");}Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();NavigationEdited=Dialog->result()==QDialog::Accepted;if(NavigationEdited)Close.dismiss();});
        Editor.findChild<QPushButton*>("epubEditNavigation")->click();QVERIFY(NavigationEdited);QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["structuring"].toBool(),30000);QVERIFY2(Editor.Snapshot()["structureError"].toString().isEmpty(),qPrintable(Editor.Snapshot()["structureError"].toString()));
        Editor.Execute({{"action","epub.edit.validate"}});QTRY_VERIFY_WITH_TIMEOUT(Editor.IsValidated(),10000);const auto Saved=Editor.Export(Temp.filePath("copy.epub"));QVERIFY(Saved.HasValue());epub::EditModel Changed;QVERIFY(Changed.Load(Saved.Value()));QCOMPARE(Changed.Publication()["navigation"].toArray().first().toObject()["label"].toString(),QStringLiteral("목차 UI 검증"));
        epub::DraftSnapshot Snapshot;Snapshot.sourcePath_=Path;Snapshot.original_=Original;Snapshot.structuralBytes_=Saved.Value();Snapshot.currentResource_=First;Snapshot.hasCurrentDraft_=true;Snapshot.currentDraft_="<html> unfinished structural draft";Snapshot.readingChapter_=1;const auto Draft=epub::DraftStore::Write(EpubSourceEditor::RecoveryDirectory(),QUuid::createUuid().toString(QUuid::Id128),Snapshot);QVERIFY2(Draft,qPrintable(Draft.error_));const auto Read=epub::DraftStore::Read(EpubSourceEditor::RecoveryDirectory(),Draft.path_);QVERIFY(Read);QCOMPARE(Read.snapshot_.readingChapter_,1);QCOMPARE(Read.snapshot_.structuralBytes_,Saved.Value());
        EpubSourceEditor Restored(Original,Path);Restored.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(Restored.IsReady(),10000);Restored.Execute({{"action","epub.edit.recover"},{"path",Draft.path_}});QTRY_VERIFY_WITH_TIMEOUT(Restored.Snapshot()["current"].toString()==First && Restored.findChild<QPlainTextEdit*>("epubEditSource")->toPlainText()==QString::fromUtf8(Snapshot.currentDraft_),30000);QVERIFY(Restored.IsModified());bool Added=false;for(const auto& Row:Restored.Snapshot()["resources"].toArray())Added|=Row.toObject()["path"]=="EPUB/Images/ui-added.png";QVERIFY(Added);QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Original);
    }
    void epubValidationSettings() {
        const auto Java=qEnvironmentVariable("HC_EPUBCHECK_JAVA"),Jar=qEnvironmentVariable("HC_EPUBCHECK_JAR");if(Java.isEmpty() || Jar.isEmpty())QSKIP("Official EPUBCheck paths required");
        QTemporaryDir Temp;DocumentThemeAdapter::Install(*qApp);const auto Path=Temp.filePath("HyperEditor.json");
        DocumentSettingsDialog Dialog(Path,{});Dialog.show();
        auto* JavaField=Dialog.findChild<QLineEdit*>("documentViews/epubEditor/javaPath");auto* JarField=Dialog.findChild<QLineEdit*>("documentViews/epubEditor/epubcheckJar");auto* Required=Dialog.findChild<QCheckBox*>("documentViews/epubEditor/requireEpubCheck");QVERIFY(JavaField && JarField && Required);QVERIFY(!Required->isChecked());
        Required->setChecked(true);QVERIFY(!Dialog.Apply());QVERIFY(!QFileInfo::exists(Path));JavaField->setText(Java);JarField->setText(Jar);QVERIFY(Dialog.Apply());
        QFile Saved(Path);QVERIFY(Saved.open(QIODevice::ReadOnly));const auto Config=QJsonDocument::fromJson(Saved.readAll()).object()["documentViews"].toObject()["epubEditor"].toObject();QCOMPARE(Config["javaPath"].toString(),Java);QCOMPARE(Config["epubcheckJar"].toString(),Jar);QVERIFY(Config["requireEpubCheck"].toBool());
        Dialog.findChild<QListWidget*>("settingsCategories")->setCurrentRow(3);const auto Evidence=qEnvironmentVariable("HC_EPUB_VALIDATION_OUTPUT");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);for(const auto& Mode:QStringList{"light","dark"}){QVERIFY(DocumentThemeAdapter::SetMode(Mode));QTest::qWait(100);QVERIFY(Dialog.grab().save(QDir(Evidence).filePath("validation-settings-"+Mode+".png")));}}Dialog.reject();QVERIFY(DocumentThemeAdapter::SetMode("system"));
        DocumentSettingsDialog Reload(Path,{});QCOMPARE(Reload.findChild<QLineEdit*>(JavaField->objectName())->text(),Java);QVERIFY(Reload.findChild<QCheckBox*>(Required->objectName())->isChecked());
    }
    void cssSaveValidation() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QFile Input(Corpus);QVERIFY(Input.open(QIODevice::ReadOnly));epub::EditModel Model;QVERIFY(Model.Load(Input.readAll()));QString Css;
        for(const auto& Item:Model.Publication()["resources"].toArray())if(Item.toObject()["mediaType"]=="text/css")Css=Item.toObject()["path"].toString();QVERIFY(!Css.isEmpty());
        for(const auto& Good:QList<QByteArray>{"p {filter:url(#filter{);}","p {filter:URL(#filter[);}","p::before {content:\"}\";/* {[ */color:red;--x:calc(1px + 2px);}","@media print {p[data-x=\"(\"] {color:blue;}}","p {content:'\\\'';}","p {content:'hello\\\nworld';}"}){QVERIFY(Model.Replace(Css,Good).isEmpty());const auto Output=Model.Export();QVERIFY2(Output,qPrintable(Output.error_+Output.location_));}
        for(const auto& Bad:QList<QByteArray>{"p {color:red;","p{color:rgb(1,2,3];}","/* unterminated","p{content:'line\nbreak';}","p{content:'open"}){QVERIFY(Model.Replace(Css,Bad).isEmpty());const auto Output=Model.Export();QVERIFY(!Output);QVERIFY(Output.error_.startsWith("CSS"));QVERIFY(Output.location_.startsWith(Css+":"));}
    }
    void epubCheckValidation() {
        const auto Java=qEnvironmentVariable("HC_EPUBCHECK_JAVA"),Jar=qEnvironmentVariable("HC_EPUBCHECK_JAR"),Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");
        if(Java.isEmpty() || Jar.isEmpty() || Corpus.isEmpty())QSKIP("Explicit official EPUBCheck Java/JAR and corpus required");
        QTemporaryDir Temp;const auto Previous=qApp->property("documentViewsConfigDir");qApp->setProperty("documentViewsConfigDir",Temp.path());const auto Restore=qScopeGuard([Previous]{qApp->setProperty("documentViewsConfigDir",Previous);});
        const auto Settings=[&](bool Required,const QString& Program){QFile File(Temp.filePath("HyperEditor.json"));if(!File.open(QIODevice::WriteOnly))return false;return File.write(QJsonDocument(QJsonObject{{"schemaVersion",2},{"documentViews",QJsonObject{{"epubEditor",QJsonObject{{"javaPath",Program},{"epubcheckJar",Jar},{"requireEpubCheck",Required}}}}}}).toJson())>0;};
        QVERIFY(Settings(true,Java));const auto Path=Temp.filePath("input.epub");QVERIFY(QFile::copy(Corpus,Path));QFile Original(Path);QVERIFY(Original.open(QIODevice::ReadOnly));const auto Bytes=Original.readAll();Original.close();epub::EditModel Model;QVERIFY(Model.Load(Bytes));QString Css;
        for(const auto& Item:Model.Publication()["resources"].toArray())if(Item.toObject()["mediaType"]=="text/css")Css=Item.toObject()["path"].toString();QVERIFY(!Css.isEmpty());
        EpubSourceEditor Editor(Bytes,Path);Editor.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),30000);
        const auto Standard=[&]{return Editor.Snapshot()["standardValidation"].toObject();};
        const auto Validate=[&]{return Editor.Execute({{"action","epub.edit.validate"}});};
        const auto Replace=[&](const QString& Text){return Editor.Execute({{"action","epub.edit.replace"},{"resource",Css},{"text",Text}});};
        QVERIFY(!Validate().contains("error"));QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["validating"].toBool(),30000);QVERIFY2(Editor.IsValidated(),qPrintable(QJsonDocument(Standard()).toJson()));QCOMPARE(Standard()["state"].toString(),QString("passed"));QVERIFY(Standard()["version"].toString().startsWith("5.4"));
        QVERIFY(!Replace("body { color red; }").contains("error"));QVERIFY(!Editor.IsValidated());Validate();QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["validating"].toBool(),30000);QCOMPARE(Standard()["state"].toString(),QString("failed"));QVERIFY(!Editor.IsValidated());
        const auto Diagnostics=Standard()["diagnostics"].toArray();int Found=-1;for(int I=0;I<Diagnostics.size();++I)if(Diagnostics[I].toObject()["resource"]==Css && Diagnostics[I].toObject()["severity"]=="ERROR")Found=I;QVERIFY2(Found>=0,qPrintable(QJsonDocument(Standard()).toJson()));
        QVERIFY(Editor.Execute({{"action","epub.edit.diagnostic"},{"index",Found}})["accepted"].toBool());QCOMPARE(Editor.Snapshot()["current"].toString(),Css);QVERIFY(!Editor.Export(Temp.filePath("bad.epub")).HasValue());
        QVERIFY(!Replace("body {color:red;}").contains("error"));Validate();QTRY_COMPARE_WITH_TIMEOUT(Standard()["state"].toString(),QString("running"),10000);Editor.Execute({{"action","epub.edit.validation-cancel"}});QTRY_COMPARE_WITH_TIMEOUT(Standard()["state"].toString(),QString("cancelled"),10000);QVERIFY(!Editor.IsValidated());
        Validate();QTRY_COMPARE_WITH_TIMEOUT(Standard()["state"].toString(),QString("running"),10000);QVERIFY(!Replace("body {color:blue;}").contains("error"));QTRY_COMPARE_WITH_TIMEOUT(Standard()["state"].toString(),QString("stale"),10000);QVERIFY(!Editor.IsValidated());
        Validate();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsValidated(),30000);const auto Output=Editor.Export(Temp.filePath("valid.epub"));QVERIFY(Output.HasValue());
        QVERIFY(Settings(true,Temp.filePath("missing-java.exe")));Editor.ReloadValidationSettings();QVERIFY(!Editor.IsValidated());Validate();QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["validating"].toBool(),10000);QCOMPARE(Standard()["state"].toString(),QString("error"));QVERIFY(!Editor.IsValidated());
        QVERIFY(Settings(false,Java));Editor.ReloadValidationSettings();Validate();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsValidated(),10000);
        QVERIFY(Settings(true,Java));QFile Legacy(QDir(QFileInfo(Corpus).absolutePath()).filePath("epub2-ncx.epub"));QVERIFY(Legacy.open(QIODevice::ReadOnly));EpubSourceEditor LegacyEditor(Legacy.readAll(),Temp.filePath("legacy.epub"));LegacyEditor.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(LegacyEditor.IsReady(),10000);LegacyEditor.Execute({{"action","epub.edit.validate"}});QTRY_VERIFY_WITH_TIMEOUT(!LegacyEditor.Snapshot()["validating"].toBool(),30000);const auto LegacyState=LegacyEditor.Snapshot()["standardValidation"].toObject();QCOMPARE(LegacyState["state"].toString(),QString("failed"));
        const auto LegacyRows=LegacyState["diagnostics"].toArray();int Unicode=-1;for(int I=0;I<LegacyRows.size();++I)if(LegacyRows[I].toObject()["resource"].toString().contains(QStringLiteral("첫 장.xhtml")))Unicode=I;QVERIFY(Unicode>=0);QVERIFY(LegacyEditor.Execute({{"action","epub.edit.diagnostic"},{"index",Unicode}})["accepted"].toBool());QVERIFY(LegacyEditor.Snapshot()["current"].toString().contains(QStringLiteral("첫 장.xhtml")));
        const auto Evidence=qEnvironmentVariable("HC_EPUB_VALIDATION_OUTPUT");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);DocumentThemeAdapter::Install(*qApp);LegacyEditor.resize(1100,720);LegacyEditor.show();QTest::qWait(100);QVERIFY(LegacyEditor.grab().save(QDir(Evidence).filePath("standard-diagnostics.png")));}
        QVERIFY(Original.open(QIODevice::ReadOnly));QCOMPARE(Original.readAll(),Bytes);qInfo()<<"Official EPUBCheck validation, CSS diagnostic navigation, cancellation, stale results and strict Save As verified";
    }
    void epubEditorWindowResize() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.resize(1000,700);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",qEnvironmentVariable("HC_EPUB_READER_EPUB")}})["documentId"].toString();
        const auto View=[&]{return document(Window,Id)["view"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(View()["epub"].toObject()["readingStateLoaded"].toBool(),60000);
        QVERIFY(foreground(Window));QVERIFY(!Window.Handle({{"action","epub.edit.begin"},{"documentId",Id}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto P=View()["epubEditor"].toObject()["preview"].toObject();if(P["waitingForFocus"].toBool())foreground(Window);return P["ready"].toBool()&&!P["busy"].toBool();})(),60000);
        QVERIFY(foreground(Window));
        const auto Dpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);const auto RestoreDpi=qScopeGuard([Dpi]{SetThreadDpiAwarenessContext(Dpi);});
        const auto Root=reinterpret_cast<HWND>(Window.winId());RECT Bounds{};QVERIFY(GetWindowRect(Root,&Bounds));
        const int Left=Bounds.left+2,Right=Bounds.right-2,Top=Bounds.top+2,Bottom=Bounds.bottom-2,MidX=(Left+Right)/2,MidY=(Top+Bottom)/2;
        const QList<QPair<QPoint,int>> Edges{{{Left,MidY},HTLEFT},{{Right,MidY},HTRIGHT},{{MidX,Top},HTTOP},{{MidX,Bottom},HTBOTTOM},{{Left,Top},HTTOPLEFT},{{Right,Top},HTTOPRIGHT},{{Left,Bottom},HTBOTTOMLEFT},{{Right,Bottom},HTBOTTOMRIGHT}};
        for(const auto& Edge:Edges) {
            POINT Point{Edge.first.x(),Edge.first.y()};const auto Target=WindowFromPoint(Point);DWORD_PTR Hit=0;
            QCOMPARE(GetAncestor(Target,GA_ROOT),Root);
            QVERIFY(SendMessageTimeoutW(Target,WM_NCHITTEST,0,MAKELPARAM(Point.x,Point.y),SMTO_ABORTIFHUNG,2000,&Hit));
            qInfo()<<"Native edge root/target/hit"<<quintptr(Root)<<quintptr(Target)<<Hit;QCOMPARE(int(Hit),Edge.second);
        }
        // A real non-client mouse drag exercises the path that QWidget::resize
        // bypasses. Release the mouse from a worker while Windows runs its modal loop.
        POINT Cursor{};GetCursorPos(&Cursor);const auto BeforeDrag=Window.size();bool Sent=false;
        std::thread Drag([&]{
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            SetCursorPos(Bounds.right-2,(Bounds.top+Bounds.bottom)/2);Sleep(80);
            INPUT Mouse{};Mouse.type=INPUT_MOUSE;Mouse.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;const auto Down=SendInput(1,&Mouse,sizeof(Mouse));Sleep(100);
            SetCursorPos(Bounds.right+118,(Bounds.top+Bounds.bottom)/2);Sleep(150);
            Mouse.mi.dwFlags=MOUSEEVENTF_LEFTUP;Sent=SendInput(1,&Mouse,sizeof(Mouse))==1 && Down==1;
        });QTest::qWait(650);Drag.join();SetCursorPos(Cursor.x,Cursor.y);QVERIFY(Sent);
        qInfo()<<"Native resize drag before/after"<<BeforeDrag<<Window.size();QVERIFY(Window.width()>BeforeDrag.width()+20);
        qInfo()<<"Edit initial size/min/max/hint"<<Window.size()<<Window.minimumSize()<<Window.maximumSize()<<Window.minimumSizeHint();
        for(const auto Target:{QSize(1300,900),QSize(900,650),QSize(1100,750),QSize(800,600),QSize(1300,900)}) {
            Window.resize(Target);QTest::qWait(500);qInfo()<<"Resize target/actual/min"<<Target<<Window.size()<<Window.minimumSize();QCOMPARE(Window.size(),Target);
        }
        Window.showMaximized();QTest::qWait(250);QCOMPARE(Window.layout()->contentsMargins(),QMargins{});
        Window.showNormal();QTest::qWait(250);QCOMPARE(Window.layout()->contentsMargins(),QMargins(6,6,6,6));
        Window.showFullScreen();QTest::qWait(250);QCOMPARE(Window.layout()->contentsMargins(),QMargins{});
        Window.showNormal();QTest::qWait(250);QCOMPARE(Window.layout()->contentsMargins(),QMargins(6,6,6,6));
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void sharedPreviewValidation() {
        const auto Path=qEnvironmentVariable("HC_EPUB_SAVE_BOOK");if(Path.isEmpty())QSKIP("Large save corpus required to overlap exports");
        QFile Input(Path);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Bytes=Input.readAll();Input.close();QVERIFY(Bytes.size()>10000000);
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        for(int Scenario=0;Scenario<3;++Scenario) {
            EpubSourceEditor Editor(Bytes,Temp.filePath(QString::number(Scenario)+".epub"));
            QTRY_VERIFY_WITH_TIMEOUT(Editor.Snapshot()["ready"].toBool(),30000);
            Editor.ConfigurePreview(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")),0);
            Editor.findChild<QCheckBox*>("epubAutoPreview")->setChecked(false);
            Editor.resize(1200,800);Editor.show();QVERIFY(foreground(Editor));
            auto* Text=Editor.findChild<QPlainTextEdit*>("epubEditSource");auto* Refresh=Editor.findChild<QPushButton*>("epubPreviewRefresh");QVERIFY(Text && Refresh);
            Text->moveCursor(QTextCursor::End);Text->insertPlainText("<!-- shared export -->");
            const auto Before=Editor.Snapshot()["exportWork"].toObject();
            if(Scenario==1){Editor.Execute({{"action","epub.edit.validate"}});Refresh->click();}
            else {Refresh->click();Editor.Execute({{"action","epub.edit.validate"}});}
            const auto Work=Editor.Snapshot()["exportWork"].toObject();
            QCOMPARE(Work["runs"].toInteger(),Before["runs"].toInteger()+1);
            QCOMPARE(Work["joins"].toInteger(),Before["joins"].toInteger()+1);
            if(Scenario==2)Text->insertPlainText("<!-- superseded -->");
            Editor.RequestPreviewClose();
            QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["validating"].toBool(),30000);
            QCOMPARE(Editor.IsValidated(),Scenario!=2);
            QTRY_VERIFY_WITH_TIMEOUT(Editor.RequestPreviewClose(),30000);
        }
        QVERIFY(Input.open(QIODevice::ReadOnly));QCOMPARE(Input.readAll(),Bytes);
        qInfo()<<"Both export join directions, preview close and stale revision rejection verified";
    }
    void livePreviewEditing() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());const auto Source=Temp.filePath("input.epub");QVERIFY(QFile::copy(Corpus,Source));QFile Input(Source);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();Input.close();epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Spine=Model.Publication()["spine"].toArray();QVERIFY(Spine.size()>1);const auto First=Spine.first().toObject()["path"].toString();QString Css,Png;
        for(const auto& V:Model.Publication()["resources"].toArray()){const auto R=V.toObject();if(R["mediaType"]=="text/css")Css=R["path"].toString();if(R["mediaType"]=="image/png")Png=R["path"].toString();}QVERIFY(!Css.isEmpty() && !Png.isEmpty());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.resize(1600,1000);Window.move(80,80);Window.show();const auto Id=Window.Handle({{"action","open"},{"path",Source}})["documentId"].toString();
        const auto View=[&]{return document(Window,Id)["view"].toObject();};const auto Editor=[&]{return View()["epubEditor"].toObject();};const auto Preview=[&]{return Editor()["preview"].toObject();};const auto Page=[&]{return Preview()["state"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(View()["epub"].toObject()["readingStateLoaded"].toBool(),60000);QVERIFY(!Window.Handle({{"action","epub.reader.chapter"},{"documentId",Id},{"value",1}}).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(View()["epub"].toObject()["chapterIndex"]==QJsonValue(1) && !View()["epub"].toObject()["readerBusy"].toBool(),15000);
        QCOMPARE(View()["epubMode"].toString(),QString("reader"));QVERIFY(View()["readOnly"].toBool());QVERIFY(Editor().isEmpty());QVERIFY(!Window.findChild<QAction*>("saveDocumentAs")->isEnabled());
        auto* Begin=Window.findChild<QAction*>("epubEdit");QVERIFY(Begin && !Begin->isChecked());QVERIFY(foreground(Window));Begin->trigger();QCOMPARE(GetAncestor(GetForegroundWindow(),GA_ROOT),reinterpret_cast<HWND>(Window.winId()));
        QCOMPARE(View()["epubMode"].toString(),QString("edit"));QVERIFY(!View()["readOnly"].toBool());// The asynchronous load can outlive the test's initial activation. Resume
        // the explicitly requested preview if another desktop window intervened.
        // Passive background behavior is tested separately below.
        QTRY_VERIFY2_WITH_TIMEOUT(([&]{if(Preview()["waitingForFocus"].toBool())foreground(Window);return Preview()["current"].toBool() && Preview()["ready"].toBool() && !Preview()["busy"].toBool();})(),qPrintable(QString::fromUtf8(QJsonDocument(Preview()).toJson(QJsonDocument::Compact))),60000);QCOMPARE(Page()["chapterIndex"].toInt(),1);QCOMPARE(Editor()["current"].toString(),Spine[1].toObject()["path"].toString());QVERIFY(Page()["profile"].toObject()["privateReading"].toBool());QVERIFY(!Page()["profile"].toObject()["override"].toBool());QVERIFY(!Page()["settingsResult"].toObject().contains("error"));
        auto* Resources=Window.findChild<QComboBox*>("epubEditResources");auto* Text=Window.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Resources && Text);Resources->setCurrentIndex(Resources->findText(First));QTRY_VERIFY_WITH_TIMEOUT(Page()["chapterIndex"].toInt()==0 && !Page()["readerBusy"].toBool(),15000);{
            const auto Gate=qEnvironmentVariable("HC_EPUB_PREVIEW_ACTIVATION_GATE");if(!Gate.isEmpty()){QFile Ready(Gate+".ready");QVERIFY(Ready.open(QIODevice::WriteOnly));Ready.write(QJsonDocument(QJsonObject{{"pid",qint64(GetCurrentProcessId())},{"window",QString::number(quintptr(Window.winId()))}}).toJson());Ready.close();QElapsedTimer Wait;Wait.start();while(!QFile::exists(Gate) && Wait.elapsed()<120000)QTest::qWait(50);QVERIFY(QFile::exists(Gate));}
        }const bool Activated=foreground(Window);QVERIFY(!Window.Handle({{"action","focus"}}).contains("error"));Text->setFocus();const bool InitialFocus=Text->hasFocus();bool ReplacementFocus=false;QString FocusDetail;
        connect(qApp,&QApplication::focusChanged,&Window,[](QWidget* Before,QWidget* After){if(qEnvironmentVariableIsEmpty("HC_EPUB_NATIVE_TRACE"))return;qInfo()<<"EPUB preview focus change"<<(Before?Before->objectName():"none")<<(After?After->objectName():"none");});
        QTimer NativeTrace;NativeTrace.setInterval(10);QByteArray LastNative;QElapsedTimer NativeElapsed;NativeElapsed.start();
        connect(&NativeTrace,&QTimer::timeout,&Window,[&]{
            const auto Foreground=GetForegroundWindow();wchar_t Class[128]{};GetClassNameW(Foreground,Class,128);DWORD Pid=0;GetWindowThreadProcessId(Foreground,&Pid);GUITHREADINFO Gui{sizeof(GUITHREADINFO)};GetGUIThreadInfo(GetCurrentThreadId(),&Gui);
            QJsonArray Readers;for(const auto* Reader:Window.findChildren<EpubProcessView*>()){const auto S=Reader->ViewState();Readers.append(QJsonObject{{"pid",S["processId"]},{"status",S["status"]},{"retired",S["retired"]}});}
            const auto Row=QJsonDocument(QJsonObject{{"class",QString::fromWCharArray(Class)},{"style",QString::number(GetWindowLongPtrW(Foreground,GWL_STYLE),16)},{"exStyle",QString::number(GetWindowLongPtrW(Foreground,GWL_EXSTYLE),16)},{"root",QString::number(quintptr(GetAncestor(Foreground,GA_ROOT)))},{"pid",int(Pid)},{"foreground",QString::number(quintptr(Foreground))},{"active",QString::number(quintptr(Gui.hwndActive))},{"focus",QString::number(quintptr(Gui.hwndFocus))},{"qtFocus",QApplication::focusWidget()?QApplication::focusWidget()->objectName():"none"},{"readers",Readers}}).toJson(QJsonDocument::Compact);
            if(Row!=LastNative){LastNative=Row;qInfo().noquote()<<"EPUB native transition"<<NativeElapsed.elapsed()<<Row;}
        });if(!qEnvironmentVariableIsEmpty("HC_EPUB_NATIVE_TRACE"))NativeTrace.start();
        auto Html=QString::fromUtf8(Model.Read(First));Html.replace("</body>","<p id=\"preview-added\">저장 전 미리보기 검증 문단</p></body>");Text->selectAll();Text->insertPlainText(Html);
        QTRY_VERIFY_WITH_TIMEOUT(Preview()["current"].toBool() && Page()["text"].toString().contains("저장 전 미리보기 검증 문단") && !Preview()["busy"].toBool(),45000);{DWORD ForegroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&ForegroundPid);const auto Detail=QString("Preview focus: pid=%1 own=%2 widget=%3 readerFocused=%4").arg(ForegroundPid).arg(GetCurrentProcessId()).arg(QApplication::focusWidget()?QApplication::focusWidget()->objectName():"none").arg(Page()["readerFocused"].toBool());ReplacementFocus=Text->hasFocus();FocusDetail=Detail;}
        if(Activated && InitialFocus && ReplacementFocus) {
            const QString Keystrokes="nativeinput",Expected="<!--"+Keystrokes+"-->";auto NativeCursor=Text->textCursor();NativeCursor.movePosition(QTextCursor::End);NativeCursor.insertText("<!---->");NativeCursor.movePosition(QTextCursor::Left,QTextCursor::MoveAnchor,3);Text->setTextCursor(NativeCursor);QList<INPUT> Keys;
            for(const auto Ch:Keystrokes){INPUT Down{};Down.type=INPUT_KEYBOARD;Down.ki.wScan=Ch.unicode();Down.ki.dwFlags=KEYEVENTF_UNICODE;Keys.append(Down);Down.ki.dwFlags|=KEYEVENTF_KEYUP;Keys.append(Down);}
            QCOMPARE(SendInput(UINT(Keys.size()),Keys.data(),sizeof(INPUT)),UINT(Keys.size()));
            const auto InputDetail=[&]{GUITHREADINFO Gui{sizeof(GUITHREADINFO)};GetGUIThreadInfo(GetCurrentThreadId(),&Gui);return QString("actualTail=%1 qt=%2 focus=%3 target=%4 active=%5").arg(Text->toPlainText().right(100)).arg(QApplication::focusWidget()?QApplication::focusWidget()->objectName():"none").arg(quintptr(Gui.hwndFocus)).arg(quintptr(Text->effectiveWinId())).arg(quintptr(Gui.hwndActive));};
            QTRY_VERIFY2_WITH_TIMEOUT(Text->toPlainText().endsWith(Expected),qPrintable(InputDetail()),5000);Html+=Expected;
            QTRY_VERIFY2_WITH_TIMEOUT(Preview()["current"].toBool() && !Preview()["busy"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(Preview()).toJson(QJsonDocument::Compact))),15000);QVERIFY(Text->hasFocus());
        }
        QVERIFY(Window.findChild<QAction*>("saveDocumentAs")->isEnabled());
        const auto Replace=[&](const QString& Path,const QString& Value){return Window.Handle({{"action","epub.edit.replace"},{"documentId",Id},{"resource",Path},{"text",Value}});};QWidget Other;Other.setWindowTitle("EPUB preview background focus verification");Other.resize(500,240);Other.move(40,40);Other.show();QVERIFY(foreground(Other));
        QVERIFY(!Replace(Css,QString::fromUtf8(Model.Read(Css))+"\np { margin-bottom: 31px !important; }").contains("error"));
        bool BackgroundPreserved=true;QElapsedTimer BackgroundWait;BackgroundWait.start();while(BackgroundWait.elapsed()<3500){QTest::qWait(20);if(GetAncestor(GetForegroundWindow(),GA_ROOT)!=reinterpret_cast<HWND>(Other.winId()))BackgroundPreserved=false;}
        QVERIFY(foreground(Window));Other.hide();QVERIFY2(BackgroundPreserved,"A passive EPUB preview activated its owner over the selected window");
        QImage Image(4,4,QImage::Format_RGB32);Image.fill(Qt::blue);QByteArray Pixels;QBuffer Buffer(&Pixels);QVERIFY(Buffer.open(QIODevice::WriteOnly));QVERIFY(Image.save(&Buffer,"PNG"));QVERIFY(!Window.Handle({{"action","epub.edit.replace"},{"documentId",Id},{"resource",Png},{"base64",QString::fromLatin1(Pixels.toBase64())}}).contains("error"));
        const auto HasImage=[&]{for(const auto& Value:Page()["images"].toArray())if(Value.toObject()["naturalWidth"].toInt()==4)return true;return false;};QTRY_VERIFY_WITH_TIMEOUT(Preview()["current"].toBool() && HasImage() && !Preview()["busy"].toBool(),45000);QCOMPARE(Page()["paragraphs"].toArray().first().toObject()["after"].toString(),QString("31px"));QCOMPARE(Editor()["current"].toString(),Png);
        Resources->setCurrentIndex(Resources->findText(First));const auto Stable=Page()["frameId"].toString();Text->selectAll();Text->insertPlainText("<html>unfinished");QTRY_VERIFY_WITH_TIMEOUT(!Preview()["error"].toString().isEmpty() && !Preview()["busy"].toBool(),15000);QCOMPARE(Page()["frameId"].toString(),Stable);QVERIFY(!Preview()["current"].toBool());QVERIFY(document(Window,Id)["dirty"].toBool());
        Text->selectAll();Text->insertPlainText(Html.replace("검증 문단","두 번째 문단"));QTRY_VERIFY_WITH_TIMEOUT(Preview()["current"].toBool() && Page()["text"].toString().contains("두 번째 문단") && !Preview()["busy"].toBool(),45000);
        auto* EditAction=Window.findChild<QAction*>("epubEdit");QVERIFY(EditAction && EditAction->isChecked());EditAction->trigger();QVERIFY(!Editor()["sourceVisible"].toBool());QCOMPARE(View()["epubMode"].toString(),QString("reader"));QVERIFY(View()["readOnly"].toBool());QVERIFY(Window.Handle({{"action","epub.edit.replace"},{"documentId",Id},{"resource",First},{"text",Html}}).contains("error"));QVERIFY(!Window.Handle({{"action","epub.reader.next-chapter"},{"documentId",Id}}).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Page()["chapterIndex"].toInt()==1 && !Page()["readerBusy"].toBool(),15000);QTRY_COMPARE_WITH_TIMEOUT(Editor()["current"].toString(),Spine[1].toObject()["path"].toString(),15000);EditAction->trigger();QVERIFY(Editor()["sourceVisible"].toBool());
        const auto Evidence=qEnvironmentVariable("HC_EPUB_PREVIEW_OUTPUT");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);for(const auto& Theme:QStringList{"light","dark"}){auto Appearance=Window.ChromeSettings();Appearance["themeMode"]=Theme;QVERIFY(Window.ApplyChromeSettings(Appearance));const QJsonValue Variant=hc::document::epubui::Appearance()["variant"];QTRY_VERIFY_WITH_TIMEOUT(Page()["appearance"].toObject()["variant"]==Variant && !Page()["readerBusy"].toBool(),10000);QCOMPARE(Page()["profile"].toObject()["theme"].toString(),QString("light"));const auto ExpectedWindow=fm::style::ThemeManager::instance().colors().toPalette().color(QPalette::Window).name();QCOMPARE(Page()["chromeWindowColor"].toString(),ExpectedWindow);QCOMPARE(Page()["toolbarWindowColor"].toString(),ExpectedWindow);QTest::qWait(150);const auto Point=Window.mapToGlobal(QPoint{});QVERIFY(Window.screen()->grabWindow(0,Point.x(),Point.y(),Window.width(),Window.height()).save(QDir(Evidence).filePath("preview-"+Theme+".png")));}}
        qInfo()<<"EPUB preview functionality verified"<<"activation"<<Activated<<"initialFocus"<<InitialFocus<<"replacementFocus"<<ReplacementFocus<<FocusDetail;
        const auto Records=QDir(Temp.filePath("epub-state")).entryList({"*.json"},QDir::Files);QCOMPARE(Records,QStringList{QString::fromLatin1(QCryptographicHash::hash(Original,QCryptographicHash::Sha256).toHex())+".json"});QVERIFY(Input.open(QIODevice::ReadOnly));QCOMPARE(Input.readAll(),Original);Input.close();QVERIFY(!QFile::exists(Temp.filePath("epub-reading.json")));
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        QVERIFY2(Activated && InitialFocus && ReplacementFocus,qPrintable(FocusDetail));
    }
    void draftStoreIntegrity() {
        QTemporaryDir Temp;const auto Root=Temp.filePath("drafts");epub::DraftSnapshot Snapshot;Snapshot.sourcePath_=Temp.filePath("input.epub");Snapshot.original_="original ZIP bytes";Snapshot.hasCurrentDraft_=true;Snapshot.currentResource_="OEBPS/chapter.xhtml";Snapshot.currentDraft_="<html>unfinished";Snapshot.changes_["OEBPS/style.css"]="p { color: red; }";Snapshot.changes_["OEBPS/img.png"]=QByteArray::fromHex("89504e4700010200");Snapshot.cursor_=7;Snapshot.locator_="epubcfi(/6/2!/4/2:0)";
        const auto Session=QUuid::createUuid().toString(QUuid::Id128);const auto Written=epub::DraftStore::Write(Root,Session,Snapshot);QVERIFY2(Written,qPrintable(Written.error_));
        const auto Read=epub::DraftStore::Read(Root,Written.path_);QVERIFY(Read);QCOMPARE(Read.snapshot_.original_,Snapshot.original_);QCOMPARE(Read.snapshot_.currentDraft_,Snapshot.currentDraft_);QCOMPARE(Read.snapshot_.changes_,Snapshot.changes_);QCOMPARE(Read.snapshot_.cursor_,7);
        const auto Empty=QUuid::createUuid().toString(QUuid::Id128);Snapshot.currentDraft_.clear();QVERIFY(epub::DraftStore::Write(Root,Empty,Snapshot));QCOMPARE(epub::DraftStore::Read(Root,QDir(Root).filePath(Empty+".hcedraft")).snapshot_.currentDraft_,QByteArray{});
        QVERIFY(!epub::DraftStore::Write(Root,"../escape",Snapshot));QVERIFY(!QFile::exists(Temp.filePath("escape.hcedraft")));
        QFile File(Written.path_);QVERIFY(File.open(QIODevice::ReadWrite));const auto Bytes=File.readAll();QVERIFY(File.seek(Bytes.size()-1));File.write(QByteArray(1,char(Bytes.back()^1)));File.close();QVERIFY(!epub::DraftStore::Read(Root,Written.path_));QVERIFY(QFile::exists(Written.path_));
        const auto Other=QDir(Root).filePath(Empty+".hcedraft");QVERIFY(epub::DraftStore::Remove(Root,Written.path_));QVERIFY(QFile::exists(Other));QCOMPARE(epub::DraftStore::List(Root).size(),1);
    }
    void draftRecoveryOverlays() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());QFile File(Corpus);QVERIFY(File.open(QIODevice::ReadOnly));const auto Original=File.readAll();epub::EditModel Model;QVERIFY(Model.Load(Original));
        epub::DraftSnapshot Snapshot;Snapshot.sourcePath_=QFileInfo(Corpus).absoluteFilePath();Snapshot.original_=Original;QString ImagePath,Css;
        for(const auto& Value:Model.Publication()["resources"].toArray()){const auto Resource=Value.toObject();if(Resource["mediaType"]=="image/png")ImagePath=Resource["path"].toString();if(Resource["mediaType"]=="text/css")Css=Resource["path"].toString();}
        QVERIFY(!ImagePath.isEmpty() && !Css.isEmpty());QImage Image(4,4,QImage::Format_RGB32);Image.fill(Qt::magenta);QByteArray Pixels;QBuffer Buffer(&Pixels);QVERIFY(Buffer.open(QIODevice::WriteOnly));QVERIFY(Image.save(&Buffer,"PNG"));Snapshot.changes_[ImagePath]=Pixels;Snapshot.changes_[Css]="p { margin-bottom: 2em; }";
        const auto Root=EpubSourceEditor::RecoveryDirectory();const auto Backup=epub::DraftStore::Write(Root,QUuid::createUuid().toString(QUuid::Id128),Snapshot);QVERIFY(Backup);
        EpubSourceEditor Editor(Original,Snapshot.sourcePath_);QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),30000);Editor.Execute({{"action","epub.edit.recover"},{"path",Backup.path_}});QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady() && !Editor.Snapshot()["recoveryBusy"].toBool() && Editor.Token()!=QCryptographicHash::hash(Original,QCryptographicHash::Sha256),30000);
        Editor.Execute({{"action","epub.edit.validate"}});QTRY_VERIFY_WITH_TIMEOUT(Editor.IsValidated(),30000);const auto Output=Editor.Export(Temp.filePath("new.epub"));QVERIFY(Output.HasValue());epub::EditModel Restored;QVERIFY(Restored.Load(Output.Value()));QCOMPARE(Restored.Read(ImagePath),Pixels);QCOMPARE(Restored.Read(Css),Snapshot.changes_[Css]);
        Editor.SetRecoveryEnabled(false);Editor.Checkpoint();QVERIFY(QFile::exists(Backup.path_));Editor.SetRecoveryEnabled(true);Editor.Checkpoint();QTRY_VERIFY_WITH_TIMEOUT(Editor.Snapshot()["recoverySaved"].toBool(),10000);
        EpubSourceEditor Second(Original,Snapshot.sourcePath_);QTRY_VERIFY_WITH_TIMEOUT(Second.IsReady(),30000);auto* Source=Second.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Source);Source->setPlainText("");Source->document()->setModified(true);Second.Checkpoint();QTRY_VERIFY_WITH_TIMEOUT(Second.Snapshot()["recoverySaved"].toBool(),10000);const auto Other=Second.Snapshot()["recoveryPath"].toString();QVERIFY(Other!=Editor.Snapshot()["recoveryPath"].toString());const auto Empty=epub::DraftStore::Read(Root,Other);QVERIFY(Empty);QVERIFY(Empty.snapshot_.hasCurrentDraft_ && Empty.snapshot_.currentDraft_.isEmpty());
        Editor.DiscardRecovery();QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(Backup.path_),10000);QVERIFY(QFile::exists(Other));Second.DiscardRecovery();
    }
    void draftCrashRecovery() {
        const auto Child=qEnvironmentVariable("HC_EPUB_DRAFT_CHILD");
        if(!Child.isEmpty()) {
            qApp->setProperty("documentViewsConfigDir",Child);QFile File(QDir(Child).filePath("input.epub"));QVERIFY(File.open(QIODevice::ReadOnly));EpubSourceEditor Editor(File.readAll(),File.fileName());Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),30000);
            auto* Text=Editor.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Text);Text->setPlainText("<html>unfinished crash draft");Text->document()->setModified(true);Editor.Checkpoint();QTRY_VERIFY_WITH_TIMEOUT(Editor.Snapshot()["recoverySaved"].toBool(),10000);
            TerminateProcess(GetCurrentProcess(),37);QFAIL("Termination failed");
        }
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");QTemporaryDir Temp;const auto Source=Temp.filePath("input.epub");QVERIFY(QFile::copy(Corpus,Source));
        QProcess Crash;auto Env=QProcessEnvironment::systemEnvironment();Env.insert("HC_EPUB_DRAFT_CHILD",Temp.path());Crash.setProcessEnvironment(Env);Crash.start(QCoreApplication::applicationFilePath(),{"draftCrashRecovery"});QTRY_VERIFY_WITH_TIMEOUT(Crash.state()==QProcess::NotRunning,45000);QCOMPARE(Crash.exitCode(),37);
        qApp->setProperty("documentViewsConfigDir",Temp.path());const auto Root=EpubSourceEditor::RecoveryDirectory();auto Entries=epub::DraftStore::List(Root);QCOMPARE(Entries.size(),1);const auto Recovery=Entries.first().path_;
        QFile File(Source);QVERIFY(File.open(QIODevice::ReadOnly));const auto Original=File.readAll();File.close();epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Opf=Model.Publication()["rootfile"].toString();auto Metadata=Model.Read(Opf);Metadata.replace(Model.Publication()["title"].toString().toUtf8(),"Externally changed");QVERIFY(Model.Replace(Opf,Metadata).isEmpty());const auto Revised=Model.Export();QVERIFY(Revised);QVERIFY(File.open(QIODevice::WriteOnly|QIODevice::Truncate));File.write(Revised.bytes_);File.close();
        EpubSourceEditor Editor(Revised.bytes_,Source);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady() && Editor.Snapshot()["recoveryCandidates"].toInt()>0,30000);QVERIFY(!Editor.Execute({{"action","epub.edit.recover"},{"path",Recovery}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady() && Editor.Snapshot()["recoverySourceChanged"].toBool(),30000);auto* Text=Editor.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(Text);QCOMPARE(Text->toPlainText(),QString("<html>unfinished crash draft"));Editor.Execute({{"action","epub.edit.validate"}});QTRY_VERIFY_WITH_TIMEOUT(!Editor.Snapshot()["validating"].toBool(),10000);QVERIFY(!Editor.IsValidated());QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Revised.bytes_);File.close();
        Editor.Checkpoint();QTRY_VERIFY_WITH_TIMEOUT(Editor.Snapshot()["recoverySaved"].toBool(),10000);const auto Own=Editor.Snapshot()["recoveryPath"].toString();QVERIFY(Own!=Recovery);QVERIFY(QFile::exists(Recovery));Editor.DiscardRecovery();QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(Own) && !QFile::exists(Recovery),10000);
        // A deleted source is restored through a private copy owned by the workspace.
        epub::DraftSnapshot Snapshot;Snapshot.sourcePath_=Source;Snapshot.original_=Original;Snapshot.currentResource_=Model.Publication()["spine"].toArray().last().toObject()["path"].toString();Snapshot.hasCurrentDraft_=true;Snapshot.currentDraft_="<html>deleted source draft";
        const auto Saved=epub::DraftStore::Write(Root,QUuid::createUuid().toString(QUuid::Id128),Snapshot);QVERIFY(Saved);QVERIFY(QFile::remove(Source));
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.show();QVERIFY(!Window.Handle({{"action","epub.recovery.open"},{"path",Saved.path_}}).contains("error"));
        const auto Restored=[&]{const auto Rows=Window.Snapshot()["documents"].toArray();return Rows.isEmpty()?QJsonObject{}:Rows.first().toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(Restored()["view"].toObject()["ready"].toBool(),60000);QVERIFY(Restored()["view"].toObject()["readOnly"].toBool());QVERIFY(Restored()["view"].toObject()["epubEditor"].toObject().isEmpty());
        auto* Edit=Window.findChild<QAction*>("epubEdit");QVERIFY(Edit);QVERIFY(foreground(Window));Edit->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Restored()["view"].toObject()["epubEditor"].toObject()["recoverySourceChanged"].toBool(),60000);QVERIFY(Restored()["dirty"].toBool());QTest::qWait(1000);
        auto* RecoveredText=Window.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(RecoveredText);QCOMPARE(RecoveredText->toPlainText(),QString("<html>deleted source draft"));QCOMPARE(Restored()["view"].toObject()["epubEditor"].toObject()["current"].toString(),Snapshot.currentResource_);
        QVERIFY(!QFile::exists(Source));QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void startupAndUnchangedProfile() {
        const auto Book=qEnvironmentVariable("HC_EPUB_BENCH_BOOK",qEnvironmentVariable("HC_EPUB_READER_EPUB"));if(Book.isEmpty())QSKIP("Explicit book required");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        for(int Round=0;Round<2;++Round) {
            DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();QElapsedTimer Elapsed;Elapsed.start();
            const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
            QTRY_VERIFY2_WITH_TIMEOUT(State()["status"]=="ready" && State()["readingStateLoaded"].toBool() && !State()["readerBusy"].toBool() && !State()["locator"].toString().isEmpty(),qPrintable(QString::fromUtf8(QJsonDocument(State()).toJson(QJsonDocument::Compact))),60000);
            qInfo().noquote()<<"EPUB stable startup"<<QJsonDocument(QJsonObject{{"round",Round},{"elapsedMs",Elapsed.elapsed()},{"metrics",State()["metrics"]},{"startup",State()["startup"]}}).toJson(QJsonDocument::Compact);
            // Font/status chrome can deliver a final viewport resize after
            // first display. Isolate the no-op profile from that independent work.
            QByteArray LastGeometry;int StableGeometry=0;QElapsedTimer GeometryWait;GeometryWait.start();
            while(StableGeometry<4 && GeometryWait.elapsed()<5000){QTest::qWait(100);const auto S=State();const auto Geometry=QJsonDocument(QJsonArray{S["paneWidth"],S["paneHeight"],S["frameId"],S["locator"]}).toJson(QJsonDocument::Compact);StableGeometry=!S["readerBusy"].toBool() && Geometry==LastGeometry?StableGeometry+1:0;LastGeometry=Geometry;}
            QVERIFY2(StableGeometry>=4,"The initial reader viewport did not settle");
            const auto BeforeIdle=State()["metrics"].toObject();QTest::qWait(750);const auto AfterIdle=State()["metrics"].toObject();
            const auto Replies=AfterIdle["snapshotReplies"].toInteger()-BeforeIdle["snapshotReplies"].toInteger();
            QVERIFY(Replies>0);qInfo()<<"Idle snapshot bytes per reply"<<(AfterIdle["snapshotReplyBytes"].toInteger()-BeforeIdle["snapshotReplyBytes"].toInteger())/Replies<<"full host state bytes"<<QJsonDocument(State()).toJson(QJsonDocument::Compact).size();
            const auto Frame=State()["frameId"].toString(),Locator=State()["locator"].toString();const auto Skips=State()["startup"].toObject()["profileSkips"].toInt(-1),Applications=State()["startup"].toObject()["profileApplications"].toInt();
            QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",State()["profile"]}}).contains("error"));
            if(Skips>=0)QTRY_VERIFY_WITH_TIMEOUT(State()["startup"].toObject()["profileSkips"].toInt()>Skips && !State()["readerBusy"].toBool(),10000);else QTest::qWait(700);
            QCOMPARE(State()["frameId"].toString(),Frame);QCOMPARE(State()["locator"].toString(),Locator);if(Skips>=0)QCOMPARE(State()["startup"].toObject()["profileApplications"].toInt(),Applications);
            QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
    }
    void readerFrameTransitions() {
        const auto Output=qEnvironmentVariable("HC_EPUB_FRAME_OUTPUT"),Book=qEnvironmentVariable("HC_EPUB_STRESS_BOOK");if(Output.isEmpty() || Book.isEmpty())QSKIP("Explicit frame evidence directory and book required");
        QVERIFY(QDir().mkpath(Output));QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1400,960);Window.move(80,80);Window.show();Window.raise();Window.activateWindow();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        const auto Ready=[&]{return State()["status"]=="ready" && State()["frameCount"].toInt()>0 && !State()["readerBusy"].toBool();};
        const auto Do=[&](const QString& Action){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id}});};
        QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool() && Ready(),60000);
        QVERIFY(foreground(Window));QJsonArray Evidence;
        for(const auto& Theme:QStringList{"light","dark"}) {
            QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"layout","paginated"},{"columns",1},{"theme",Theme}}}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(State()["profile"].toObject()["theme"]==Theme && Ready(),20000);
            for(int I=0;I<10 && State()["chapterPages"].toInt()<4;++I){const auto Chapter=State()["chapterIndex"].toInt();QVERIFY(!Do("next-chapter").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(State()["chapterIndex"].toInt()!=Chapter && Ready(),15000);}
            QVERIFY(State()["chapterPages"].toInt()>=4);
            for(const auto& Action:QStringList{"next","next-chapter","resize"}) {

                const auto Frame=State()["frameId"].toString();const auto Chapter=State()["chapterIndex"].toInt();const auto Page=State()["chapterPage"].toInt();
                bool Captured=true,Gap=false;int Tick=0;QTimer Capture;Capture.setInterval(20);QElapsedTimer Elapsed;Elapsed.start();
                connect(&Capture,&QTimer::timeout,&Capture,[&]{
                    const auto S=State();Gap|=S["status"]=="ready" && S["frameCount"].toInt()==0 && S["startup"].toObject()["retiredViews"].toInt()==0;
                    if(QList<int>{0,2,4,8,16,24}.contains(Tick)) {
                        const auto Point=Window.mapToGlobal(QPoint{});const auto Shot=Window.screen()->grabWindow(0,Point.x(),Point.y(),Window.width(),Window.height());
                        const auto Name=QString("%1-%2-%3.png").arg(Theme,Action).arg(Tick);
                        Captured&=!Shot.isNull() && Shot.save(QDir(Output).filePath(Name));
                        Evidence.append(QJsonObject{{"file",Name},{"elapsedMs",Elapsed.elapsed()},{"frames",S["frameCount"]},{"retired",S["startup"].toObject()["retiredViews"]},{"busy",S["readerBusy"]}});
                    }++Tick;
                });Capture.start();
                if(Action=="resize")Window.resize(Window.width()==1400?1240:1400,900);else QVERIFY(!Do(Action).contains("error"));
                QTest::qWait(550);QTRY_VERIFY_WITH_TIMEOUT(Ready(),15000);Capture.stop();QVERIFY(Captured);QVERIFY2(!Gap,"No rendered or retained page during transition");

                if(Action=="next"){QCOMPARE(State()["frameId"].toString(),Frame);QCOMPARE(State()["chapterIndex"].toInt(),Chapter);QCOMPARE(State()["chapterPage"].toInt(),Page+1);}
            }
        }
        QFile File(QDir(Output).filePath("frames.json"));QVERIFY(File.open(QIODevice::WriteOnly));File.write(QJsonDocument(Evidence).toJson());
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readerPanelsAndKeys() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus required");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1600,1000);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",QFileInfo(Corpus).dir().filePath("epub3-toc-fragments.epub")}})["documentId"].toString();
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        const auto Do=[&](const QString& Action){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id}});};
        QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool(),60000);
        auto* MainMenu=Window.findChild<QMenuBar*>("mainMenu");QVERIFY(MainMenu);
        const auto Menus=MainMenu->actions();
        QVERIFY(std::any_of(Menus.cbegin(),Menus.cend(),[](QAction* Action){return Action->text()==QString("EPUB") && Action->isVisible() && Action->menu() && !Action->menu()->actions().isEmpty();}));
        for(const auto& Name:QStringList{"toc","bookmarks","search","settings"})for(int I=0;I<2;++I) {
            QVERIFY(!Do("toggle-"+Name).contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["openPanel"].toString(),I==0?Name:QString{},10000);
        }
        QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"layout","paginated"},{"columns",1}}}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(State()["profile"].toObject()["layout"]=="paginated" && !State()["readerBusy"].toBool() && State()["chapterPages"].toInt()>8,20000);
        HWND Reader=nullptr;EnumChildWindows(reinterpret_cast<HWND>(Window.winId()),[](HWND Child,LPARAM Data)->BOOL {wchar_t Title[128]{};GetWindowTextW(Child,Title,128);if(QString::fromWCharArray(Title)==QString("EPUB 읽기"))*reinterpret_cast<HWND*>(Data)=Child;return TRUE;},reinterpret_cast<LPARAM>(&Reader));QVERIFY(Reader);
        QVERIFY(foreground(Window));
        auto* Process=Window.findChild<EpubProcessView*>();QVERIFY(Process);QVERIFY(Process->FocusInput());
        const bool Focused=QTest::qWaitFor([&]{return State()["readerFocused"].toBool();},10000);
        if(!Focused) {
            GUITHREADINFO Native{sizeof(Native)};GetGUIThreadInfo(GetWindowThreadProcessId(Reader,nullptr),&Native);
            qWarning()<<"Reader focus diagnostics"<<QApplication::focusWidget()<<QApplication::activePopupWidget()<<QApplication::activeWindow()<<quintptr(GetForegroundWindow())<<quintptr(Native.hwndFocus)<<quintptr(Reader)<<State()["startup"]<<State()["readerBusy"];
        }
        QVERIFY(Focused);
        DWORD Pid=0;const auto Thread=GetWindowThreadProcessId(Reader,&Pid);
        const auto Key=[&](int Virtual,int Scan,bool Extended) {
            GUITHREADINFO Info{sizeof(Info)};if(!GetGUIThreadInfo(Thread,&Info))return false;
            const auto Target=Info.hwndFocus?Info.hwndFocus:Reader;const LPARAM Bits=1|(Scan<<16)|(Extended?(1<<24):0);
            return PostMessageW(Target,WM_KEYDOWN,Virtual,Bits) && PostMessageW(Target,WM_KEYUP,Virtual,Bits|(1LL<<30)|(1LL<<31));
        };
        const int InitialPage=State()["chapterPage"].toInt();QVERIFY(!Do("next").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(State()["chapterPage"].toInt()==InitialPage+1 && !State()["readerBusy"].toBool(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(State()["toolbarPreviousEnabled"].toBool() && State()["toolbarNextEnabled"].toBool(),10000);
        const auto StableToolbar=State()["toolbarAvailabilityChanges"].toInt();bool ToolbarStable=true;QTimer ToolbarPoll;ToolbarPoll.setInterval(10);
        connect(&ToolbarPoll,&QTimer::timeout,&Window,[&]{const auto S=State();ToolbarStable&=S["toolbarPreviousEnabled"].toBool() && S["toolbarNextEnabled"].toBool();});ToolbarPoll.start();
        for(bool Extended:{true,false}) {
            const int Page=State()["chapterPage"].toInt();QElapsedTimer Elapsed;Elapsed.start();
            QVERIFY(Key(VK_NEXT,0x51,Extended));QTRY_COMPARE_WITH_TIMEOUT(State()["chapterPage"].toInt(),Page+1,10000);
            qInfo()<<"Page-down observed ms"<<Extended<<Elapsed.elapsed();
            QTRY_VERIFY_WITH_TIMEOUT(!State()["readerBusy"].toBool(),10000);
            QVERIFY(Key(VK_PRIOR,0x49,Extended));QTRY_COMPARE_WITH_TIMEOUT(State()["chapterPage"].toInt(),Page,10000);
            QTRY_VERIFY_WITH_TIMEOUT(!State()["readerBusy"].toBool(),10000);
        }
        {
            const int Page=State()["chapterPage"].toInt();
            QVERIFY(Key(VK_NUMPAD3,0x51,false));QTRY_COMPARE_WITH_TIMEOUT(State()["chapterPage"].toInt(),Page+1,10000);
            QTRY_VERIFY_WITH_TIMEOUT(!State()["readerBusy"].toBool(),10000);
            QVERIFY(Key(VK_NUMPAD9,0x49,false));QTRY_COMPARE_WITH_TIMEOUT(State()["chapterPage"].toInt(),Page,10000);
            QTRY_VERIFY_WITH_TIMEOUT(!State()["readerBusy"].toBool(),10000);
        }
        ToolbarPoll.stop();QVERIFY2(ToolbarStable,"Previous/next toolbar buttons flashed disabled during page navigation");QCOMPARE(State()["toolbarAvailabilityChanges"].toInt(),StableToolbar);
        for(int Columns:{2,1}) {
            QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"columns",Columns}}}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(State()["toolbarColumns"].toInt()==Columns && State()["columnCount"].toInt()==Columns && !State()["readerBusy"].toBool(),20000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readingStress() {
        const auto Book=qEnvironmentVariable("HC_EPUB_STRESS_BOOK");if(Book.isEmpty())QSKIP("Explicit EPUB stress book required");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool(),60000);
        for(int I=0;I<240;++I) {
            if(I%6==0) {
                QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"layout","paginated"},{"columns",I%12==0?1:2}}}}).contains("error"));
                QTRY_COMPARE_WITH_TIMEOUT(State()["profile"].toObject()["columns"].toInt(),I%12==0?1:2,10000);
            }
            const auto Serial=State()["readerAction"].toObject()["serial"].toInteger();
            const auto Action=I%4==0?"next-chapter":I%4==2?"previous-chapter":"next";
            QVERIFY(!Window.Handle({{"action",QString("epub.reader.")+Action},{"documentId",Id}}).contains("error"));
            QTRY_VERIFY2_WITH_TIMEOUT(State()["status"]=="error" || State()["readerAction"].toObject()["serial"].toInteger()>Serial,qPrintable(QString::fromUtf8(QJsonDocument(State()).toJson(QJsonDocument::Compact))),15000);
            if(I%6==0 || State()["status"]=="error")qInfo().noquote()<<"EPUB stress"<<I<<QJsonDocument(State()).toJson(QJsonDocument::Compact);
            QVERIFY2(State()["status"]=="ready",qPrintable(QString::fromUtf8(QJsonDocument(State()).toJson(QJsonDocument::Compact))));
        }
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void unicodeTocFragments() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus not configured");
        const auto Book=QFileInfo(Corpus).dir().filePath("epub3-toc-fragments.epub");QVERIFY(QFile::exists(Book));
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);
        Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool(),60000);
        QVERIFY(!Window.Handle({{"action","epub.reader.toc"},{"documentId",Id},{"value",2}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(State()["chapterPage"].toInt()>1,15000);QTRY_COMPARE_WITH_TIMEOUT(State()["tocIndex"].toInt(),2,15000);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readerUxFlow() {
        const auto Book=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Book.isEmpty())QSKIP("Reader corpus not configured");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool() && !State()["readingStateRestoring"].toBool(),60000);
        const auto Do=[&](const QString& Action,QJsonValue Value=QJsonValue{}){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id},{"value",Value}});};
        QVERIFY(!Do("next-chapter").contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["chapterIndex"].toInt(),1,15000);
        QVERIFY(!Do("back").contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["chapterIndex"].toInt(),0,15000);
        QVERIFY(!Do("toc",1).contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["tocIndex"].toInt(),1,15000);
        QVERIFY(!Window.Handle({{"action","epub.settings.preview"},{"documentId",Id},{"profile",QJsonObject{{"layout","paginated"},{"columns",2}}}}).contains("error"));
        QTRY_COMPARE_WITH_TIMEOUT(State()["profile"].toObject()["layout"].toString(),QString("paginated"),15000);QTRY_VERIFY_WITH_TIMEOUT(!State()["frameBounds"].toArray().isEmpty(),15000);
        QVERIFY(!Window.Handle({{"action","epub.settings.cancel"},{"documentId",Id}}).contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["profile"].toObject()["layout"].toString(),QString("scroll"),15000);
        QVERIFY(!Window.Handle({{"action","preferences"}}).contains("error"));
        QPointer<DocumentSettingsDialog> Settings;for(auto* Widget:QApplication::topLevelWidgets())if(auto* Dialog=qobject_cast<DocumentSettingsDialog*>(Widget))Settings=Dialog;
        QVERIFY(Settings);QVERIFY(Settings->isVisible());QTest::qWait(1000);QVERIFY2(State()["status"]=="ready",qPrintable(QString::fromUtf8(QJsonDocument(State()).toJson())));Settings->reject();
        QVERIFY(!Do("fullscreen").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Window.isFullScreen(),10000);
        QVERIFY(!Do("fullscreen").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(!Window.isFullScreen(),10000);
        QTRY_COMPARE_WITH_TIMEOUT(State()["status"].toString(),QString("ready"),10000);
        QVERIFY(!Do("next").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(!State()["readerAction"].toObject()["busy"].toBool(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readerFailureAndRetry() {
        const auto Book=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Book.isEmpty())QSKIP("Reader corpus not configured");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);auto Runtime=testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION"));
        auto Denied=Runtime;Denied.approvedInventorySha256_=QByteArray(64,'0');Window.ConfigureEpub(Denied);Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(Window.findChild<QWidget*>("epubFailure") && Window.findChild<QWidget*>("epubFailure")->isVisible(),60000);
        auto* Retry=Window.findChild<QPushButton*>("epubRetry");QVERIFY(Retry);QVERIFY(!Window.findChild<QPlainTextEdit*>("epubFailureDetails")->toPlainText().isEmpty());
        Window.ConfigureEpub(Runtime);Retry->click();
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
        QVERIFY(Window.findChild<QWidget*>("epubFailure")->isHidden());
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void cleanup() {
        EpubProcessView::RequestShutdown();
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        EpubProcessView::ReleaseLifecycle();
    }
    void inputBudget() {
        QTemporaryDir Temp;QFile File(Temp.filePath("oversized.epub"));
        QVERIFY(File.open(QIODevice::WriteOnly));QVERIFY(File.resize(65LL*1024*1024));File.close();
        const auto Result=ReadDocumentFile(File.fileName(),{},true);
        QVERIFY(!Result.HasValue());QCOMPARE(QString::fromStdString(Result.Failure().code_),QString("epub-input-budget"));
    }
    void compatibilityBooks_data() {
        QTest::addColumn<QString>("Path");QTest::addColumn<int>("Missing");QTest::addColumn<bool>("Webp");
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");
        if(Corpus.isEmpty() && qEnvironmentVariableIsEmpty("HC_EPUB_COMPAT_INPUTS"))QSKIP("Explicit compatibility corpus not configured");
        if(!Corpus.isEmpty()) {
            const auto Dir=QFileInfo(Corpus).dir();
            QTest::newRow("legacy-mime")<<Dir.filePath("epub2-legacy-mime.epub")<<0<<false;
            QTest::newRow("legacy-font")<<Dir.filePath("epub3-legacy-font.epub")<<0<<false;
            QTest::newRow("webp")<<Dir.filePath("epub3-webp.epub")<<0<<true;
            QTest::newRow("recovered-spine")<<Dir.filePath("epub3-recovered-spine.epub")<<1<<true;
        }
        QFile Inputs(qEnvironmentVariable("HC_EPUB_COMPAT_INPUTS"));
        if(Inputs.open(QIODevice::ReadOnly)) {const auto Rows=QJsonDocument::fromJson(Inputs.readAll()).array();for(int I=0;I<Rows.size();++I)QTest::newRow(qPrintable(QString("real-%1").arg(I+1)))<<Rows[I].toObject()["path"].toString()<<Rows[I].toObject()["expectedMissingSpineItems"].toInt()<<Rows[I].toObject()["expectWebp"].toBool();}
    }
    void compatibilityBooks() {
        QFETCH(QString,Path);QFETCH(int,Missing);QFETCH(bool,Webp);
        QFile Source(Path);QVERIFY(Source.open(QIODevice::ReadOnly));const auto Original=Source.readAll();Source.close();
        epub::EditModel Model;QVERIFY2(Model.Load(Original),qPrintable(Model.Error()));
        QCOMPARE(Model.Publication()["recovery"].toObject()["missingSpineItems"].toInt(),Missing);
        if(Missing) {
            QCOMPARE(Model.Export().error_,QString("epub-edit-spine-repair-required"));
            if(Missing==1) {
                const auto Root=Model.Publication()["rootfile"].toString();auto Opf=Model.Read(Root);QVERIFY(Opf.contains("<itemref idref=\"absent\"/>"));Opf.replace("<itemref idref=\"absent\"/>","");
                QVERIFY(Model.Replace(Root,Opf).isEmpty());const auto Export=Model.Export();QVERIFY2(Export,qPrintable(Export.error_));epub::EditModel Reopened;QVERIFY(Reopened.Load(Export.bytes_));QCOMPARE(Reopened.Publication()["recovery"].toObject()["missingSpineItems"].toInt(),0);
            }
        } else QCOMPARE(Model.Export().bytes_,Original);
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Path}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        const auto View=[&]{return document(Window,Id)["view"].toObject();};const auto State=[&]{return View()["epub"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(View()["ready"].toBool() || State()["status"]=="error",60000);
        QVERIFY2(View()["ready"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(View()).toJson())));
        QTRY_VERIFY_WITH_TIMEOUT(!State()["frameBounds"].toArray().isEmpty() && State()["frameBounds"].toArray().first().toObject()["height"].toInt()>0,15000);
        QCOMPARE(State()["recovery"].toObject()["missingSpineItems"].toInt(),Missing);
        QVERIFY(!State()["text"].toString().trimmed().isEmpty() || !State()["images"].toArray().isEmpty());
        const auto Do=[&](const QString& Action,QJsonValue Value=QJsonValue{}){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id},{"value",Value}});};
        if(Missing) {QVERIFY(!Do("locate","epubcfi(/6/4!/4/2/1:0)").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(State()["readerAction"].toObject()["located"].toBool(),30000);QVERIFY(!State()["text"].toString().trimmed().isEmpty());}
        if(Webp) {
            const auto Find=[&]{const auto Images=State()["images"].toArray();for(int I=0;I<Images.size();++I){const auto Image=Images[I].toObject();if(Image["src"].toString().endsWith(".webp") && Image["complete"].toBool() && Image["naturalWidth"].toInt()>0)return I;}return -1;};
            QTRY_VERIFY_WITH_TIMEOUT(Find()>=0,30000);QVERIFY(!Do("image",Find()).contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["popup"].toObject()["kind"].toString(),QString("image"),15000);QVERIFY(!Do("close-popup").contains("error"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),Original);
    }
    void percentageCover() {
        const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader corpus not configured");
        const auto Path=QFileInfo(Corpus).dir().filePath("epub3-percentage-cover.epub");QVERIFY(QFile::exists(Path));
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Path}})["documentId"].toString();
        const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
        QTRY_VERIFY_WITH_TIMEOUT(!State()["frameBounds"].toArray().isEmpty() && State()["frameBounds"].toArray().first().toObject()["height"].toInt()>=State()["paneHeight"].toInt()*0.9,15000);
        QVERIFY(!State()["images"].toArray().isEmpty());const auto Image=State()["images"].toArray().first().toObject();QVERIFY(Image["complete"].toBool());QVERIFY(Image["naturalWidth"].toInt()>0);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void legacyPublication() {
        const auto Book=qEnvironmentVariable("HC_EPUB_COMPAT_BOOK");if(Book.isEmpty())QSKIP("Explicit legacy EPUB compatibility input required");
        QFile File(Book);QVERIFY(File.open(QIODevice::ReadOnly));const auto Original=File.readAll();File.close();
        epub::EditModel Model;QVERIFY(Model.Load(Original));
        QString Chapter;for(const auto& Value:Model.Publication()["resources"].toArray()){const auto Item=Value.toObject();if(Item["mediaType"]=="application/xhtml+xml"){Chapter=Item["path"].toString();break;}}
        QVERIFY(!Chapter.isEmpty());auto Text=Model.Read(Chapter);QVERIFY(Text.contains("</body>"));Text.replace("</body>","<p>EPUB compatibility edit</p></body>");
        QVERIFY(Model.Replace(Chapter,Text).isEmpty());const auto Output=Model.Export();QVERIFY2(Output,qPrintable(Output.error_+": "+Output.location_));
        epub::EditModel Saved;QVERIFY(Saved.Load(Output.bytes_));QCOMPARE(Saved.Read(Chapter),Text);
        QTemporaryDir Temp;QFile Edited(Temp.filePath("legacy-edited.epub"));QVERIFY(Edited.open(QIODevice::WriteOnly));QCOMPARE(Edited.write(Output.bytes_),Output.bytes_.size());Edited.close();
        qApp->setProperty("documentViewsConfigDir",Temp.path());
        for(const auto& Path:QStringList{Book,Edited.fileName()}) {
            DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
            const auto Id=Window.Handle({{"action","open"},{"path",Path}})["documentId"].toString();QVERIFY(!Id.isEmpty());
            const auto View=[&]{return document(Window,Id)["view"].toObject();};
            QTRY_VERIFY_WITH_TIMEOUT(View()["ready"].toBool() || View()["epub"].toObject()["status"]=="error",60000);
            QVERIFY2(View()["ready"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(View()).toJson())));
            QTRY_VERIFY_WITH_TIMEOUT(!View()["epub"].toObject()["images"].toArray().isEmpty(),10000);
            QTRY_VERIFY_WITH_TIMEOUT(View()["epub"].toObject()["frameBounds"].toArray().first().toObject()["height"].toInt()>0,15000);
            QVERIFY(!Window.Handle({{"action","close"},{"documentId",Id},{"discard",true}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
        QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Original);
    }
    void editSavePerformance() {
        const auto Book=qEnvironmentVariable("HC_EPUB_SAVE_BOOK"),Output=qEnvironmentVariable("HC_EPUB_SAVE_OUTPUT");
        if(Book.isEmpty() || Output.isEmpty())QSKIP("Explicit save benchmark input and output required");
        QVERIFY(QDir().mkpath(Output));QFile Input(Book);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Original=Input.readAll();Input.close();
        epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Xhtml=Model.Publication()["spine"].toArray().first().toObject()["path"].toString();const auto Text=QString::fromUtf8(Model.Read(Xhtml));
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();const auto State=[&]{return document(Window,Id);};
        QTRY_VERIFY_WITH_TIMEOUT(State()["view"].toObject()["ready"].toBool(),60000);
        QElapsedTimer Clock,Heartbeat;Clock.start();Heartbeat.start();qint64 Gap=0;QTimer Tick;Tick.setInterval(10);connect(&Tick,&QTimer::timeout,&Window,[&]{Gap=qMax(Gap,Heartbeat.restart());});Tick.start();
        QVERIFY(!Window.Handle({{"action","epub.edit.begin"},{"documentId",Id}}).contains("error"));
        auto* Editor=Window.findChild<EpubSourceEditor*>();QVERIFY(Editor);QTRY_VERIFY_WITH_TIMEOUT(Editor->IsReady(),60000);
        const auto EnterMs=Clock.elapsed(),EnterGap=Gap;
        auto* MainMenu=Window.findChild<QMenuBar*>("mainMenu");QVERIFY(MainMenu);const auto Menus=MainMenu->actions();
        QVERIFY(std::any_of(Menus.cbegin(),Menus.cend(),[](QAction* Action){return Action->text()==QString("EPUB") && Action->isVisible() && Action->menu();}));
        QJsonArray Runs;
        for(int Repeat=0;Repeat<10;++Repeat) {
            const auto Revised=Text+QString("\n<!-- save benchmark %1 -->\n").arg(Repeat);
            Clock.restart();Gap=0;Heartbeat.restart();
            QVERIFY(!Window.Handle({{"action","epub.edit.replace"},{"documentId",Id},{"resource",Xhtml},{"text",Revised}}).contains("error"));
            const auto ReplaceMs=Clock.elapsed();QVERIFY(State()["dirty"].toBool());
            Clock.restart();QVERIFY(!Window.Handle({{"action","epub.edit.validate"},{"documentId",Id}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(Editor->IsValidated(),60000);const auto ValidateMs=Clock.elapsed(),ValidateGap=Gap;
            Clock.restart();QVERIFY(!Window.Handle({{"action","epub.edit.validate"},{"documentId",Id}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(Editor->IsValidated(),60000);const auto CachedValidateMs=Clock.elapsed();
            const auto Destination=QDir(Output).filePath(QString("copy-%1.epub").arg(Repeat));QVERIFY(!QFile::exists(Destination));
            Clock.restart();Gap=0;Heartbeat.restart();
            QVERIFY(!Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",Destination}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(!State()["error"].toString().isEmpty() || (QFile::exists(Destination) && !State()["saving"].toBool() && !State()["dirty"].toBool()),60000);
            QVERIFY2(State()["error"].toString().isEmpty(),qPrintable(State()["error"].toString()));
            const auto SaveMs=Clock.elapsed();QTRY_VERIFY_WITH_TIMEOUT(Editor->IsReady(),60000);const auto SaveAndReloadMs=Clock.elapsed(),SaveGap=Gap;
            QFile Saved(Destination);QVERIFY(Saved.open(QIODevice::ReadOnly));const auto Bytes=Saved.readAll();epub::EditModel Reopened;QVERIFY(Reopened.Load(Bytes));QCOMPARE(Reopened.Read(Xhtml),Revised.toUtf8());
            Runs.append(QJsonObject{{"repeat",Repeat},{"replaceMs",ReplaceMs},{"validateMs",ValidateMs},{"cachedValidateMs",CachedValidateMs},{"validateUiMaxGapMs",ValidateGap},{"saveToCommitMs",SaveMs},{"saveAndReloadMs",SaveAndReloadMs},{"saveUiMaxGapMs",SaveGap},{"bytes",Bytes.size()}});
        }
        Tick.stop();QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        QVERIFY(Input.open(QIODevice::ReadOnly));QCOMPARE(Input.readAll(),Original);
        QFile Report(QDir(Output).filePath("timings.json"));QVERIFY(Report.open(QIODevice::WriteOnly|QIODevice::NewOnly));
        Report.write(QJsonDocument(QJsonObject{{"inputBytes",Original.size()},{"editEnterMs",EnterMs},{"enterUiMaxGapMs",EnterGap},{"runs",Runs},{"sourcePreserved",true},{"destination","local save copies"},{"pollResolutionMs",50}}).toJson());
    }
    void epubEditing_data(){QTest::addColumn<QString>("Name");QTest::newRow("epub2")<<QString("epub2-ncx.epub");QTest::newRow("epub3")<<QString("epub3-images.epub");QTest::newRow("compressed")<<QString("epub3-compressed.epub");}
    void epubEditing() {
        QFETCH(QString,Name);const auto Corpus=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Corpus.isEmpty())QSKIP("Reader-control corpus not configured");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());const auto SourcePath=Temp.filePath("input.epub");QVERIFY(QFile::copy(QFileInfo(Corpus).dir().filePath(Name),SourcePath));
        QFile Source(SourcePath);QVERIFY(Source.open(QIODevice::ReadOnly));const auto Original=Source.readAll();Source.close();epub::EditModel Model;QVERIFY(Model.Load(Original));const auto Publication=Model.Publication();
        const auto Xhtml=Publication["spine"].toArray().first().toObject()["path"].toString();const auto Opf=Publication["rootfile"].toString();QString Css,Png;
        for(const auto& V:Publication["resources"].toArray()){const auto R=V.toObject();if(R["mediaType"]=="text/css")Css=R["path"].toString();if(R["mediaType"]=="image/png")Png=R["path"].toString();}
        const auto Baseline=Model.Fingerprint();QCOMPARE(Model.Export().bytes_,Original);QVERIFY(Model.Replace(Png,"broken image").size()>0);QCOMPARE(Model.Fingerprint(),Baseline);
        QVERIFY(Model.Replace(Xhtml,"<html>").isEmpty());QVERIFY(!Model.Export());Model.Undo();QCOMPARE(Model.Fingerprint(),Baseline);Model.Redo();QVERIFY(!Model.Export());Model.Undo();
        QVERIFY(!Css.isEmpty());QVERIFY(!Png.isEmpty());auto XhtmlText=QString::fromUtf8(Model.Read(Xhtml));XhtmlText.replace("</body>","<p>편집한 문단\u00a0과 이미지 설명</p></body>");
        auto Metadata=QString::fromUtf8(Model.Read(Opf));Metadata.replace(Publication["title"].toString(),"EPUB 편집 검증본");
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",SourcePath}})["documentId"].toString();const auto View=[&]{return document(Window,Id)["view"].toObject();};const auto Editor=[&]{return View()["epubEditor"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(View()["ready"].toBool(),60000);QVERIFY(View()["readOnly"].toBool());QVERIFY(!Window.Handle({{"action","epub.edit.begin"},{"documentId",Id}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(Editor()["ready"].toBool(),30000);QVERIFY(!View()["readOnly"].toBool());QVERIFY(!document(Window,Id)["dirty"].toBool());
        const auto Replace=[&](const QString& Path,const QString& Text){return Window.Handle({{"action","epub.edit.replace"},{"documentId",Id},{"resource",Path},{"text",Text}});};
        QVERIFY(!Replace(Xhtml,XhtmlText).contains("error"));QVERIFY(document(Window,Id)["dirty"].toBool());
        auto* SourceEditor=Window.findChild<QPlainTextEdit*>("epubEditSource");QVERIFY(SourceEditor);QVERIFY(SourceEditor->find("편집한 문단"));auto Cursor=SourceEditor->textCursor();Cursor.insertText("직접 편집한 문단");XhtmlText.replace("편집한 문단","직접 편집한 문단");
        Window.SetClosePrompt([](const QString&){return int(QMessageBox::Cancel);});QVERIFY(!Window.RequestClose());QVERIFY(document(Window,Id)["dirty"].toBool());QCOMPARE(View()["epub"].toObject()["status"].toString(),QString("ready"));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
        QVERIFY(!Replace(Css,QString::fromUtf8(Model.Read(Css))+"\np { margin-bottom: 1.1em; }\n").contains("error"));QVERIFY(!Replace(Opf,Metadata).contains("error"));
        QImage Image(3,2,QImage::Format_ARGB32);Image.fill(QColor(20,100,150,170));QByteArray ImageBytes;QBuffer Buffer(&ImageBytes);Buffer.open(QIODevice::WriteOnly);QVERIFY(Image.save(&Buffer,"PNG"));
        QVERIFY(!Window.Handle({{"action","epub.edit.replace"},{"documentId",Id},{"resource",Png},{"base64",QString::fromLatin1(ImageBytes.toBase64())}}).contains("error"));
        QVERIFY(!Window.Handle({{"action","undo"},{"documentId",Id}}).contains("error"));QVERIFY(View()["canRedo"].toBool());QVERIFY(!Window.Handle({{"action","redo"},{"documentId",Id}}).contains("error"));
        QVERIFY(!Replace(Xhtml,"<html>").contains("error"));const auto Validate=[&]{return Window.Handle({{"action","epub.edit.validate"},{"documentId",Id}});};QVERIFY(!Validate().contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(!Editor()["validating"].toBool() && !Editor()["error"].toString().isEmpty(),30000);QVERIFY(!Editor()["validated"].toBool());
        const auto Bad=Temp.filePath("bad.epub");Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",Bad}});QVERIFY(!QFile::exists(Bad));QVERIFY(document(Window,Id)["dirty"].toBool());
        QVERIFY(!Replace(Xhtml,XhtmlText).contains("error"));QVERIFY(!Validate().contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Editor()["validated"].toBool(),30000);
        Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",SourcePath}});QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),Original);Source.close();
        const auto Alias=Temp.filePath("alias.epub");QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(Alias).utf16()),reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(SourcePath).utf16()),nullptr));
        Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",Alias},{"overwrite",true}});QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),Original);Source.close();
        const auto Output=Temp.filePath("output.epub");QVERIFY(!Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",Output}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Output) && !document(Window,Id)["dirty"].toBool(),30000);QTRY_VERIFY_WITH_TIMEOUT(Editor()["ready"].toBool(),30000);
        QFile Saved(Output);QVERIFY(Saved.open(QIODevice::ReadOnly));const auto SavedBytes=Saved.readAll();Saved.close();epub::EditModel Reopened;QVERIFY(Reopened.Load(SavedBytes));QCOMPARE(Reopened.Publication()["title"].toString(),QString("EPUB 편집 검증본"));QCOMPARE(Reopened.Read(Xhtml),XhtmlText.toUtf8());QCOMPARE(Reopened.Read(Png),ImageBytes);
        const auto Evidence=qEnvironmentVariable("HC_EPUB_EDIT_OUTPUT");if(!Evidence.isEmpty()){QDir().mkpath(Evidence);QFile Before(QDir(Evidence).filePath(Name+".original")),After(QDir(Evidence).filePath(Name));QVERIFY(Before.open(QIODevice::WriteOnly|QIODevice::NewOnly));Before.write(Original);QVERIFY(After.open(QIODevice::WriteOnly|QIODevice::NewOnly));After.write(SavedBytes);QVERIFY(Window.grab().save(QDir(Evidence).filePath(Name+".png")));}
        auto* ReopenAction=Window.findChild<QAction*>("epubReopen");QVERIFY(ReopenAction);ReopenAction->trigger();QTRY_VERIFY_WITH_TIMEOUT(View()["ready"].toBool() && View()["epubEditor"].toObject().isEmpty(),60000);
        QTRY_VERIFY_WITH_TIMEOUT(View()["epub"].toObject()["images"].toArray().last().toObject()["naturalWidth"].toInt()==3,15000);
        QVERIFY(!Window.Handle({{"action","epub.edit.begin"},{"documentId",Id}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(Editor()["ready"].toBool(),30000);
        QVERIFY(!document(Window,Id)["dirty"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),Original);
        DocumentWorkspaceWidget Viewer(Temp.filePath("viewer"),true);Viewer.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Viewer.show();const auto ReadId=Viewer.Handle({{"action","open"},{"path",SourcePath}})["documentId"].toString();QTRY_VERIFY_WITH_TIMEOUT(document(Viewer,ReadId)["view"].toObject()["ready"].toBool(),60000);
        QVERIFY(Viewer.Handle({{"action","epub.edit.begin"},{"documentId",ReadId}}).contains("error"));
        const auto ReadView=[&]{return document(Viewer,ReadId)["view"].toObject();};QTRY_VERIFY_WITH_TIMEOUT(ReadView()["ready"].toBool(),60000);
        auto* ViewerEdit=Viewer.findChild<QAction*>("epubEdit");QVERIFY(ViewerEdit && !ViewerEdit->isCheckable());QTRY_VERIFY_WITH_TIMEOUT(ViewerEdit->isEnabled(),10000);ViewerEdit->trigger();
        const auto Handoff=Viewer.Snapshot()["editRequest"].toObject();QCOMPARE(Handoff["format"].toString(),QString("epub"));QCOMPARE(Handoff["documentId"].toString(),ReadId);QVERIFY(Handoff["locator"].toString().startsWith("epubcfi("));
        QCOMPARE(ReadView()["epubMode"].toString(),QString("reader"));QVERIFY(ReadView()["readOnly"].toBool());QVERIFY(ReadView()["epubEditor"].toObject().isEmpty());
        QVERIFY(!Viewer.Handle({{"action","editResult"},{"token",Handoff["token"]},{"accepted",true}}).contains("error"));QVERIFY(Viewer.Snapshot()["editRequest"].toObject().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(Viewer.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void aggregateQueueAndCancellation() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"));
        Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));QVERIFY(Window.SetTabbed(true));Window.resize(1280,900);Window.show();
        Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
        QTimer Trace;Trace.setInterval(500);connect(&Trace,&QTimer::timeout,&Window,[&]{const auto Path=qEnvironmentVariable("HC_EPUB_QUEUE_EVIDENCE");if(!Path.isEmpty()){QFile File(Path);if(File.open(QIODevice::WriteOnly|QIODevice::Truncate))File.write(QJsonDocument(Window.Snapshot()).toJson());}});Trace.start();
        QStringList Ids;const auto Book=qEnvironmentVariable("HC_EPUB_POC_EPUB");
        for(int I=0;I<3;++I){const auto Copy=Temp.filePath(QString("book-%1.epub").arg(I));QVERIFY(QFile::copy(Book,Copy));Ids.append(Window.Handle({{"action","open"},{"path",Copy}})["documentId"].toString());}
        QString Queued;const auto FindQueued=[&]{for(const auto& Id:Ids)if(document(Window,Id)["view"].toObject()["epub"].toObject()["status"]=="queued"){Queued=Id;return true;}return false;};
        QTRY_VERIFY_WITH_TIMEOUT(FindQueued(),30000);QVERIFY(!Queued.isEmpty());
        QVERIFY(!Window.Handle({{"action","close"},{"documentId",Queued},{"discard",true}}).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(document(Window,Queued).isEmpty(),10000);Ids.removeAll(Queued);
        for(const auto& Id:Ids){QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);QCOMPARE(document(Window,Id)["view"].toObject()["epub"].toObject()["metrics"].toObject()["aggregateActiveLimit"].toInt(),2);}
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void largeBook() {
        const auto Book=qEnvironmentVariable("HC_EPUB_LARGE_EPUB");if(Book.isEmpty())QSKIP("Large-book corpus not configured");
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        QFile File(Book);QVERIFY(File.open(QIODevice::ReadOnly));const auto Hash=QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256);File.close();
        QElapsedTimer Elapsed;Elapsed.start();const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),90000);
        const auto State=document(Window,Id)["view"].toObject()["epub"].toObject();QVERIFY(State["text"].toString().contains("Chapter 0"));QVERIFY(!State["images"].toArray().isEmpty());
        qInfo().noquote()<<"Large-book observed startup ms:"<<Elapsed.elapsed()<<"metrics:"<<QJsonDocument(State["metrics"].toObject()).toJson(QJsonDocument::Compact);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256),Hash);
    }
    void foreignReaderEscape() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",qEnvironmentVariable("HC_EPUB_POC_EPUB")}})["documentId"].toString();
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
        HWND Reader=nullptr;EnumChildWindows(reinterpret_cast<HWND>(Window.winId()),[](HWND Child,LPARAM Data)->BOOL {wchar_t Title[128]{};GetWindowTextW(Child,Title,128);if(QString::fromWCharArray(Title)==QString("EPUB 읽기"))*reinterpret_cast<HWND*>(Data)=Child;return TRUE;},reinterpret_cast<LPARAM>(&Reader));
        QVERIFY(Reader);DWORD Pid=0;const auto Thread=GetWindowThreadProcessId(Reader,&Pid);QVERIFY(Pid!=GetCurrentProcessId());
        GUITHREADINFO Info{sizeof(Info)};QVERIFY(GetGUIThreadInfo(Thread,&Info));const auto Target=Info.hwndFocus?Info.hwndFocus:Reader;
        QVERIFY(PostMessageW(Target,WM_KEYDOWN,VK_ESCAPE,1));QVERIFY(PostMessageW(Target,WM_KEYUP,VK_ESCAPE,1));
        QTRY_VERIFY_WITH_TIMEOUT(Window.Snapshot()["closeRequested"].toBool(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readingControls() {
        QTemporaryDir Temp;QVERIFY(Temp.isValid());qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Book=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Book.isEmpty())QSKIP("Reader-control corpus not configured");
        const auto Open=[&](DocumentWorkspaceWidget& Window){Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();return Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();};
        QString SavedLocator;
        {
            DocumentWorkspaceWidget Window(Temp.filePath("recovery"));const auto Id=Open(Window);QVERIFY(!Id.isEmpty());
            const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
            const auto Do=[&](const QString& Action,QJsonValue Value=QJsonValue{}){return Window.Handle({{"action","epub.reader."+Action},{"documentId",Id},{"value",Value}});};
            QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool(),60000);
            QVERIFY(State()["text"].toString().contains("나무 그늘에서 잠시 쉬었다가"));
            QVERIFY(!Do("search","reader-control needle").contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(State()["readerAction"].toObject()["hits"].toArray().size()==2,15000);
            const auto Hit=State()["readerAction"].toObject()["hits"].toArray().last().toObject();QCOMPARE(Hit["chapter"].toInt(),1);
            QVERIFY(!Do("locate",Hit["cfi"]).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(State()["text"].toString().contains("돌아오는 길"),15000);
            QVERIFY(!Do("bookmark-add","두 번째 장").contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["bookmarks"].toArray().size(),1,10000);
            SavedLocator=State()["bookmarks"].toArray().first().toObject()["locator"].toString();QVERIFY(!SavedLocator.isEmpty());
            QVERIFY(!Do("image",0).contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["popup"].toObject()["kind"].toString(),QString("image"),10000);
            QVERIFY(!Do("close-popup").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(State()["popup"].isNull(),10000);
            QVERIFY(!Do("footnote","#hc-note").contains("error"));QTRY_COMPARE_WITH_TIMEOUT(State()["popup"].toObject()["text"].toString(),QString("각주 팝업 검증 내용"),10000);
            QVERIFY(!Do("close-popup").contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
        {
            DocumentWorkspaceWidget Window(Temp.filePath("recovery2"));const auto Id=Open(Window);const auto State=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
            QTRY_VERIFY_WITH_TIMEOUT(State()["readingStateLoaded"].toBool() && !State()["readingStateRestoring"].toBool(),60000);
            QCOMPARE(State()["bookmarks"].toArray().size(),1);QCOMPARE(State()["bookmarks"].toArray().first().toObject()["locator"].toString(),SavedLocator);
            QTRY_VERIFY_WITH_TIMEOUT(State()["text"].toString().contains("돌아오는 길"),15000);QVERIFY(State()["readingStateError"].toString().isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
    }
    void advancedProfiles_data() {
        QTest::addColumn<QString>("Background");for(const auto* Value:{"paper","grid","image","invalid"})QTest::newRow(Value)<<QString(Value);
    }
    void advancedProfiles() {
        QFETCH(QString,Background);QTemporaryDir Temp;QVERIFY(Temp.isValid());qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Book=qEnvironmentVariable("HC_EPUB_POC_EPUB");QFile Source(Book);QVERIFY(Source.open(QIODevice::ReadOnly));const auto Original=Source.readAll();Source.close();
        const auto Hash=QCryptographicHash::hash(Original,QCryptographicHash::Sha256).toHex();
        QJsonObject Profile{{"background",Background=="invalid"?"paper":Background},{"backgroundColor","#f2e7d5"},{"textColor","#344455"},{"cssEnabled",true},
            {"customCss",Background=="invalid"?QString("@import 'https://invalid';"):QString("p { color: #52331e; text-indent: 1.25em; margin-block-start: .5em; }")}};
        if(Background=="image") {
            QImage Image(32,24,QImage::Format_ARGB32);Image.fill(QColor(20,90,130,180));QByteArray Data;QBuffer Buffer(&Data);Buffer.open(QIODevice::WriteOnly);QVERIFY(Image.save(&Buffer,"PNG"));
            const auto Id=QString::fromLatin1(QCryptographicHash::hash(Data,QCryptographicHash::Sha256).toHex());QVERIFY(QDir().mkpath(Temp.filePath("epub-assets")));
            QFile Asset(Temp.filePath("epub-assets/bg-"+Id+".png"));QVERIFY(Asset.open(QIODevice::WriteOnly));QCOMPARE(Asset.write(Data),Data.size());Asset.close();Profile["backgroundAsset"]=Id;
        }
        QFile Preferences(Temp.filePath("epub-reading.json"));QVERIFY(Preferences.open(QIODevice::WriteOnly));
        Preferences.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"global",Profile},{"books",QJsonObject{}}}).toJson());Preferences.close();
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        const auto Epub=[&]{return document(Window,Id)["view"].toObject()["epub"].toObject();};
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
        if(Background=="invalid") {QVERIFY(!Epub()["cssValid"].toBool());QVERIFY(!Epub()["cssValidationError"].toString().isEmpty());}
        else {
            QTRY_COMPARE_WITH_TIMEOUT(Epub()["paragraphs"].toArray().first().toObject()["color"].toString(),QString("rgb(82, 51, 30)"),15000);
            QCOMPARE(Epub()["paragraphs"].toArray().first().toObject()["indent"].toString(),QString("23.75px"));
            QVERIFY(Epub()["backgroundImageApplied"].toBool());QCOMPARE(Epub()["canvasColor"].toString(),QString("rgb(242, 231, 213)"));
            QVERIFY(Epub()["cssValid"].toBool());
        }
        const auto Execute=[&](const QString& Action,QJsonObject Patch=QJsonObject{}){return Window.Handle({{"action","epub.settings."+Action},{"documentId",Id},{"profile",Patch}});};
        QVERIFY(!Execute("preview",{{"cssEnabled",false},{"background","theme"}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(Epub()["settingsDirty"].toBool(),10000);
        QTRY_COMPARE_WITH_TIMEOUT(Epub()["profile"].toObject()["cssEnabled"].toBool(),false,10000);
        QVERIFY(!Execute("cancel").contains("error"));QTRY_VERIFY_WITH_TIMEOUT(!Epub()["settingsDirty"].toBool(),10000);
        QVERIFY(!Execute("preview",{{"cssEnabled",false},{"size",25}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(Epub()["settingsDirty"].toBool() && Epub()["cssValid"].toBool(),10000);
        QVERIFY(!Execute("apply").contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(!Epub()["settingsBusy"].toBool() && !Epub()["settingsDirty"].toBool(),10000);
        QVERIFY(Preferences.open(QIODevice::ReadOnly));const auto Saved=QJsonDocument::fromJson(Preferences.readAll()).object();Preferences.close();
        QCOMPARE(Saved["books"].toObject()[QString::fromLatin1(Hash)].toObject()["size"].toInt(),25);
        QVERIFY(!document(Window,Id)["dirty"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),Original);
    }
    void sharedWorkspace_data() {
        QTest::addColumn<bool>("Hosted");QTest::addColumn<QString>("Mode");
        for(bool Hosted:{false,true})for(const auto* Mode:{"ready","cancel","invalid"})
            QTest::newRow(qPrintable(QString(Hosted?"viewer-":"editor-")+Mode))<<Hosted<<QString(Mode);
    }
    void sharedWorkspace() {
        QFETCH(bool,Hosted);QFETCH(QString,Mode);
        QTemporaryDir Temp;QVERIFY(Temp.isValid());
        qApp->setProperty("documentViewsConfigDir", Temp.path());
        QFile Text(Temp.filePath("keep.txt"));QVERIFY(Text.open(QIODevice::WriteOnly));Text.write("original");Text.close();
        const auto PdfPath=Temp.filePath("keep.pdf");
        { QPdfWriter Pdf(PdfPath);QPainter Painter(&Pdf);Painter.drawText(100,100,"Keep PDF tab"); }
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),Hosted);
        Window.ConfigureEpub(testEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));
        const auto Hwp=qEnvironmentVariable("HC_EPUB_HWP");
        if(!Hwp.isEmpty()) Window.ConfigureHwp(MakeProductHwpRuntime(qEnvironmentVariable("HC_EPUB_HWP_APPLICATION",qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")), qgetenv("HC_EPUB_HWP_PIN")));
        Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
        QVERIFY(Window.SetTabbed(true));Window.resize(1280,900);Window.show();
        const auto TextId=Window.Handle({{"action","open"},{"path",Text.fileName()}})["documentId"].toString();QVERIFY(!TextId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,TextId)["lifecycle"]=="ready",30000);
        QVERIFY(!Window.Handle({{"action","replaceText"},{"documentId",TextId},{"text","keep unsaved text"}}).contains("error"));
        QVERIFY(document(Window,TextId)["dirty"].toBool());
        const auto PdfId=Window.Handle({{"action","open"},{"path",PdfPath}})["documentId"].toString();QVERIFY(!PdfId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,PdfId)["lifecycle"]=="ready",30000);
        QCOMPARE(document(Window,PdfId)["view"].toObject()["viewId"],QJsonValue("pdf"));
        QString HwpId;
        if(Mode=="ready" && !Hwp.isEmpty()) {
            HwpId=Window.Handle({{"action","open"},{"path",Hwp}})["documentId"].toString(); QVERIFY(!HwpId.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(document(Window,HwpId)["view"].toObject()["ready"].toBool(),60000);
        }
        QString Book=qEnvironmentVariable("HC_EPUB_POC_EPUB");
        QFile Source(Book); QVERIFY(Source.open(QIODevice::ReadOnly)); const auto Hash=QCryptographicHash::hash(Source.readAll(),QCryptographicHash::Sha256).toHex();
        QFile Preferences(Temp.filePath("epub-reading.json")); QVERIFY(Preferences.open(QIODevice::WriteOnly));
        Preferences.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"global",QJsonObject{{"line",2.2},{"after",1.1}}},
            {"books",QJsonObject{{QString::fromLatin1(Hash),QJsonObject{{"before",0.5}}}}}}).toJson()); Preferences.close();
        if(Mode=="invalid") {Book=Temp.filePath("invalid.epub");QFile Bad(Book);QVERIFY(Bad.open(QIODevice::WriteOnly));Bad.write("invalid epub");}
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        if(Mode=="cancel")QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["viewId"]=="epub",30000);
        if(Mode!="cancel") {
            if(Mode=="invalid")QTRY_VERIFY_WITH_TIMEOUT(!document(Window,Id)["error"].toString().isEmpty(),30000);
            else {
                QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
                QVERIFY(document(Window,Id)["view"].toObject()["readOnly"].toBool());
                QVERIFY(!document(Window,Id)["view"].toObject()["epub"].toObject()["images"].toArray().isEmpty());
                QVERIFY(document(Window,Id)["view"].toObject()["epub"].toObject()["settingsPersistent"].toBool());
                QTRY_COMPARE_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["epub"].toObject()["readingProfile"].toObject()["line"].toDouble(),2.2,10000);
                QCOMPARE(document(Window,Id)["view"].toObject()["epub"].toObject()["readingProfile"].toObject()["before"].toDouble(),0.5);
                auto& Theme = fm::style::ThemeManager::instance();
                Theme.setScheme(fm::style::ThemeManager::Scheme::Dark);
                Theme.setDarkTone(fm::style::ThemeManager::DarkTone::Navy);
                Theme.setOverride(fm::style::Variant::Navy, fm::style::Token::Accent, QColor("#b95c87"));
                const auto Expected = epubui::Appearance();
                QTRY_COMPARE_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["epub"].toObject()["appearance"].toObject(), Expected, 15000);
                const auto TextColor=Theme.colors().toPalette().color(QPalette::Text);
                const auto CssColor=QString("rgb(%1, %2, %3)").arg(TextColor.red()).arg(TextColor.green()).arg(TextColor.blue());
                QTRY_COMPARE_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["epub"].toObject()["paragraphs"].toArray().first().toObject()["color"].toString(),CssColor,15000);
                QVERIFY(!document(Window,Id)["canSave"].toBool());QVERIFY(!document(Window,Id)["canSaveAs"].toBool());
                QVERIFY(Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",Temp.filePath("out.epub")}}).contains("error"));
                QVERIFY(!QFile::exists(Temp.filePath("out.epub")));
                const auto Generation=document(Window,Id)["generation"].toString();
                QVERIFY(!Window.Handle({{"action","reload"},{"documentId",Id},{"discard",true}}).contains("error"));
                QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["generation"].toString()!=Generation,30000);
                QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
            }
        }
        QVERIFY(!Window.Handle({{"action","close"},{"documentId",Id},{"discard",true}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id).isEmpty(),30000);
        QVERIFY(document(Window,TextId)["dirty"].toBool());
        QCOMPARE(Window.Snapshot()["documents"].toArray().size(),HwpId.isEmpty()?2:3);
        if(!HwpId.isEmpty()) QVERIFY(document(Window,HwpId)["view"].toObject()["ready"].toBool());
        QCOMPARE(document(Window,PdfId)["view"].toObject()["viewId"],QJsonValue("pdf"));
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);
    }
};
QTEST_MAIN(EpubWorkspaceTests)
#include "testWorkspace.moc"
