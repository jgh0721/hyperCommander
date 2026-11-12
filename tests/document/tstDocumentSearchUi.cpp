#include "stdafx.h"
#include "UniqueLibs/document/search/documentSearchPanel.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "documentTabsProbe.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include "UniqueLibs/document/epubSourceEditor.hpp"
#include "core/AppSettings.h"
#include "../nativeTestInput.hpp"
#include <QtCore/private/qzipwriter_p.h>
#include <QTabBar>
#include <atomic>
#include <thread>
#include <QComboBox>
#include <QtTest>
#include <QApplication>
#include <QLineEdit>
#include <QTableView>
#include <QPushButton>
#include <QDockWidget>
#include <QLabel>
#include <QTabWidget>
#include <QScreen>
#include <iostream>
#include <Windows.h>
#include <Psapi.h>

using namespace hc::document;
class DocumentSearchUiTest final : public QObject {
    Q_OBJECT
private slots:
    void diskEncodingRoundTrip() {
        const auto Root=qEnvironmentVariable("HC_DOCUMENT_SEARCH_IO_ROOT");
        QTemporaryDir Directory(Root.isEmpty()?QDir::tempPath()+"/hc-search-io-XXXXXX":Root+"/hc-search-io-XXXXXX"),Settings;
        QVERIFY(Directory.isValid());qApp->setProperty("documentViewsConfigDir",Settings.path());
        QFile Owner(Directory.filePath(".owner.json"));QVERIFY(Owner.open(QIODevice::WriteOnly));Owner.write("{\"task\":\"document-search-io-validation\",\"expires\":\"2026-11-10\"}");Owner.close();
        QMap<QString,QByteArray> Originals;
        const auto Before=QString::fromUtf8("hello 한글\r\nhello\nhello\r");const auto After=QString::fromUtf8("world 한글\r\nworld\nworld\r");
        for(const auto& Encoding:QStringList{"UTF-8","UTF-16LE","UTF-16BE","CP949"}){
            const auto Path=Directory.filePath(Encoding+".txt");const auto Bytes=EncodeDocumentText(Before,Encoding,Encoding.startsWith("UTF"));QVERIFY(Bytes.HasValue());
            QFile File(Path);QVERIFY(File.open(QIODevice::WriteOnly));QCOMPARE(File.write(Bytes.Value()),Bytes.Value().size());Originals[Path]=Bytes.Value();
        }
        {
            search::Panel Panel;Panel.show();Panel.Open(true,Directory.path(),false,{});Panel.findChild<QLineEdit*>("documentSearchInclude")->setText("*.txt");Panel.SetQuery("hello");Panel.findChild<QLineEdit*>("documentSearchReplacement")->setText("world");
            auto* All=Panel.findChild<QPushButton*>("documentSearchReplace_all");QTRY_VERIFY_WITH_TIMEOUT(All->isEnabled(),30000);QCOMPARE(Panel.findChild<QTableView*>("documentSearchResults")->model()->rowCount(),12);
            All->click();QTRY_VERIFY_WITH_TIMEOUT(!Panel.Busy(),30000);
            for(auto It=Originals.cbegin();It!=Originals.cend();++It){QFile File(It.key());QVERIFY(File.open(QIODevice::ReadOnly));const auto Encoding=QFileInfo(It.key()).baseName();const auto Expected=EncodeDocumentText(After,Encoding,Encoding.startsWith("UTF"));QVERIFY2(File.readAll()==Expected.Value(),qPrintable(Panel.findChild<QLabel*>("documentSearchStatus")->text()));}
            Panel.findChild<QPushButton*>("documentSearchReplace_undo")->click();QTRY_VERIFY_WITH_TIMEOUT(!Panel.Busy(),30000);
            for(auto It=Originals.cbegin();It!=Originals.cend();++It){QFile File(It.key());QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),It.value());}
            qInfo().noquote()<<"IO_ROUNDTRIP root="<<Directory.path()<<" encodings=UTF-8,UTF-16LE,UTF-16BE,CP949 mixed_eol=true exact_bytes_restored=true";
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(),10000);
    }
    void epubBodyBatchAndDraft() {
        QTemporaryDir Directory;QVERIFY(Directory.isValid());
        qApp->setProperty("documentViewsConfigDir",Directory.path());qApp->setProperty("epubWorkspaceDirectory",Directory.path());
        const auto Path=Directory.filePath("book.epub");
        const QByteArray Chapter="<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter</title></head><body><p>hel<b>lo</b> hello</p></body></html>";
        {QZipWriter Zip(Path);Zip.setCompressionPolicy(QZipWriter::NeverCompress);Zip.addFile("mimetype","application/epub+zip");
            Zip.addFile("META-INF/container.xml","<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles><rootfile full-path=\"EPUB/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
            Zip.addFile("EPUB/content.opf","<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">search-test</dc:identifier><dc:title>Search</dc:title><dc:language>en</dc:language><meta property=\"dcterms:modified\">2026-11-03T00:00:00Z</meta></metadata><manifest><item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"two\" href=\"two.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/></manifest><spine><itemref idref=\"one\"/><itemref idref=\"two\"/></spine></package>");
            Zip.addFile("EPUB/nav.xhtml","<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents</title></head><body><nav epub:type=\"toc\"><ol><li><a href=\"one.xhtml\">First</a></li><li><a href=\"two.xhtml\">Second</a></li></ol></nav></body></html>");
            Zip.addFile("EPUB/one.xhtml",Chapter);Zip.addFile("EPUB/two.xhtml",Chapter);Zip.close();QCOMPARE(Zip.status(),QZipWriter::NoError);}
        QFile Original(Path);QVERIFY(Original.open(QIODevice::ReadOnly));const auto Bytes=Original.readAll();Original.close();
        {
            EpubSourceEditor Editor(Bytes,Path);Editor.SetRecoveryEnabled(false);Editor.resize(850,650);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);
            search::Panel Panel;Panel.resize(1000,600);Panel.show();
            Panel.SetBookSources([&]{return Editor.CaptureSearchSources("book");});
            connect(&Panel,&search::Panel::ApplyBuffer,&Editor,[&](quint64 Ticket,const QJsonObject& Source,const QString&,const QString&){Panel.FinishBuffer(Ticket,Editor.ApplySearchBatch(Source["bookBatch"].toArray()));});
            Panel.Open(true,Path,true,{});Panel.findChild<QComboBox*>("documentSearchTarget")->setCurrentIndex(1);
            Panel.SetQuery("hello");Panel.findChild<QLineEdit*>("documentSearchReplacement")->setText("a&b");
            auto* All=Panel.findChild<QPushButton*>("documentSearchReplace_all");auto* Table=Panel.findChild<QTableView*>("documentSearchResults");
            QTRY_VERIFY_WITH_TIMEOUT(All->isEnabled(),15000);QCOMPARE(Table->model()->rowCount(),4);
            const auto Before=Editor.CaptureSearchSources("book")();All->click();QTRY_VERIFY_WITH_TIMEOUT(!Panel.Busy(),10000);
            int Changed=0;for(const auto& Value:Editor.CaptureSearchSources("book")()){const auto Source=Value.toObject();if(Source["resource"].toString().endsWith("one.xhtml")||Source["resource"].toString().endsWith("two.xhtml")){QVERIFY(Source["text"].toString().contains("a&amp;b<b></b> a&amp;b"));++Changed;}}QCOMPARE(Changed,2);
            Panel.findChild<QPushButton*>("documentSearchReplace_undo")->click();QTRY_VERIFY_WITH_TIMEOUT(!Panel.Busy(),10000);QCOMPARE(Editor.CaptureSearchSources("book")(),Before);
            Editor.SelectSearchRange("EPUB/one.xhtml",QString::fromUtf8(Chapter).indexOf("hello"),5);
            QVERIFY(Editor.Execute({{"action","epub.edit.insert"},{"text","draft-token"}})["accepted"].toBool());
            Panel.SourcesChanged();Panel.SetQuery("draft-token");QTRY_VERIFY_WITH_TIMEOUT(All->isEnabled(),15000);QCOMPARE(Table->model()->rowCount(),1);
            QVERIFY(Original.open(QIODevice::ReadOnly));QCOMPARE(Original.readAll(),Bytes);
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(),10000);
    }
    void millionResultsStress() {
        if(!qEnvironmentVariableIsSet("HC_DOCUMENT_SEARCH_STRESS"))QSKIP("Explicit owned-fixture stress run required.");
        QTemporaryDir Directory;QVERIFY(Directory.isValid());qApp->setProperty("documentViewsConfigDir",Directory.filePath("settings"));
        const QString Original=QString("needle\r\n").repeated(10000),Changed=QString("swap\r\n").repeated(10000);
        QStringList Paths;
        for(int Index=0;Index<100;++Index){const auto Path=Directory.filePath(QString("%1.txt").arg(Index,3,10,QChar('0')));QFile File(Path);QVERIFY(File.open(QIODevice::WriteOnly));
            const auto Bytes=EncodeDocumentText(Original,Index%2?"UTF-8":"UTF-16LE",Index%2==0);QVERIFY(Bytes.HasValue());QCOMPARE(File.write(Bytes.Value()),Bytes.Value().size());Paths.append(Path);}
        {
            search::Panel Panel;Panel.resize(1280,800);Panel.show();Panel.Open(true,Directory.path(),false,{});
            Panel.findChild<QLineEdit*>("documentSearchInclude")->setText("*.txt");Panel.SetQuery("needle");Panel.findChild<QLineEdit*>("documentSearchReplacement")->setText("swap");
            auto* Table=Panel.findChild<QTableView*>("documentSearchResults");auto* All=Panel.findChild<QPushButton*>("documentSearchReplace_all");
            QElapsedTimer Clock;Clock.start();QTRY_VERIFY_WITH_TIMEOUT(Table->model()->rowCount()>0,15000);QVERIFY(Table->model()->rowCount()<1000000);
            QEventLoop Loop;QTimer Poll,Deadline;Poll.setInterval(25);Deadline.setSingleShot(true);connect(&Poll,&QTimer::timeout,&Loop,[&]{if(All->isEnabled())Loop.quit();});connect(&Deadline,&QTimer::timeout,&Loop,&QEventLoop::quit);Poll.start();Deadline.start(300000);Loop.exec();QVERIFY(All->isEnabled());
            QCOMPARE(Table->model()->rowCount(),1000000);const auto SearchMs=Clock.elapsed();
            Table->scrollTo(Table->model()->index(999999,2));QTRY_VERIFY_WITH_TIMEOUT(Table->model()->index(999999,2).data(Qt::UserRole).toJsonObject().contains("replacement"),10000);
            Clock.restart();Table->selectAll();Panel.findChild<QPushButton*>("documentSearchReplace_selected")->click();const auto SelectMs=Clock.elapsed();
            QTRY_VERIFY_WITH_TIMEOUT(!Panel.Busy(),120000);
            const auto Verify=[&](const QString& Expected){for(int Index=0;Index<Paths.size();++Index){QFile File(Paths[Index]);if(!File.open(QIODevice::ReadOnly))return false;const auto Encoded=EncodeDocumentText(Expected,Index%2?"UTF-8":"UTF-16LE",Index%2==0);if(!Encoded.HasValue()||File.readAll()!=Encoded.Value())return false;}return true;};
            QVERIFY2(Verify(Changed),qPrintable(Panel.findChild<QLabel*>("documentSearchStatus")->text()));const auto ApplyMs=Clock.elapsed();Clock.restart();Panel.findChild<QPushButton*>("documentSearchReplace_undo")->click();QTRY_VERIFY_WITH_TIMEOUT(!Panel.Busy(),120000);QVERIFY(Verify(Original));
            qInfo().noquote()<<QString("MILLION_RESULTS rows=1000000 files=100 search_ms=%1 selection_dispatch_ms=%2 apply_ms=%3 undo_ms=%4 exact_bytes_restored=true").arg(SearchMs).arg(SelectMs).arg(ApplyMs).arg(Clock.elapsed());QVERIFY(SelectMs<1000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(),10000);
    }
    void qtTreeIncrementalPreview() {
        const auto Root=qEnvironmentVariable("HC_DOCUMENT_SEARCH_BENCH_ROOT");if(Root.isEmpty())QSKIP("Set HC_DOCUMENT_SEARCH_BENCH_ROOT to a read-only Qt tree.");
        QVERIFY(QFileInfo(Root).isDir());QTemporaryDir Directory;qApp->setProperty("documentViewsConfigDir",Directory.path());
        {
            search::Panel Panel;Panel.resize(1280,800);Panel.SetReadOnly(true);Panel.show();Panel.Open(true,Root,false,{});
            auto* Query=Panel.findChild<QLineEdit*>("documentSearchQuery");auto* Replacement=Panel.findChild<QLineEdit*>("documentSearchReplacement");
            auto* Include=Panel.findChild<QLineEdit*>("documentSearchInclude");auto* Table=Panel.findChild<QTableView*>("documentSearchResults");auto* All=Panel.findChild<QPushButton*>("documentSearchReplace_all");
            Include->setText("*.h;*.hpp;*.cpp;*.cxx;*.qml");QTest::keyClicks(Query,"Q_OBJECT");Replacement->setText("Q_OBJECT_PREVIEW");
            QElapsedTimer Clock,Beat;Clock.start();Beat.start();QVector<qint64> Delays;QTimer Heartbeat;
            QFile Evidence(qEnvironmentVariable("HC_DOCUMENT_SEARCH_BENCH_LOG"));
            if(!Evidence.fileName().isEmpty())QVERIFY(Evidence.open(QIODevice::WriteOnly|QIODevice::Truncate));
            QTimer Sampler;Sampler.setInterval(10000);
            const auto Sample=[&]{
                PROCESS_MEMORY_COUNTERS_EX Memory{};Memory.cb=sizeof(Memory);K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Memory),sizeof(Memory));
                const QJsonObject Record{{"task","document-search-scale-validation"},{"root",Root},{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"expires","2026-11-09"},{"elapsedMs",Clock.elapsed()},{"rows",Table->model()->rowCount()},{"privateBytes",qint64(Memory.PrivateUsage)},{"peakWorkingSetBytes",qint64(Memory.PeakWorkingSetSize)},{"status",Panel.findChild<QLabel*>("documentSearchStatus")->text()}};
                if(Evidence.isOpen()){Evidence.write(QJsonDocument(Record).toJson(QJsonDocument::Compact)+'\n');Evidence.flush();}
            };connect(&Sampler,&QTimer::timeout,&Panel,Sample);Sampler.start();Sample();
            Heartbeat.setTimerType(Qt::PreciseTimer);Heartbeat.setInterval(10);connect(&Heartbeat,&QTimer::timeout,&Panel,[&]{Delays.append(Beat.restart());});Heartbeat.start();
            QTRY_VERIFY_WITH_TIMEOUT(Table->model()->rowCount()>0,30000);const auto FirstMs=Clock.elapsed();QVERIFY(!All->isEnabled());
            QTimer Interaction;Interaction.setInterval(750);int Changes=0;
            connect(&Interaction,&QTimer::timeout,&Panel,[&]{
                Replacement->setFocus();Replacement->selectAll();QTest::keyClicks(Replacement,++Changes%2?"Q_OBJECT_PREVIEW":"Q_OBJECT_REPLACED",Qt::NoModifier,0);
                const int Count=Table->model()->rowCount();if(Count)Table->scrollTo(Table->model()->index((Changes%2)?Count-1:0,2));
            });Interaction.start();
            // qWait's sleeping test driver distorts timer latency. Run the same
            // native event loop as the product while collecting responsiveness.
            QEventLoop Loop;QTimer Completion,Deadline;Completion.setInterval(25);Deadline.setSingleShot(true);
            connect(&Completion,&QTimer::timeout,&Loop,[&]{if(All->isEnabled())Loop.quit();});
            connect(&Deadline,&QTimer::timeout,&Loop,&QEventLoop::quit);Completion.start();Deadline.start(6000000);Loop.exec();
            QVERIFY(All->isEnabled());Interaction.stop();Heartbeat.stop();Sampler.stop();Sample();
            QVERIFY(Changes>0);std::sort(Delays.begin(),Delays.end());QVERIFY(!Delays.isEmpty());
            const auto P95=Delays[(Delays.size()-1)*95/100];
            qInfo().noquote()<<QString("QT_TREE root=%1 rows=%2 first_ms=%3 total_ms=%4 heartbeat_p95_ms=%5 max_ms=%6 preview_edits=%7 status=%8").arg(Root).arg(Table->model()->rowCount()).arg(FirstMs).arg(Clock.elapsed()).arg(P95).arg(Delays.last()).arg(Changes).arg(Panel.findChild<QLabel*>("documentSearchStatus")->text());
            QVERIFY2(P95<=100,"UI heartbeat p95 exceeded 100 ms");QVERIFY2(Delays.last()<=1000,"UI stalled for more than one second");
            QFile Errors(Panel.property("diagnosticLog").toString());if(Errors.open(QIODevice::ReadOnly)){const auto Contents=Errors.read(4000);qInfo().noquote()<<"SEARCH_ERRORS"<<QString::fromUtf8(Contents.left(4000));}
            Table->scrollToTop();QTRY_VERIFY_WITH_TIMEOUT(Table->model()->index(0,2).data(Qt::UserRole).toJsonObject().contains("replacement"),10000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(),10000);
    }
    void realWorkspaceSearchReplaceDockUndo_data() {
        QTest::addColumn<QString>("Suffix");QTest::newRow("text")<<QString(".txt");QTest::newRow("markdown")<<QString(".md");
    }
    void realWorkspaceSearchReplaceDockUndo() {
        QFETCH(QString,Suffix);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        qApp->setProperty("documentViewsConfigDir", Directory.filePath("settings"));
        const auto A = Directory.filePath("a"+Suffix), B = Directory.filePath("b.txt");
        const auto Put = [](const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put(A, "hello hello\r\n")); QVERIFY(Put(B, "hello\r\n"));
        {
            DocumentWorkspaceWidget Window(Directory.filePath("recovery")); Window.resize(1280, 960); Window.show();
            Window.SetClosePrompt([](const QString&) { return 1; });
            const auto Id = Window.Handle({{"action", "open"}, {"path", A}})["documentId"].toString(); QVERIFY(!Id.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(Window.IsIdle(), 15000);
            auto* Pane = Window.findChild<DocumentViewPane*>(); QVERIFY(Pane); QCOMPARE(Pane->SearchText(), QString("hello hello\r\n"));
            QTest::keyClick(&Window, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
            auto* Panel = Window.findChild<search::Panel*>(); QVERIFY(Panel);
            auto* Query = Panel->findChild<QLineEdit*>("documentSearchQuery"); auto* Replacement = Panel->findChild<QLineEdit*>("documentSearchReplacement"); QVERIFY(Query); QVERIFY(Replacement);
            QTRY_VERIFY(Query->hasFocus()); QTest::keyClicks(Query, "hello"); QTest::keyClick(Query, Qt::Key_Tab); QVERIFY(Replacement->hasFocus()); QTest::keyClicks(Replacement, "world");
            QTest::keyClick(Replacement,Qt::Key_N,Qt::AltModifier);QTRY_VERIFY(Query->hasFocus());
            QTest::keyClick(Query,Qt::Key_W,Qt::AltModifier);QTRY_VERIFY(Replacement->hasFocus());
            QTest::keyClick(Replacement,Qt::Key_B,Qt::AltModifier);QTRY_VERIFY(Panel->findChild<QLineEdit*>("documentSearchExclude")->hasFocus());
            QSignalSpy FindClicked(Panel->findChild<QPushButton*>("documentSearchFind"),&QPushButton::clicked); QTest::keyClick(Panel->findChild<QLineEdit*>("documentSearchExclude"),Qt::Key_T,Qt::AltModifier); QTRY_COMPARE(FindClicked.size(),1);
            auto* Table = Panel->findChild<QTableView*>("documentSearchResults"); QVERIFY(Table);
            QTRY_COMPARE_WITH_TIMEOUT(Table->model()->rowCount(), 3, 15000);
            QTRY_VERIFY_WITH_TIMEOUT(!Table->model()->index(0, 2).data(Qt::UserRole).toJsonObject().isEmpty(), 10000);
            QCOMPARE(Table->model()->index(0, 2).data(Qt::UserRole).toJsonObject()["replacement"].toString(), QString("world"));
            Table->selectRow(1);QTest::keyClick(&Window,Qt::Key_F,Qt::ControlModifier|Qt::ShiftModifier);
            QTRY_VERIFY(Query->hasFocus());QVERIFY(!Replacement->isVisible());QCOMPARE(Table->model()->rowCount(),3);QVERIFY(Table->selectionModel()->isRowSelected(1,{}));
            QTRY_VERIFY_WITH_TIMEOUT(!Table->model()->index(0,2).data(Qt::UserRole).toJsonObject().isEmpty(),10000);
            QVERIFY(!Table->model()->index(0,2).data(Qt::UserRole).toJsonObject().contains("replacement"));
            QTest::keyClick(&Window,Qt::Key_H,Qt::ControlModifier|Qt::ShiftModifier);QTRY_VERIFY(Query->hasFocus());QVERIFY(Replacement->isVisible());QCOMPARE(Replacement->text(),QString("world"));
            QTRY_VERIFY_WITH_TIMEOUT(Table->model()->index(0,2).data(Qt::UserRole).toJsonObject().contains("replacement"),10000);
            // 파일에서 찾기는 도구 창(fmdock 도크 "tool:search"): 위치 = 분리 창(떠 있는 창) · 콘텐츠 탭(중앙) · 하단 패널.
            test::DocumentTabs Tabs(&Window); QVERIFY(Tabs);
            auto* Dock = Tabs.tool("search"); QVERIFY(Dock); QVERIFY(!Dock->isFloating()); QVERIFY(Dock->isAncestorOf(Panel));
            Panel->PlacementRequested("floating"); QTRY_VERIFY(Dock->isFloating()); QCOMPARE(Table->model()->rowCount(), 3);
            Panel->PlacementRequested("tab"); QTRY_VERIFY(!Dock->isFloating()); QVERIFY(Dock->isInCentralArea()); QVERIFY(Dock->isCurrentTab()); QCOMPARE(Table->model()->rowCount(), 3);
            Panel->PlacementRequested("bottom"); QVERIFY(!Dock->isFloating()); QVERIFY(!Dock->isInCentralArea()); QVERIFY(Panel->isVisible());
            QTRY_COMPARE(Panel->window(),static_cast<QWidget*>(&Window));QTest::qWait(100);
            QVERIFY(Dock->isAncestorOf(Panel));
            QVERIFY(Window.rect().contains(QRect(Panel->mapTo(&Window,QPoint{}),Panel->size())));
            QVERIFY(Panel->height()>200);QVERIFY(Table->height()>40);
            const auto Screenshot = qEnvironmentVariable("HC_DOCUMENT_SEARCH_SCREENSHOT"); if (!Screenshot.isEmpty()) QVERIFY(Window.grab().save(Screenshot));
            auto* All = Panel->findChild<QPushButton*>("documentSearchReplace_all"); QVERIFY(All); QTRY_VERIFY_WITH_TIMEOUT(All->isEnabled(), 10000); All->click();
            QTRY_VERIFY_WITH_TIMEOUT(!Panel->Busy(), 15000); QCOMPARE(Pane->SearchText(), QString("world world\r\n"));
            QFile File(B); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("world\r\n")); File.close();
            auto* Undo = Panel->findChild<QPushButton*>("documentSearchReplace_undo"); QVERIFY(Undo); QTRY_VERIFY(Undo->isEnabled()); Undo->click();
            QTRY_VERIFY_WITH_TIMEOUT(!Panel->Busy(), 15000); QCOMPARE(Pane->SearchText(), QString("hello hello\r\n"));
            QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("hello\r\n")); File.close();
            for(const auto& Scope:QStringList{"selected","files"}) {
                QTRY_VERIFY_WITH_TIMEOUT(All->isEnabled(),15000);Table->scrollToTop();QTest::qWait(30);QTest::mouseClick(Table->viewport(),Qt::LeftButton,Qt::NoModifier,Table->visualRect(Table->model()->index(0,0)).center());
                auto* Button=Panel->findChild<QPushButton*>("documentSearchReplace_"+Scope);QVERIFY(Button);QVERIFY(Button->isEnabled());QVERIFY(Table->selectionModel()->isRowSelected(0,{}));Button->click();
                QTRY_VERIFY_WITH_TIMEOUT(!Panel->Busy(),15000);QCOMPARE(Pane->SearchText(),Scope=="selected"?QString("world hello\r\n"):QString("world world\r\n"));
                QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),QByteArray("hello\r\n"));File.close();
                Undo->click();QTRY_VERIFY_WITH_TIMEOUT(!Panel->Busy(),15000);QCOMPARE(Pane->SearchText(),QString("hello hello\r\n"));
            }
            Window.Handle({{"action", "close"}, {"documentId", Id}, {"discard", true}});
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(), 10000);
    }
    // The settings dialog shows "콘텐츠 탭 (기본)" when the key is absent; the workspace opens the same place.
    void findInFilesFollowsSetting_data() {
        QTest::addColumn<QString>("Stored");QTest::addColumn<bool>("InTab");
        QTest::newRow("absent")<<QString()<<true;QTest::newRow("tab")<<QString("tab")<<true;QTest::newRow("bottom")<<QString("bottom")<<false;
    }
    void findInFilesFollowsSetting() {
        QFETCH(QString,Stored);QFETCH(bool,InTab);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        qApp->setProperty("documentViewsConfigDir", Directory.filePath("settings"));
        if (!Stored.isEmpty()) { AppSettings Settings; Settings.setValue("textView/findInFiles/openLocation", Stored); Settings.sync(); }
        const auto Path = Directory.filePath("a.txt"); { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("alpha\r\n"); }
        {
            DocumentWorkspaceWidget Window(Directory.filePath("recovery")); Window.resize(1000, 700); Window.show();
            Window.SetClosePrompt([](const QString&) { return 1; });
            QVERIFY(!Window.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString().isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(Window.IsIdle(), 15000);
            QTest::keyClick(&Window, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
            auto* Panel = Window.findChild<search::Panel*>(); QVERIFY(Panel); QTRY_VERIFY(Panel->isVisible());
            test::DocumentTabs Tabs(&Window); QVERIFY(Tabs);
            auto* Dock = Tabs.tool("search"); QVERIFY(Dock); QVERIFY(Dock->isAncestorOf(Panel));
            QCOMPARE(Dock->isInCentralArea(), InTab); QVERIFY(Dock->isCurrentTab());
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(), 10000);
    }
    // A window of another thread under a pane keeps the Win32 keyboard when its tab is hidden, as the EPUB
    // reader process does. Qt never saw it as a focus widget, so each way of leaving the tab must hand it back.
    void foreignFocusLeavesHiddenTab() {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        qApp->setProperty("documentViewsConfigDir", Directory.filePath("settings"));
        const auto Put = [](const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        const auto A = Directory.filePath("a.txt"), B = Directory.filePath("b.txt"); QVERIFY(Put(A, "alpha\r\n")); QVERIFY(Put(B, "beta\r\n"));
        {
            DocumentWorkspaceWidget Window(Directory.filePath("recovery")); Window.resize(1000, 700); Window.show();
            Window.SetClosePrompt([](const QString&) { return 1; });
            QVERIFY(QTest::qWaitForWindowExposed(&Window));
            const auto IdA = Window.Handle({{"action", "open"}, {"path", A}})["documentId"].toString();
            const auto IdB = Window.Handle({{"action", "open"}, {"path", B}})["documentId"].toString();
            QVERIFY(!IdA.isEmpty() && !IdB.isEmpty()); QTRY_VERIFY_WITH_TIMEOUT(Window.IsIdle(), 15000);
            const auto Root = reinterpret_cast<HWND>(Window.winId());
            QVERIFY2(ActivateTestWindow(Root), "An active input desktop is required for native focus coverage");
            test::DocumentTabs Tabs(&Window); QVERIFY(Tabs); QCOMPARE(Tabs.count(), 2);
            const auto PaneOf = [&](int Index) { return qobject_cast<DocumentViewPane*>(Tabs.widget(Index)); };
            QVERIFY(PaneOf(0) && PaneOf(1));
            // The stand-in reader: a child window created and pumped by its own thread inside pane A.
            auto* Host = new QWidget(PaneOf(0)); Host->setGeometry(0, 0, 240, 120); Host->show();
            const auto HostWindow = reinterpret_cast<HWND>(Host->winId());
            std::atomic<HWND> Reader{}; std::atomic<bool> Stop{}, Stopped{};
            std::thread Thread([&] {
                Reader = CreateWindowExW(0, L"STATIC", L"foreign reader", WS_CHILD | WS_VISIBLE, 0, 0, 200, 100, HostWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
                MSG Message{}; while (!Stop) { while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&Message); Sleep(2); }
                if (Reader) DestroyWindow(Reader);
                Stopped = true;
            });
            // DestroyWindow notifies the parent on this thread: keep pumping until the reader thread is done.
            const auto Join = qScopeGuard([&] { Stop = true; (void)QTest::qWaitFor([&] { return Stopped.load(); }, 5000); Thread.join(); });
            QTRY_VERIFY(Reader.load());
            // Qt's focus widget sits outside the reader's page, as after a click into the reader.
            const auto EnterReader = [&] {
                Window.Handle({{"action", "activate"}, {"documentId", IdA}});
                if (Tabs.currentIndex() != 0 || !ActivateTestWindow(Root)) return false;
                Window.findChild<QWidget*>("mainToolbar")->setFocus(Qt::OtherFocusReason); SetFocus(Reader.load());
                return GetFocus() == Reader.load();
            };
            const auto KeyboardInB = [&] {
                const auto Focus = GetFocus(); auto* Widget = QApplication::focusWidget();
                return Focus && GetWindowThreadProcessId(Focus, nullptr) == GetCurrentThreadId() && Widget && PaneOf(1)->isAncestorOf(Widget);
            };
            QVERIFY(EnterReader());
            QVERIFY(Tabs.tabButton(1)); QTest::mouseClick(Tabs.tabButton(1), Qt::LeftButton, {}, Tabs.tabButton(1)->rect().center());
            QCOMPARE(Tabs.currentIndex(), 1); QTRY_VERIFY(KeyboardInB());
            QVERIFY(EnterReader());
            Window.Handle({{"action", "activate"}, {"documentId", IdB}}); QTRY_VERIFY(KeyboardInB());
            // A switch that did not release first (a wheel over the tab bar) repairs afterwards.
            QVERIFY(EnterReader());
            Tabs.setCurrentIndex(1); QTRY_VERIFY(KeyboardInB());
            QVERIFY(EnterReader());
            QTest::keyClick(&Window, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
            auto* Panel = Window.findChild<search::Panel*>(); QVERIFY(Panel); QTRY_VERIFY(Panel->isVisible() && Tabs.tool("search")->isCurrentTab());
            auto* Query = Panel->findChild<QLineEdit*>("documentSearchQuery"); QVERIFY(Query);
            QTRY_VERIFY(Query->hasFocus() && GetFocus() && GetWindowThreadProcessId(GetFocus(), nullptr) == GetCurrentThreadId());
        }
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(), 10000);
    }
};
int main(int Argc,char** Argv) {
    // The isolated UI harness intentionally removes inherited QT* variables.
    // Opt-in tree benchmarks need a longer QtTest watchdog inside this process.
    if(qEnvironmentVariableIsSet("HC_DOCUMENT_SEARCH_BENCH_ROOT"))qputenv("QTEST_FUNCTION_TIMEOUT","6600000");
    QApplication App(Argc,Argv);
    if(App.arguments().contains("--probe-dpr")){std::cout<<App.primaryScreen()->devicePixelRatio()<<std::endl;return 0;}
    DocumentSearchUiTest Test;return QTest::qExec(&Test,Argc,Argv);
}
#include "tstDocumentSearchUi.moc"
