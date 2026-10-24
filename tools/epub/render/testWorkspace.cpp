#include "stdafx.h"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/epubProcessView.hpp"
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
#include <QJsonDocument>
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "commonLibs/document/epub/epubEditModel.hpp"

using namespace hc::document;
namespace {
QJsonObject document(DocumentWorkspaceWidget& Window, const QString& Id) {
    for(const auto& Value:Window.Snapshot()["documents"].toArray())
        if(Value.toObject()["documentId"]==Id)return Value.toObject();
    return {};
}
}
class EpubWorkspaceTests final : public QObject {
    Q_OBJECT
private slots:
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
            DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
            const auto Id=Window.Handle({{"action","open"},{"path",Path}})["documentId"].toString();QVERIFY(!Id.isEmpty());
            const auto View=[&]{return document(Window,Id)["view"].toObject();};
            QTRY_VERIFY_WITH_TIMEOUT(View()["ready"].toBool() || View()["epub"].toObject()["status"]=="error",60000);
            QVERIFY2(View()["ready"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(View()).toJson())));
            QVERIFY(!View()["epub"].toObject()["images"].toArray().isEmpty());
            QVERIFY(!Window.Handle({{"action","close"},{"documentId",Id},{"discard",true}}).contains("error"));
            QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
        QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Original);
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
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
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
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),Original);
        DocumentWorkspaceWidget Viewer(Temp.filePath("viewer"),true);Viewer.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Viewer.show();const auto ReadId=Viewer.Handle({{"action","open"},{"path",SourcePath}})["documentId"].toString();QTRY_VERIFY_WITH_TIMEOUT(document(Viewer,ReadId)["view"].toObject()["ready"].toBool(),60000);
        QVERIFY(Viewer.Handle({{"action","epub.edit.begin"},{"documentId",ReadId}}).contains("error"));QTRY_VERIFY_WITH_TIMEOUT(Viewer.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void aggregateQueueAndCancellation() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"));
        Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));QVERIFY(Window.SetTabbed(true));Window.resize(1280,900);Window.show();
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
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        QFile File(Book);QVERIFY(File.open(QIODevice::ReadOnly));const auto Hash=QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256);File.close();
        QElapsedTimer Elapsed;Elapsed.start();const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),90000);
        const auto State=document(Window,Id)["view"].toObject()["epub"].toObject();QVERIFY(State["text"].toString().contains("Chapter 0"));QVERIFY(!State["images"].toArray().isEmpty());
        qInfo().noquote()<<"Large-book observed startup ms:"<<Elapsed.elapsed()<<"metrics:"<<QJsonDocument(State["metrics"].toObject()).toJson(QJsonDocument::Compact);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256),Hash);
    }
    void foreignReaderEscape() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
        const auto Id=Window.Handle({{"action","open"},{"path",qEnvironmentVariable("HC_EPUB_POC_EPUB")}})["documentId"].toString();
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
        HWND Reader=nullptr;EnumChildWindows(reinterpret_cast<HWND>(Window.winId()),[](HWND Child,LPARAM Data)->BOOL {wchar_t Title[128]{};GetWindowTextW(Child,Title,128);if(QString::fromWCharArray(Title)==QString("EPUB 읽기 설정"))*reinterpret_cast<HWND*>(Data)=Child;return TRUE;},reinterpret_cast<LPARAM>(&Reader));
        QVERIFY(Reader);DWORD Pid=0;const auto Thread=GetWindowThreadProcessId(Reader,&Pid);QVERIFY(Pid!=GetCurrentProcessId());
        GUITHREADINFO Info{sizeof(Info)};QVERIFY(GetGUIThreadInfo(Thread,&Info));const auto Target=Info.hwndFocus?Info.hwndFocus:Reader;
        QVERIFY(PostMessageW(Target,WM_KEYDOWN,VK_ESCAPE,1));QVERIFY(PostMessageW(Target,WM_KEYUP,VK_ESCAPE,1));
        QTRY_VERIFY_WITH_TIMEOUT(Window.Snapshot()["closeRequested"].toBool(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
    }
    void readingControls() {
        QTemporaryDir Temp;QVERIFY(Temp.isValid());qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Book=qEnvironmentVariable("HC_EPUB_READER_EPUB");if(Book.isEmpty())QSKIP("Reader-control corpus not configured");
        const auto Open=[&](DocumentWorkspaceWidget& Window){Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();return Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();};
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
            QVERIFY(State()["text"].toString().contains("돌아오는 길"));QVERIFY(State()["readingStateError"].toString().isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        }
    }
    void advancedProfiles_data() {
        QTest::addColumn<QString>("Background");for(const auto* Value:{"paper","grid","image","invalid"})QTest::newRow(Value)<<QString(Value);
    }
    void advancedProfiles() {
        QFETCH(QString,Background);QTemporaryDir Temp;QVERIFY(Temp.isValid());qApp->setProperty("documentViewsConfigDir",Temp.path());
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));Window.resize(1280,900);Window.show();
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
        Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));
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
