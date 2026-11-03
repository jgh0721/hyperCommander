#include "stdafx.h"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/markdownDocumentView.hpp"
#include "UniqueLibs/document/markdownProcessView.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include <Scintilla.h>
#include <QtTest>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "tests/nativeTestInput.hpp"
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QFile>
#include <QListWidget>
#include <QFontComboBox>
#include <QScreen>
#include <QJsonDocument>
#include <iostream>
#include <qt_windows.h>
#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "core/AppSettings.h"
using namespace hc::document;
class MarkdownTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir config_;
    TextInput input()
    {
        TextInput I;
        I.path_ = config_.filePath("guide.md");
        I.text_ = "# 제목\n\n## 반복\n\n본문 $x^2$\n\n## 반복\n\n```mermaid\ngraph LR\n A-->B\n```\n\n```cpp\nint "
                  "main() {}\n```\n\n```text\n# 제목 아님\n```\n";
        I.bytes_ = I.text_.toUtf8();
        I.encoding_ = "UTF-8";
        I.eol_ = "LF";
        return I;
    }
  private slots:
    void initTestCase()
    {
        QVERIFY(config_.isValid());
        qApp->setProperty("documentViewsConfigDir", config_.path());
        QVERIFY2(DocumentShortcutRegistry::Validate({}).isEmpty(), qPrintable(DocumentShortcutRegistry::Validate({})));
    }
    void readerAndSource()
    {
        DocumentViewPane Pane;
        Pane.resize(1100, 800);
        QVERIFY(Pane.Load(input(), true, false));
        Pane.show();
        auto *Md = Pane.findChild<MarkdownDocumentView *>();
        QVERIFY(Md);
        QVERIFY(!Md->SourceVisible());
        QTRY_VERIFY2_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 qPrintable(QString::fromUtf8(QJsonDocument(Md->State()).toJson(QJsonDocument::Compact))),
                                 15000);
        QCOMPARE(Md->State()["problems"].toInt(), 0);
        auto *Tree = Md->findChild<QTreeWidget *>();
        QCOMPARE(Tree->topLevelItemCount(), 1);
        QCOMPARE(Tree->topLevelItem(0)->childCount(), 2);
        const auto Original = Pane.Content();
        QVERIFY(Pane.RunViewCommand("markdown.toggleSource"));
        QVERIFY(Md->SourceVisible());
        QCOMPARE(Pane.Content(), Original);
        QVERIFY(Pane.RunViewCommand("markdown.toggleSource"));
        for (const auto &F : Pane.StatusFields())
            QVERIFY(F.id_ != "position" && F.id_ != "encoding");
        Pane.raise();
        Pane.activateWindow();
        QTest::qWait(500);
        Pane.screen()
            ->grabWindow(0, Pane.mapToGlobal(QPoint()).x(), Pane.mapToGlobal(QPoint()).y(), Pane.width(), Pane.height())
            .save(QDir::currentPath() + "/_build/markdown-implementation/viewer.png");
    }
    void previewAlignment()
    {
        DocumentViewPane Pane;
        Pane.resize(1100, 700);
        QVERIFY(Pane.Load(input(), true, true));
        Pane.show();
        auto *Text = Pane.findChild<QTextView *>();
        auto *Preview = Pane.findChild<QWidget *>("markdownPreview");
        QVERIFY(Text && Preview);
        for (const bool Ruler : {true, false, true})
        {
            Text->setRulerVisible(Ruler);
            QTRY_COMPARE(Preview->mapToGlobal(QPoint{}).y(), Text->Backend()->widget()->mapToGlobal(QPoint{}).y());
        }
        QVERIFY(Pane.RunViewCommand("markdown.preview"));
        QTRY_COMPARE(Preview->geometry().top(), 0);
    }
    void splitAndSync()
    {
        DocumentViewPane Pane;
        Pane.resize(1280, 850);
        QVERIFY(Pane.Load(input(), true, true));
        Pane.show();
        auto *Md = Pane.findChild<MarkdownDocumentView *>();
        QCOMPARE(Md->State()["mode"].toString(), QString("split"));
        QVERIFY(!Md->CanRun("markdown.positionSync"));
        QVERIFY(Md->CanRun("markdown.syncScroll"));
        QVERIFY(Pane.RunViewCommand("markdown.syncScroll"));
        QVERIFY(!Md->State()["scrollSync"].toBool());
        QTRY_VERIFY2_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 qPrintable(QString::fromUtf8(QJsonDocument(Md->State()).toJson(QJsonDocument::Compact))),
                                 30000);
        Pane.raise();
        Pane.activateWindow();
        QTest::qWait(500);
        Pane.screen()
            ->grabWindow(0, Pane.mapToGlobal(QPoint()).x(), Pane.mapToGlobal(QPoint()).y(), Pane.width(), Pane.height())
            .save(QDir::currentPath() + "/_build/markdown-implementation/editor.png");
        auto *Text = Pane.findChild<QTextView *>();
        QVERIFY(Text);
        const auto Original = Pane.Content();
        Text->ReplaceContent("# 수정\n\n$$invalid\\unknown$$\n");
        QTRY_VERIFY2_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 qPrintable(QString::fromUtf8(QJsonDocument(Md->State()).toJson(QJsonDocument::Compact))),
                                 30000);
        QVERIFY(Md->State()["problems"].toInt() > 0);
        QVERIFY(Pane.Content() != Original);
        QVERIFY(Pane.RunViewCommand("markdown.preview"));
        QVERIFY(Pane.Content().contains("# "));
        QVERIFY(!Pane.Content().contains("<html"));
        Pane.grab().save(config_.filePath("editor.png"));
    }
    void previewFocus() {
        DocumentViewPane Pane;Pane.resize(1100,800);QVERIFY(Pane.Load(input(),true,true));Pane.show();Pane.activateWindow();
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Pane.winId())));QVERIFY(QTest::qWaitForWindowActive(&Pane));auto* Md=Pane.findChild<MarkdownDocumentView*>();QVERIFY(Md);
        auto* Input=Pane.InputWidget();QVERIFY(Input);Input->setFocus();
        for(int Pass=0;Pass<3;++Pass) {
            Md->Refresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2),Md->State()["revision"].toInteger(-1),30000);
            QTRY_VERIFY(Input->hasFocus());
            QTRY_COMPARE(GetFocus(),reinterpret_cast<HWND>(Input->effectiveWinId()));
        }
    }
    void previewStartupKeyboard()
    {
        TextInput I=input();I.text_="# Keyboard\n\n";
        for(int N=0;N<200;++N)I.text_+=QString("Paragraph %1 with enough text to scroll.\n\n").arg(N);
        I.bytes_=I.text_.toUtf8();
        DocumentViewPane Pane;Pane.resize(1100,800);QVERIFY(Pane.Load(I,true,false));Pane.show();Pane.activateWindow();
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Pane.winId())));QVERIFY(QTest::qWaitForWindowActive(&Pane));Pane.Focus();
        auto* Preview=Pane.findChild<MarkdownProcessView*>();auto* Md=Pane.findChild<MarkdownDocumentView*>();QVERIFY(Preview && Md);
        QTRY_VERIFY_WITH_TIMEOUT(Preview->HasFocus(),30000);
        const auto Key=[](WORD Code) {INPUT Events[2]{};for(auto& E:Events){E.type=INPUT_KEYBOARD;E.ki.wVk=Code;E.ki.dwFlags=KEYEVENTF_EXTENDEDKEY;}Events[1].ki.dwFlags|=KEYEVENTF_KEYUP;return SendInput(2,Events,sizeof(INPUT));};
        QTest::qWait(500);QCOMPARE(Key(VK_NEXT),UINT(2));
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["previewScroll"].toObject()["progress"].toInt()>0,5000);
        QSignalSpy Escape(&Pane,&DocumentViewPane::EscapeRequested);QCOMPARE(Key(VK_ESCAPE),UINT(2));QTRY_COMPARE(Escape.count(),1);
        QVERIFY(Pane.RunViewCommand("markdown.toggleSource"));QVERIFY(Md->SourceVisible());QTRY_VERIFY(Pane.InputWidget()->hasFocus());
        QVERIFY(Pane.RunViewCommand("markdown.toggleSource"));QTRY_VERIFY(Preview->HasFocus());
    }
    void continuousScrollSync_data()
    {
        QTest::addColumn<bool>("TallImage");QTest::addColumn<bool>("SourceWrap");QTest::addColumn<bool>("PreviewWrap");
        QTest::newRow("paragraphs")<<false<<true<<true;
        for(bool SourceWrap:{false,true})for(bool PreviewWrap:{false,true})
            QTest::newRow(qPrintable(QString("image-source%1-preview%2").arg(SourceWrap).arg(PreviewWrap)))<<true<<SourceWrap<<PreviewWrap;
    }
    void continuousScrollSync()
    {
        QFETCH(bool,TallImage);QFETCH(bool,SourceWrap);QFETCH(bool,PreviewWrap);QTemporaryDir Temp;int ImageLine=0;
        AppSettings Settings;Settings.setValue("markdown/scrollSync",true);Settings.sync();
        TextInput I=input();I.path_=Temp.filePath("guide.md");I.text_="# Scroll\n\n";
        QImage Image(420,1400,QImage::Format_RGB32);Image.fill(QColor("#6688aa"));QVERIFY(Image.save(Temp.filePath("tall.png")));
        for(int N=0;N<180;++N) {
            if(TallImage && N==45){ImageLine=I.text_.count('\n')+1;I.text_+="![Tall image](tall.png)\n\n";}
            const auto Paragraph=QString("Paragraph %1 ").arg(N)+QString("long paragraph with wrapped text ").repeated(15);
            I.text_+=PreviewWrap?Paragraph+"\n\n":QString(3,QChar(96))+"text\n"+Paragraph+"\n"+QString(3,QChar(96))+"\n\n";
        }
        I.bytes_=I.text_.toUtf8();DocumentViewPane Pane;Pane.resize(1300,850);QVERIFY(Pane.Load(I,true,true));Pane.show();Pane.activateWindow();
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Pane.winId())));QVERIFY(QTest::qWaitForWindowActive(&Pane));auto* Md=Pane.findChild<MarkdownDocumentView*>();QVERIFY(Md);
        QVERIFY(Pane.RunViewCommand("markdown.split"));Pane.Focus();
        auto* Text=Pane.findChild<QTextView*>();auto* Backend=static_cast<ScintillaQtDirectBackend*>(Text->Backend());
        Text->setWordWrap(SourceWrap);QTest::qWait(300);QCOMPARE(Backend->Send(SCI_GETWRAPMODE)!=SC_WRAP_NONE,SourceWrap);
        QTRY_COMPARE_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(),Md->State()["revision"].toInteger(),30000);
        QTRY_COMPARE_WITH_TIMEOUT(Md->State()["previewScroll"].toObject()["textRows"].toInt()>1,PreviewWrap,5000);
        const auto Aligned=[&]{
            const auto State=Md->State(),Source=State["sourceAnchor"].toObject(),Preview=State["previewScroll"].toObject(),Applied=Preview["appliedAnchor"].toObject();
            if(Source["caret"].toBool())return Applied["caret"].toBool() && qAbs(Applied["line"].toDouble()-Source["line"].toDouble())<.02
                && Applied["visible"].toBool() && (Applied["clamped"].toBool() || qAbs(Applied["actualRatio"].toDouble()-Source["ratio"].toDouble())<.05);
            return qAbs(Preview["line"].toDouble()-Backend->fractionalLineAtViewportRatio(.5))<1;
        };
        for(const double Line:{double(ImageLine?ImageLine:90)-20.2,double(ImageLine?ImageLine:90)-4.4,double(ImageLine?ImageLine:90)+.1,double(ImageLine?ImageLine:90)+.6,double(ImageLine?ImageLine:90)+20.4}){Backend->scrollFractionalLineToViewportRatio(Line,.5);QTRY_VERIFY2_WITH_TIMEOUT(Aligned(),qPrintable(QString("top %1 caret %2: %3").arg(Backend->firstVisibleLine()).arg(Backend->Send(SCI_GETCURRENTPOS)).arg(QString::fromUtf8(QJsonDocument(Md->State()).toJson(QJsonDocument::Compact)))),5000);}
        const auto Paragraph50=I.text_.indexOf("Paragraph 50 ");QVERIFY(Paragraph50>0);
        Backend->Send(SCI_GOTOPOS,I.text_.left(Paragraph50).toUtf8().size());Pane.Focus();
        QTRY_VERIFY_WITH_TIMEOUT(Aligned(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["previewScroll"].toObject()["position"].toObject()["visible"].toBool(),5000);
        QCOMPARE(Md->State()["previewScroll"].toObject()["position"].toObject()["line"].toInt(),Text->currentLine());
        // Moving within a display row must not scroll, on either side of a line break.
        const auto CaretLine=Backend->Send(SCI_LINEFROMPOSITION,Backend->Send(SCI_GETCURRENTPOS));
        const auto RowStart=Backend->Send(SCI_POSITIONFROMLINE,CaretLine),LineEnd=Backend->Send(SCI_GETLINEENDPOSITION,CaretLine);
        auto Boundary=LineEnd;
        if(SourceWrap) {
            const auto FirstY=Backend->Send(SCI_POINTYFROMPOSITION,0,RowStart);
            for(auto Pos=Backend->Send(SCI_POSITIONAFTER,RowStart);Pos<LineEnd;Pos=Backend->Send(SCI_POSITIONAFTER,Pos))
                if(Backend->Send(SCI_POINTYFROMPOSITION,0,Pos)>FirstY){Boundary=Pos;break;}
            QVERIFY(Boundary<LineEnd);
        }
        QList<qintptr> Bases{RowStart};
        if(SourceWrap)Bases.append(Backend->Send(SCI_POSITIONBEFORE,Backend->Send(SCI_POSITIONBEFORE,Boundary)));
        Bases.append(Backend->Send(SCI_POSITIONBEFORE,Backend->Send(SCI_POSITIONBEFORE,LineEnd)));
        int Crossings=0;
        for(const auto Base:Bases) {
            Backend->Send(SCI_GOTOPOS,Base);QTest::qWait(500);
            QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Pane.winId())));
            Backend->widget()->setFocus();SetFocus(reinterpret_cast<HWND>(Backend->widget()->effectiveWinId()));QTRY_VERIFY(Backend->widget()->hasFocus());QTRY_VERIFY_WITH_TIMEOUT(Aligned(),5000);
            for(int Step=0;Step<12;++Step) {
                const auto BeforePos=Backend->Send(SCI_GETCURRENTPOS),BeforeLine=Backend->Send(SCI_LINEFROMPOSITION,BeforePos);
                const auto BeforeY=Backend->Send(SCI_POINTYFROMPOSITION,0,BeforePos);
                const int BeforeTop=Backend->firstVisibleLine();const auto BeforeAnchor=Md->State()["sourceAnchor"].toObject();
                const double BeforeOffset=Md->State()["previewScroll"].toObject()["offset"].toDouble();
                QTest::keyClick(Backend->widget(),Step<6?Qt::Key_Right:Qt::Key_Left);QTest::qWait(350);
                const auto AfterPos=Backend->Send(SCI_GETCURRENTPOS);
                QVERIFY2(AfterPos!=BeforePos,qPrintable(QString("base %1 step %2 position %3 focus %4").arg(Base).arg(Step).arg(BeforePos).arg(Backend->widget()->hasFocus())));
                QTRY_VERIFY_WITH_TIMEOUT(Aligned(),5000);
                if(Backend->Send(SCI_LINEFROMPOSITION,AfterPos)==BeforeLine && Backend->Send(SCI_POINTYFROMPOSITION,0,AfterPos)==BeforeY && Backend->firstVisibleLine()==BeforeTop) {
                    QCOMPARE(Md->State()["sourceAnchor"].toObject(),BeforeAnchor);
                    QVERIFY2(qAbs(Md->State()["previewScroll"].toObject()["offset"].toDouble()-BeforeOffset)<2,"Horizontal caret movement scrolled the preview");
                } else ++Crossings;
            }
        }
        QVERIFY(Crossings>0);
        Backend->Send(SCI_GOTOPOS,I.text_.left(Paragraph50).toUtf8().size());Pane.Focus();QTRY_VERIFY_WITH_TIMEOUT(Aligned(),5000);
        for(int Step=0;Step<24;++Step){
            QTest::keyClick(Pane.InputWidget(),Step<12?Qt::Key_Down:Qt::Key_Up,Step%3==0?Qt::ControlModifier:Qt::NoModifier);
            QTest::qWait(350);QTRY_VERIFY2_WITH_TIMEOUT(Aligned(),qPrintable(QString::fromUtf8(QJsonDocument(Md->State()).toJson(QJsonDocument::Compact))),5000);
        }
        if(TallImage) {
            const auto Start=Backend->Send(SCI_POSITIONFROMLINE,ImageLine-1),End=Backend->Send(SCI_GETLINEENDPOSITION,ImageLine-1);
            Backend->Send(SCI_GOTOPOS,Start+(End-Start)/2);
            Backend->scrollFractionalLineToViewportRatio(ImageLine+.4,.5);QTRY_VERIFY_WITH_TIMEOUT(Aligned(),5000);QTest::qWait(400);
            const auto Offset=Md->State()["previewScroll"].toObject()["offset"].toDouble();
            for(int Pass=0;Pass<3;++Pass) {
                Md->Refresh(true);QTRY_COMPARE_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(),Md->State()["revision"].toInteger(),30000);
                for(int Sample=0;Sample<12;++Sample){QTest::qWait(50);QVERIFY2(qAbs(Md->State()["previewScroll"].toObject()["offset"].toDouble()-Offset)<2,qPrintable(QString("refresh %1 sample %2: %3 -> %4 source %5").arg(Pass).arg(Sample).arg(Offset).arg(Md->State()["previewScroll"].toObject()["offset"].toDouble()).arg(QString::fromUtf8(QJsonDocument(Md->State()["sourceAnchor"].toObject()).toJson(QJsonDocument::Compact)))));}
            }
        }
    }
    void viewerEscapeSetting_data()
    {
        QTest::addColumn<bool>("Close");QTest::newRow("enabled")<<true;QTest::newRow("disabled")<<false;
    }
    void viewerEscapeSetting()
    {
        QFETCH(bool,Close);QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        AppSettings Settings;Settings.setValue("common/closeViewerOnEscape",Close);Settings.sync();
        QFile File(Temp.filePath("keys.md"));QVERIFY(File.open(QIODevice::WriteOnly));File.write("# Keys\n");File.close();
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),true);Window.show();Window.activateWindow();
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Window.winId())));QVERIFY(QTest::qWaitForWindowActive(&Window));Window.Handle({{"action","open"},{"path",File.fileName()}});
        auto* Preview=Window.findChild<MarkdownProcessView*>();QTRY_VERIFY_WITH_TIMEOUT((Preview=Window.findChild<MarkdownProcessView*>()) && Preview->HasFocus(),30000);
        INPUT Events[2]{};for(auto& E:Events){E.type=INPUT_KEYBOARD;E.ki.wVk=VK_ESCAPE;}Events[1].ki.dwFlags=KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2,Events,sizeof(INPUT)),UINT(2));QTest::qWait(600);QCOMPARE(Window.Snapshot()["closeRequested"].toBool(),Close);
        qApp->setProperty("documentViewsConfigDir",config_.path());
    }
    void settings()
    {
        AppSettings Stored;
        const auto Path = Stored.fileName();
        {
            DocumentSettingsDialog Dialog(Path, {});
            Dialog.resize(1100, 950);
            Dialog.show();
            auto *List = Dialog.findChild<QListWidget *>();
            QVERIFY(List);
            List->setCurrentRow(4);
            auto *Enabled = Dialog.findChild<QCheckBox *>("documentViews/markdown/previewFonts/enabled");
            auto *Mode = Dialog.findChild<QComboBox *>("documentViews/markdown/previewFonts/mode");
            auto *Font = Dialog.findChild<QFontComboBox *>("documentViews/markdown/previewFonts/bodyFamily");
            auto *Position = Dialog.findChild<QCheckBox *>("documentViews/markdown/positionSync");
            auto *Scroll = Dialog.findChild<QCheckBox *>("documentViews/markdown/scrollSync");
            QVERIFY(Enabled && Mode && Font && !Position && Scroll);
            QTest::qWait(50);
            QVERIFY(!Enabled->isChecked());
            QVERIFY(!Mode->isEnabled());
            QVERIFY(!Font->isEnabled());
            Enabled->setChecked(true);
            Mode->setCurrentIndex(Mode->findData("user"));
            QVERIFY(Font->isEnabled());
            Scroll->setChecked(true);
            QVERIFY(Scroll->isEnabled());
            QVERIFY(Scroll->isChecked());
            Dialog.grab().save(QDir::currentPath() + "/_build/markdown-implementation/settings.png");
            QVERIFY(Dialog.Apply());
        }
        AppSettings Reloaded;
        QVERIFY(Reloaded.value("markdown/previewFonts/enabled").toBool());
        QCOMPARE(Reloaded.value("markdown/previewFonts/mode").toString(), QString("user"));

        QVERIFY(Reloaded.value("markdown/scrollSync").toBool());
        {
            DocumentSettingsDialog Cancel(Path, {});
            Cancel.findChild<QCheckBox *>("documentViews/markdown/previewFonts/enabled")->setChecked(false);
            Cancel.reject();
        }
        AppSettings AfterCancel;
        QVERIFY(AfterCancel.value("markdown/previewFonts/enabled").toBool());
    }
};
int main(int Argc,char** Argv) {
    QApplication App(Argc,Argv);
    if(App.arguments().contains("--probe-dpr")){std::cout<<App.primaryScreen()->devicePixelRatio()<<std::endl;return 0;}
    MarkdownTests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstMarkdown.moc"
