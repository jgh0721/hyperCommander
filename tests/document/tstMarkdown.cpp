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
#include <QScopeGuard>
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
#include <QJsonArray>
#include <QRegularExpression>
#include <QAbstractScrollArea>
#include <QMenu>
#include <QToolBar>
#include <QClipboard>
#include <QTimer>
#include <QApplication>
#include <functional>
#include <algorithm>
#include <limits>
#include <iostream>
#include <qt_windows.h>
#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "core/AppSettings.h"
using namespace hc::document;
// D4: the anchor's text and its rendered copy sit at the same screen height whenever both are
// shown. Every word of a test paragraph is a unique token, so a source row and its
// rendered line are matched by text, not by the mapping under test.
struct SyncProbe
{
    MarkdownDocumentView *md_ = nullptr;
    QTextView *text_ = nullptr;
    ScintillaQtDirectBackend *backend_ = nullptr;
    QWidget *viewport_ = nullptr;
    MarkdownProcessView *preview_ = nullptr;
    int id_ = 0;
    QString failure_;
    QList<double> gaps_;
    explicit SyncProbe(QWidget &Pane)
        : md_(Pane.findChild<MarkdownDocumentView *>()), text_(Pane.findChild<QTextView *>()),
          backend_(text_ ? static_cast<ScintillaQtDirectBackend *>(text_->Backend()) : nullptr),
          viewport_(backend_ ? backend_->scrollArea()->viewport() : nullptr), preview_(Pane.findChild<MarkdownProcessView *>()) {}
    bool valid() const { return md_ && text_ && backend_ && viewport_ && preview_; }
    QJsonObject measure(QJsonObject Request)
    {
        Request["id"] = ++id_; md_->RequestDiagnostics(Request);
        if (!QTest::qWaitFor([&] { return md_->State()["diagnostics"].toObject()["id"].toInt() == id_; }, 3000)) return {};
        return md_->State()["diagnostics"].toObject();
    }
    static QString tokenParagraph(int N)
    {
        QStringList Words{QString("Paragraph %1").arg(N)};
        // Every word is a token, so the token holding the anchor character is the measured one.
        for (int W = 0; W < 60; ++W) Words.append(QString("q%1t%2").arg(N).arg(W));
        return Words.join(' ');
    }
    bool synced()
    {
        const auto State = md_->State(), Source = State["sourceAnchor"].toObject(), View = State["previewScroll"].toObject();
        if (View["appliedSerial"].toInteger() != State["scrollRequests"].toInteger()) { failure_ = "request pending"; return false; }
        if (Source["top"].toBool()) { failure_ = "top"; return View["offset"].toDouble() <= 1; }
        if (Source["end"].toBool()) { failure_ = "end"; return View["bottom"].toBool(); }
        // Measure only once the preview has applied the current anchor; an edit's line shift
        // changes the line number the preview sees, never the column or the screen offset.
        const auto Applied = View["appliedAnchor"].toObject();
        if ((State["lineShifts"].toInt() == 0 && Applied["line"].toDouble() != Source["line"].toDouble())
            || Applied["column"].toInt(-1) != Source["column"].toInt() || qAbs(Applied["offset"].toDouble() - Source["offset"].toDouble()) > .5) {
            failure_ = "anchor not applied yet"; return false;
        }
        const int Line = Source["line"].toInt() - 1; const auto Start = backend_->Send(SCI_POSITIONFROMLINE, quintptr(Line));
        const auto At = backend_->Send(SCI_POSITIONRELATIVECODEUNITS, quintptr(Start), Source["column"].toInt());
        const auto RowY = backend_->Send(SCI_POINTYFROMPOSITION, 0, At);
        if (RowY < 0 || RowY >= viewport_->height()) { failure_ = "anchor row hidden"; return false; }
        const double SourceY = viewport_->mapToGlobal(QPoint(0, int(RowY))).y(), PreviewTop = preview_->mapToGlobal(QPoint(0, 0)).y();
        const auto LineText = text_->Content().section('\n', Line, Line);
        if (LineText.startsWith("![")) {
            // An image line: the rendered image is shown around the row's height.
            const auto Box = measure({{"images", QJsonArray{"tall.png"}}})["images"].toObject()["tall.png"].toObject();
            if (Box.isEmpty() || !Box["visible"].toBool()) { failure_ = "image hidden"; return false; }
            const double Top = PreviewTop + Box["top"].toDouble(), Bottom = PreviewTop + Box["bottom"].toDouble();
            failure_ = QString("image %1..%2 row %3").arg(Top).arg(Bottom).arg(SourceY);
            return Bottom - Top > preview_->height() ? qAbs(Top - SourceY) <= 26 : SourceY >= Top - 2 && SourceY <= Bottom + 2;
        }
        static const QRegularExpression TokenPattern("q\\d+t\\d+");
        // The token holding the anchor character, else the nearest one on its row. Character wrap
        // can start that token on the previous row; it is still the anchor's text. A space at a
        // soft wrap hangs at the end of the previous rendered line, so the token before it wins ties.
        QString Token; qintptr Best = std::numeric_limits<qintptr>::max();
        for (auto Match = TokenPattern.globalMatch(LineText); Match.hasNext();) {
            const auto M = Match.next(); const auto Position = Start + qintptr(M.capturedStart()), End = Position + M.capturedLength();
            const bool Holds = Position <= At && At < End;
            if (!Holds && backend_->Send(SCI_POINTYFROMPOSITION, 0, Position) != RowY) continue;
            const qintptr Distance = Holds ? 0 : At >= End ? (At - End) * 2 : (Position - At) * 2 + 1;
            if (Distance < Best) { Token = M.captured(); Best = Distance; }
        }
        if (Token.isEmpty()) return true;   // nothing on this row to compare
        const auto Box = measure({{"tokens", QJsonArray{Token}}})["tokens"].toObject()[Token].toObject();
        if (Box.isEmpty() || !Box["visible"].toBool()) { failure_ = QString("token %1 hidden").arg(Token); return false; }
        if (View["appliedAnchor"].toObject()["clamped"].toBool()) return true;
        const double Gap = qAbs(PreviewTop + Box["top"].toDouble() - SourceY), Tolerance = qMax(20.0, Box["height"].toDouble() * 1.4);
        failure_ = QString("token %1 gap %2 > %3").arg(Token).arg(Gap).arg(Tolerance);
        if (Gap > Tolerance) return false;
        if (Gap > 8 && qEnvironmentVariableIsSet("HC_SYNC_TRACE")) qInfo().noquote() << "sync gap" << Gap << "token" << Token << "anchor" << QString::fromUtf8(QJsonDocument(Source).toJson(QJsonDocument::Compact)) << "row" << RowY << "tokenTop" << Box["top"].toDouble() << "applied" << QString::fromUtf8(QJsonDocument(View["appliedAnchor"].toObject()).toJson(QJsonDocument::Compact));
        gaps_.append(Gap); return true;
    }
    // Preview-driven: the preview's reference character sits at its source row's height.
    bool previewSynced()
    {
        const auto View = md_->State()["previewScroll"].toObject();
        if (!View["column"].isDouble()) { failure_ = "preview reference has no character"; return false; }
        const int Line = int(View["line"].toDouble()) - 1; const auto Start = backend_->Send(SCI_POSITIONFROMLINE, quintptr(Line));
        const auto At = backend_->Send(SCI_POSITIONRELATIVECODEUNITS, quintptr(Start), View["column"].toInt());
        const double SourceY = viewport_->mapToGlobal(QPoint(0, int(backend_->Send(SCI_POINTYFROMPOSITION, 0, At)))).y();
        const double PreviewY = preview_->mapToGlobal(QPoint(0, 0)).y() + View["anchorOffset"].toDouble();
        const auto Row = double(backend_->Send(SCI_TEXTHEIGHT, quintptr(Line)));
        failure_ = QString("preview reference %1 source row %2 > %3").arg(PreviewY).arg(SourceY).arg(Row);
        if (qAbs(SourceY - PreviewY) > Row) return false;
        gaps_.append(qAbs(SourceY - PreviewY)); return true;
    }
    bool check(const QString &Where)
    {
        if (QTest::qWaitFor([&] { return synced(); }, 5000)) return true;
        qWarning().noquote() << Where << failure_ << QString::fromUtf8(QJsonDocument(md_->State()).toJson(QJsonDocument::Compact)).left(1500);
        return false;
    }
    void summary(const QString &Name)
    {
        auto Sorted = gaps_; std::sort(Sorted.begin(), Sorted.end());
        if (!Sorted.isEmpty())
            qInfo().noquote() << QString("%1 sync accuracy: samples=%2 medianPx=%3 p95Px=%4 maxPx=%5").arg(Name).arg(Sorted.size())
                .arg(Sorted[Sorted.size() / 2], 0, 'f', 1).arg(Sorted[qMin(Sorted.size() - 1, int(Sorted.size() * .95))], 0, 'f', 1).arg(Sorted.last(), 0, 'f', 1);
    }
};
// Physical-pixel input to the native preview window, which belongs to another process.
static QRect nativeRect(QWidget *Widget)
{
    RECT Rect{}; GetWindowRect(reinterpret_cast<HWND>(Widget->winId()), &Rect);
    return QRect(QPoint(Rect.left, Rect.top), QPoint(Rect.right - 1, Rect.bottom - 1));
}
static void sendMouse(DWORD Flags, int Data = 0)
{
    INPUT Input{}; Input.type = INPUT_MOUSE; Input.mi.dwFlags = Flags; Input.mi.mouseData = DWORD(Data); SendInput(1, &Input, sizeof(INPUT));
}
// Context menus run their own event loop: Inspect sees the next popup menu, which is then closed.
static void onNextMenu(std::function<void(QMenu *)> Inspect)
{
    auto *Poll = new QTimer(qApp);
    Poll->setInterval(50);
    QObject::connect(Poll, &QTimer::timeout, qApp, [Poll, Inspect = std::move(Inspect), Tries = 0]() mutable {
        auto *Menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!Menu && ++Tries < 200)
            return;
        Poll->stop();
        Poll->deleteLater();
        if (Menu)
        {
            Inspect(Menu);
            Menu->close();
        }
    });
    Poll->start();
}
    // A split view whose preview shows token paragraphs, with the source focused.
    struct SyncFixture
    {
        QTemporaryDir temp_;
        std::unique_ptr<DocumentViewPane> pane_;
        TextInput input_;
        bool open(int Paragraphs, const QSize &Size = {1300, 850})
        {
            AppSettings Settings;Settings.setValue("markdown/scrollSync",true);Settings.setValue("markdown/autoPreview",true);Settings.sync();
            input_.path_=temp_.filePath("sync.md");input_.encoding_="UTF-8";input_.eol_="LF";input_.text_="# Sync\n\n";
            for(int N=0;N<Paragraphs;++N)input_.text_+=SyncProbe::tokenParagraph(N)+"\n\n";
            input_.bytes_=input_.text_.toUtf8();pane_=std::make_unique<DocumentViewPane>();pane_->resize(Size);
            if(!pane_->Load(input_,true,true))return false;pane_->show();pane_->activateWindow();
            if(!ActivateTestWindow(reinterpret_cast<HWND>(pane_->winId())) || !QTest::qWaitForWindowActive(pane_.get()))return false;
            if(!pane_->RunViewCommand("markdown.split"))return false;pane_->Focus();
            auto* Md=pane_->findChild<MarkdownDocumentView*>();
            return Md && QTest::qWaitFor([&]{return Md->State()["renderedRevision"].toInteger()==Md->State()["revision"].toInteger();},30000);
        }
    };
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
    void syncSettingMigration_data()
    {
        QTest::addColumn<bool>("Position");QTest::addColumn<bool>("Scroll");QTest::addColumn<bool>("Expected");
        QTest::newRow("both-on")<<true<<true<<true;
        QTest::newRow("position-only")<<true<<false<<false;
        // Scroll sync without position sync never ran; it migrates as off.
        QTest::newRow("scroll-only")<<false<<true<<false;
        QTest::newRow("both-off")<<false<<false<<false;
    }
    void syncSettingMigration()
    {
        QFETCH(bool,Position);QFETCH(bool,Scroll);QFETCH(bool,Expected);
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Restore=qScopeGuard([&]{qApp->setProperty("documentViewsConfigDir",config_.path());});
        {AppSettings S;S.setValue("markdown/positionSync",Position);S.setValue("markdown/scrollSync",Scroll);S.sync();}
        for(int Run=0;Run<2;++Run){
            DocumentViewPane Pane;TextInput I=input();I.path_=Temp.filePath("guide.md");QVERIFY(Pane.Load(I,true,true));
            auto* Md=Pane.findChild<MarkdownDocumentView*>();QVERIFY(Md);
            QCOMPARE(Md->State()["scrollSync"].toBool(),Expected);QVERIFY(!Md->State().contains("positionSync"));
            AppSettings S;QVERIFY(!S.contains("markdown/positionSync"));QCOMPARE(S.value("markdown/scrollSync").toBool(),Expected);
        }
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
        QCOMPARE(Md->RenderProblemCount(), Md->State()["problems"].toInt());
        bool ProblemField = false;
        for (const auto &F : Md->StatusFields())
            ProblemField = ProblemField || (F.id_ == "render" && F.text_ == QStringLiteral("렌더링 문제 %1건").arg(Md->RenderProblemCount()));
        QVERIFY(ProblemField);
        QVERIFY(Pane.Content() != Original);
        QVERIFY(Pane.RunViewCommand("markdown.preview"));
        QVERIFY(Pane.Content().contains("# "));
        QVERIFY(!Pane.Content().contains("<html"));
        Pane.grab().save(config_.filePath("editor.png"));
    }
    // Plan 8.2: one toolbar row, picker status fields with their data, and the Korean context menus.
    void chromeToolbarStatusAndMenus()
    {
        DocumentViewPane Pane;
        Pane.resize(1100, 800);
        QVERIFY(Pane.Load(input(), true, true));
        Pane.show();
        auto *Md = Pane.findChild<MarkdownDocumentView *>();
        QVERIFY(Md);
        QVERIFY(Pane.RunViewCommand("markdown.split"));
        auto *Bar = Md->Toolbar();
        QCOMPARE(Bar->objectName(), QString("markdownToolbar"));
        const auto Find = [Bar](std::initializer_list<const char *> Names) -> QAction * {
            for (auto *A : Bar->actions())
                for (const auto *Name : Names)
                    if (A->objectName() == QLatin1String(Name))
                        return A;
            return nullptr;
        };
        for (const auto *Id : {"markdown.preview", "markdown.source", "markdown.split", "markdown.toggleToc", "markdown.syncScroll",
                               "markdown.zoomOut", "markdown.zoomIn", "markdown.refresh", "markdown.find"})
            QVERIFY2(Find({Id}), Id);
        // The text editor's replace, wrap and whitespace join the row; its own bar stays hidden.
        auto *Replace = Find({"textToolbar.replace", "text.replaceBar"});
        auto *Wrap = Find({"textToolbar.wordWrap", "text.wordWrap"});
        auto *Whitespace = Find({"textToolbar.whitespace", "text.whitespace"});
        QVERIFY(Replace && Wrap && Whitespace);
        QVERIFY(!Find({"textToolbar.find", "text.find", "text.readOnly"}));
        QToolBar *SourceBar = nullptr;
        for (auto *Other : Pane.findChildren<QToolBar *>())
            if (Other != Bar && Other->findChild<QAction *>("text.readOnly", Qt::FindDirectChildrenOnly))
                SourceBar = Other;
        QVERIFY(SourceBar && !SourceBar->isVisible());
        QVERIFY(Replace->isVisible() && Wrap->isVisible() && Whitespace->isVisible());
        QVERIFY(Pane.RunViewCommand("markdown.preview"));
        QVERIFY(!Replace->isVisible() && !Wrap->isVisible() && !Whitespace->isVisible());
        QTRY_VERIFY2_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 qPrintable(QString::fromUtf8(QJsonDocument(Md->State()).toJson(QJsonDocument::Compact))),
                                 30000);

        // Status fields: heading · zoom · render are pickers; the zoom starts the right-aligned group.
        const auto Field = [Md](const char *Id) {
            for (const auto &F : Md->StatusFields())
                if (F.id_ == QLatin1String(Id))
                    return F;
            return DocumentStatusField{};
        };
        QVERIFY(Field("heading").picker_);
        QVERIFY(Field("heading").text_.endsWith(" %"));
        QVERIFY(Field("mdzoom").picker_ && Field("mdzoom").trailing_);
        QCOMPARE(Field("mdzoom").text_, QStringLiteral("미리보기 100 %"));
        QVERIFY(Field("render").picker_);
        QCOMPARE(Field("render").text_, QStringLiteral("렌더링 최신"));
        QVERIFY(!Field("markdown.viewMode").id_.isEmpty());
        QCOMPARE(Md->RenderProblemCount(), 0);
        QVERIFY(Md->RenderedAt().isValid());

        // Zoom: direct value, command with a value, reset, bounds.
        Md->SetPreviewZoom(125);
        QCOMPARE(Md->PreviewZoomPercent(), 125);
        QCOMPARE(Field("mdzoom").text_, QStringLiteral("미리보기 125 %"));
        QVERIFY(Md->Run("markdown.zoomReset"));
        QCOMPARE(Md->PreviewZoomPercent(), 100);
        QVERIFY(Md->CanRun("markdown.zoomSet"));
        QVERIFY(Md->Run("markdown.zoomSet", 150));
        QCOMPARE(Md->State()["zoom"].toInt(), 150);
        Md->SetPreviewZoom(1000);
        QCOMPARE(Md->PreviewZoomPercent(), MarkdownDocumentView::MaxZoomPercent);
        QVERIFY(Md->Run("markdown.zoomReset"));

        // Headings: outline order, source lines and depth.
        const auto Headings = Md->Headings();
        QCOMPARE(Headings.size(), 3);
        QCOMPARE(Headings[0].first, QStringLiteral("제목"));
        QCOMPARE(Headings[0].second, 1);
        QCOMPARE(Headings[1].second, 3);
        QCOMPARE(Headings[2].second, 7);
        QCOMPARE(Md->HeadingDepth(0), 0);
        QCOMPARE(Md->HeadingDepth(2), 1);
        Md->GoToHeading(2);
        QCOMPARE(Md->CurrentHeadingIndex(), 2);

        // Preview menu: the renderer's report opens the host's Korean menu.
        auto *Preview = Md->findChild<MarkdownProcessView *>();
        QVERIFY(Preview);
        QStringList Labels;
        onNextMenu([&](QMenu *Menu) {
            for (auto *A : Menu->actions())
                if (!A->isSeparator())
                    Labels << A->text();
        });
        emit Preview->Reported({{"type", "context"}, {"x", 30}, {"y", 30}, {"selection", ""}, {"link", "https://example.com/"},
                                {"fragment", ""}, {"line", 1}, {"column", 0}});
        QTRY_VERIFY(!Labels.isEmpty());
        QCOMPARE(Labels, (QStringList{QStringLiteral("복사"), QStringLiteral("전체 선택"), QStringLiteral("소스에서 이 위치 보기"),
                                      QStringLiteral("링크 열기"), QStringLiteral("링크 주소 복사"), QStringLiteral("미리보기 확대"),
                                      QStringLiteral("미리보기 축소"), QStringLiteral("미리보기 원래 크기"), QStringLiteral("미리보기 갱신"),
                                      QStringLiteral("개요"), QStringLiteral("스크롤 동기화")}));

        // Outline menu, and its copy item.
        // The toolbar (and its actions) lives in the pane's layout, outside the Markdown view.
        auto *Toc = Md->Toolbar()->findChild<QAction *>("markdown.toggleToc");
        auto *Tree = Md->findChild<QTreeWidget *>();
        QVERIFY(Toc && Tree);
        const bool TocShown = Toc->isChecked();
        if (!TocShown)
            Toc->trigger();
        QTRY_VERIFY(Tree->isVisible());
        QStringList OutlineLabels;
        onNextMenu([&](QMenu *Menu) {
            for (auto *A : Menu->actions())
            {
                if (A->isSeparator())
                    continue;
                OutlineLabels << A->text();
                if (A->text() == QStringLiteral("제목 텍스트 복사"))
                    A->trigger();
            }
        });
        emit Tree->customContextMenuRequested(Tree->visualItemRect(Tree->topLevelItem(0)).center());
        QTRY_VERIFY(!OutlineLabels.isEmpty());
        QCOMPARE(OutlineLabels, (QStringList{QStringLiteral("이 제목으로 이동"), QStringLiteral("모두 펼치기"), QStringLiteral("모두 접기"),
                                             QStringLiteral("제목 텍스트 복사")}));
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("제목"));
        if (!TocShown)
            Toc->trigger();

        // Source menu's "미리보기에서 이 위치 보기" shows the preview beside a source-only view.
        QVERIFY(Pane.RunViewCommand("markdown.source"));
        QVERIFY(!Preview->isVisible());
        Md->ShowSourceLineInPreview();
        QCOMPARE(Md->State()["mode"].toString(), QString("split"));
        QTRY_VERIFY(Preview->isVisible());
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
            const auto Paragraph=SyncProbe::tokenParagraph(N);
            I.text_+=PreviewWrap?Paragraph+"\n\n":QString(3,QChar(96))+"text\n"+Paragraph+"\n"+QString(3,QChar(96))+"\n\n";
        }
        I.bytes_=I.text_.toUtf8();DocumentViewPane Pane;Pane.resize(1300,850);QVERIFY(Pane.Load(I,true,true));Pane.show();Pane.activateWindow();
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Pane.winId())));QVERIFY(QTest::qWaitForWindowActive(&Pane));auto* Md=Pane.findChild<MarkdownDocumentView*>();QVERIFY(Md);
        QVERIFY(Pane.RunViewCommand("markdown.split"));Pane.Focus();
        SyncProbe Probe(Pane);QVERIFY(Probe.valid());auto* Text=Probe.text_;auto* Backend=Probe.backend_;
        Text->setWordWrap(SourceWrap);QTest::qWait(300);QCOMPARE(Backend->Send(SCI_GETWRAPMODE)!=SC_WRAP_NONE,SourceWrap);
        QTRY_COMPARE_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(),Md->State()["revision"].toInteger(),30000);
        QCOMPARE(Probe.measure({{"textRows",true}})["textRows"].toInt()>1,PreviewWrap);
        for(const double Line:{double(ImageLine?ImageLine:90)-20.2,double(ImageLine?ImageLine:90)-4.4,double(ImageLine?ImageLine:90)+.1,double(ImageLine?ImageLine:90)+.6,double(ImageLine?ImageLine:90)+20.4}){
            Backend->scrollFractionalLineToViewportRatio(Line,.5);QVERIFY(Probe.check(QString("scroll %1").arg(Line)));
        }
        const auto Paragraph50=I.text_.indexOf("Paragraph 50 ");QVERIFY(Paragraph50>0);
        Backend->Send(SCI_GOTOPOS,I.text_.left(Paragraph50).toUtf8().size());Pane.Focus();
        QVERIFY(Probe.check("caret at paragraph 50"));
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["previewScroll"].toObject()["position"].toObject()["visible"].toBool(),5000);
        QCOMPARE(Md->State()["previewScroll"].toObject()["position"].toObject()["line"].toInt(),Text->currentLine());
        // D5: horizontal moves keep the caret's character aligned. The preview moves only when that
        // character's rendered line changes, at most once per key, and never back and forth.
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
        const auto Where=[&]{const auto A=Md->State()["sourceAnchor"].toObject();return std::pair{A["line"].toInt(),A["column"].toInt()};};
        for(const auto Base:Bases) {
            Backend->Send(SCI_GOTOPOS,Base);
            QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Pane.winId())));
            Backend->widget()->setFocus();SetFocus(reinterpret_cast<HWND>(Backend->widget()->effectiveWinId()));QTRY_VERIFY(Backend->widget()->hasFocus());
            QVERIFY(Probe.check(QString("base %1").arg(Base)));
            for(int Step=0;Step<8;++Step) {
                const bool Right=Step<4;const auto BeforePos=Backend->Send(SCI_GETCURRENTPOS),BeforeLine=Backend->Send(SCI_LINEFROMPOSITION,BeforePos);
                const auto BeforeRow=Backend->Send(SCI_POINTYFROMPOSITION,0,BeforePos);const auto BeforeAnchor=Where();
                const auto BeforeRequests=Md->State()["scrollRequests"].toInteger();
                QTest::keyClick(Backend->widget(),Right?Qt::Key_Right:Qt::Key_Left);
                const auto AfterPos=Backend->Send(SCI_GETCURRENTPOS);
                QVERIFY2(AfterPos!=BeforePos,qPrintable(QString("base %1 step %2 position %3 focus %4").arg(Base).arg(Step).arg(BeforePos).arg(Backend->widget()->hasFocus())));
                QVERIFY(Probe.check(QString("base %1 step %2").arg(Base).arg(Step)));
                QVERIFY2(Md->State()["scrollRequests"].toInteger()-BeforeRequests<=1,"One key sent several scroll requests");
                const auto AfterAnchor=Where();
                QVERIFY2(Right?AfterAnchor>=BeforeAnchor:AfterAnchor<=BeforeAnchor,"The anchor moved against the caret");
                if(Backend->Send(SCI_LINEFROMPOSITION,AfterPos)!=BeforeLine || Backend->Send(SCI_POINTYFROMPOSITION,0,AfterPos)!=BeforeRow)++Crossings;
            }
        }
        QVERIFY(Crossings>0);
        Backend->Send(SCI_GOTOPOS,I.text_.left(Paragraph50).toUtf8().size());Pane.Focus();QVERIFY(Probe.check("caret back at paragraph 50"));
        for(int Step=0;Step<24;++Step){
            QTest::keyClick(Pane.InputWidget(),Step<12?Qt::Key_Down:Qt::Key_Up,Step%3==0?Qt::ControlModifier:Qt::NoModifier);
            QVERIFY(Probe.check(QString("vertical step %1").arg(Step)));
        }
        // W9-3: a width change reflows both panes; the preview re-applies the anchor by itself.
        Pane.resize(1050,850);QVERIFY(Probe.check("narrower window"));Pane.resize(1300,850);QVERIFY(Probe.check("wider window"));
        if(TallImage) {
            const auto Start=Backend->Send(SCI_POSITIONFROMLINE,ImageLine-1),End=Backend->Send(SCI_GETLINEENDPOSITION,ImageLine-1);
            Backend->Send(SCI_GOTOPOS,Start+(End-Start)/2);
            Backend->scrollFractionalLineToViewportRatio(ImageLine+.4,.5);QVERIFY(Probe.check("image line"));QTest::qWait(400);
            const auto Offset=Md->State()["previewScroll"].toObject()["offset"].toDouble();
            for(int Pass=0;Pass<3;++Pass) {
                Md->Refresh(true);QTRY_COMPARE_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(),Md->State()["revision"].toInteger(),30000);
                for(int Sample=0;Sample<12;++Sample){QTest::qWait(50);QVERIFY2(qAbs(Md->State()["previewScroll"].toObject()["offset"].toDouble()-Offset)<2,qPrintable(QString("refresh %1 sample %2: %3 -> %4 source %5").arg(Pass).arg(Sample).arg(Offset).arg(Md->State()["previewScroll"].toObject()["offset"].toDouble()).arg(QString::fromUtf8(QJsonDocument(Md->State()["sourceAnchor"].toObject()).toJson(QJsonDocument::Compact)))));}
            }
        }
        // W9-5: nothing is computed while the editor is idle.
        QTest::qWait(300);const auto Syncs=Md->State()["anchorSyncs"].toInteger();QTest::qWait(1000);QCOMPARE(Md->State()["anchorSyncs"].toInteger(),Syncs);
        Probe.summary(QString("Markdown %1").arg(QTest::currentDataTag()));
    }
    void previewWheelWithoutFocusMovesSource()
    {
        // D2: the pointer scrolls the preview under it while the source keeps the focus; the
        // source follows and the preview does not jump back.
        SyncFixture F;QVERIFY(F.open(220));SyncProbe Probe(*F.pane_);QVERIFY(Probe.valid());
        auto* Md=Probe.md_;auto* Backend=Probe.backend_;QTRY_VERIFY(Backend->widget()->hasFocus());
        QVERIFY(Probe.check("initial"));const int Top=Backend->firstVisibleLine();
        const auto Rect=nativeRect(Probe.preview_);QVERIFY(Rect.isValid());SetCursorPos(Rect.center().x(),Rect.center().y());QTest::qWait(100);
        for(int Notch=0;Notch<6;++Notch){sendMouse(MOUSEEVENTF_WHEEL,-WHEEL_DELTA);QTest::qWait(60);}
        QTRY_VERIFY_WITH_TIMEOUT(Backend->firstVisibleLine()>Top,5000);
        QVERIFY(Backend->widget()->hasFocus());QVERIFY(!Md->State()["previewScroll"].toObject()["top"].toBool());
        QTest::qWait(400);const auto Offset=Md->State()["previewScroll"].toObject()["offset"].toDouble();
        // The source's own echo must not move the preview the user placed.
        QTest::qWait(1000);QVERIFY(qAbs(Md->State()["previewScroll"].toObject()["offset"].toDouble()-Offset)<2);
        QVERIFY2(Probe.previewSynced(),qPrintable(Probe.failure_));
        Probe.summary("Markdown preview wheel");
    }
    void previewScrollWithoutPageInputKeepsSync()
    {
        // Scrollbar drags and presses deliver no page input events. A preview scroll that no
        // request, input or layout change explains counts as the user's, so the source follows.
        // Find-in-page scrolls the same way and is driven here without synthetic hit testing.
        SyncFixture F;QVERIFY(F.open(400));SyncProbe Probe(*F.pane_);QVERIFY(Probe.valid());
        auto* Backend=Probe.backend_;QVERIFY(Probe.check("initial"));const int Top=Backend->firstVisibleLine();
        QTest::qWait(400);Probe.preview_->Send({{"type","find"},{"text","q300t30"}});
        QTRY_VERIFY_WITH_TIMEOUT(Probe.md_->State()["previewScroll"].toObject()["intent"].toString()=="scrollbar",5000);
        QTRY_VERIFY_WITH_TIMEOUT(Backend->firstVisibleLine()>Top+100,5000);
        QVERIFY2(QTest::qWaitFor([&]{return Probe.previewSynced();},3000),qPrintable(Probe.failure_));
        Probe.summary("Markdown scroll without page input");
    }
    void previewAnchorTableIsCached()
    {
        // W5: scrolling measures one block per frame; the block table is built per layout.
        SyncFixture F;QVERIFY(F.open(2500));SyncProbe Probe(*F.pane_);QVERIFY(Probe.valid());auto* Md=Probe.md_;
        Md->Run("markdown.focusOtherPane");QTRY_VERIFY_WITH_TIMEOUT(Probe.preview_->HasFocus(),10000);
        const auto Builds=Md->State()["previewScroll"].toObject()["anchorBuilds"].toInteger();
        INPUT Events[2]{};for(auto& E:Events){E.type=INPUT_KEYBOARD;E.ki.wVk=VK_NEXT;E.ki.dwFlags=KEYEVENTF_EXTENDEDKEY;}Events[1].ki.dwFlags|=KEYEVENTF_KEYUP;
        for(int Page=0;Page<20;++Page){QCOMPARE(SendInput(2,Events,sizeof(INPUT)),UINT(2));QTest::qWait(60);}
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["previewScroll"].toObject()["progress"].toInt()>0,5000);QTest::qWait(300);
        const auto View=Md->State()["previewScroll"].toObject();
        qInfo()<<"Markdown preview scroll: anchorBuilds"<<View["anchorBuilds"].toInteger()-Builds<<"alignments"<<View["alignments"].toInteger()<<"lastBuildMs"<<View["anchorMs"].toDouble();
        QVERIFY(View["anchorBuilds"].toInteger()-Builds<=1);
    }
    void editsKeepSyncBeforeRender()
    {
        // W9-4: with automatic preview off the preview keeps an older revision. Line edits are
        // mapped onto it, so the caret's text stays aligned until the next render.
        AppSettings Settings;Settings.setValue("markdown/autoPreview",false);Settings.sync();
        const auto Restore=qScopeGuard([]{AppSettings S;S.setValue("markdown/autoPreview",true);S.sync();});
        SyncFixture F;
        {
            F.input_.path_=F.temp_.filePath("edit.md");F.input_.encoding_="UTF-8";F.input_.eol_="LF";F.input_.text_="# Edit\n\n";
            for(int N=0;N<160;++N)F.input_.text_+=SyncProbe::tokenParagraph(N)+"\n\n";
            F.input_.bytes_=F.input_.text_.toUtf8();F.pane_=std::make_unique<DocumentViewPane>();F.pane_->resize(1300,850);
            QVERIFY(F.pane_->Load(F.input_,true,true));F.pane_->show();F.pane_->activateWindow();
            QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(F.pane_->winId())));QVERIFY(QTest::qWaitForWindowActive(F.pane_.get()));
            QVERIFY(F.pane_->RunViewCommand("markdown.split"));F.pane_->Focus();
        }
        SyncProbe Probe(*F.pane_);QVERIFY(Probe.valid());auto* Md=Probe.md_;auto* Backend=Probe.backend_;
        QTRY_COMPARE_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(),Md->State()["requestedRevision"].toInteger(),30000);
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger()>0,30000);
        Backend->Send(SCI_GOTOPOS,0);Backend->replace(QString(12,QChar('\n')));
        QVERIFY(Md->State()["revision"].toInteger()>Md->State()["requestedRevision"].toInteger());QVERIFY(Md->State()["lineShifts"].toInt()>0);
        const auto Text=Probe.text_->Content();const auto At=Text.indexOf("Paragraph 70 ");QVERIFY(At>0);
        Backend->Send(SCI_GOTOPOS,Text.left(At).toUtf8().size());Backend->Send(SCI_SCROLLCARET);
        QVERIFY(Probe.check("caret after an unrendered edit"));
        QTest::keyClick(Backend->widget(),Qt::Key_Down);QVERIFY(Probe.check("next row after an unrendered edit"));
    }
    void unchangedInputIsNotRenderedAgain()
    {
        // A render replaces the whole page and keeps the helper's GUI thread busy, and that thread
        // shares its input queue with the editor. Reloads with unchanged options and an edit undone
        // back to the rendered text send nothing; real edits and the explicit refresh still render.
        {AppSettings S;S.setValue("markdown/autoPreview",true);S.sync();}
        DocumentViewPane Pane;Pane.resize(1100,800);
        QVERIFY(Pane.Load(input(),true,true));Pane.show();QVERIFY(Pane.RunViewCommand("markdown.split"));
        auto* Md=Pane.findChild<MarkdownDocumentView*>();QVERIFY(Md);
        const auto Settled=[&]{return Md->State()["renderedRevision"].toInteger(-2)==Md->State()["revision"].toInteger(-1);};
        const auto Requested=[&]{return Md->State()["requestedRevision"].toInteger();};
        QTRY_VERIFY_WITH_TIMEOUT(Settled(),30000);
        const auto First=Requested();
        Md->ReloadPreferences();Md->Refresh();
        QCOMPARE(Requested(),First);QVERIFY(Settled());
        auto* Text=Pane.findChild<QTextView*>();QVERIFY(Text);
        auto* Backend=static_cast<ScintillaQtDirectBackend*>(Text->Backend());
        Backend->Send(SCI_DOCUMENTEND);Backend->replace("x");QVERIFY(!Settled());Backend->Send(SCI_UNDO);
        QTRY_VERIFY_WITH_TIMEOUT(Settled(),5000);
        QCOMPARE(Requested(),First);QCOMPARE(Md->State()["lineShifts"].toInt(),0);
        Backend->replace("y");
        QTRY_VERIFY_WITH_TIMEOUT(Requested()>First,5000);QTRY_VERIFY_WITH_TIMEOUT(Settled(),30000);
        const auto Second=Requested();
        Md->Refresh(true);QVERIFY(Requested()>Second);QTRY_VERIFY_WITH_TIMEOUT(Settled(),30000);
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
