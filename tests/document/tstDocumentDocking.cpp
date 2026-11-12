#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentToolPanel.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/markdownDocumentView.hpp"
#include "UniqueLibs/document/markdownProcessView.hpp"
#include "UniqueLibs/document/search/documentSearchPanel.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "documentTabsProbe.hpp"

#include <fmdock/DockManager.h>

#include <Windows.h>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

// HyperEditor 도킹 통합(P6): 문서는 중앙 영역의 도크, 파일에서 찾기 · 개요 · 책 구조 · 문제 · 출력은 도구 창.
using namespace hc::document;
using fm::dock::DropArea;
namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes)
    { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QJsonObject Document(const DocumentWorkspaceWidget& Widget, const QString& Id)
    { for (auto Item : Widget.Snapshot()["documents"].toArray()) if (Item.toObject()["documentId"].toString() == Id) return Item.toObject(); return {}; }
    bool Ready(const DocumentWorkspaceWidget& Widget, const QString& Id) { return Document(Widget, Id)["lifecycle"].toString() == "ready"; }
    QString Active(const DocumentWorkspaceWidget& Widget) { return Widget.Snapshot()["activeDocumentId"].toString(); }
    // Settings and recovery in a temporary product directory for each test.
    struct Profile
    {
        QTemporaryDir root_;
        QVariant previous_ = qApp->property("documentViewsConfigDir");
        Profile()
        {
            qApp->setProperty("documentViewsConfigDir", root_.path());
            Put(hc::settings::UnifiedConfig::FileName(root_.path()), R"({"schemaVersion":2,"documentViews":{}})");
        }
        ~Profile() { qApp->setProperty("documentViewsConfigDir", previous_); }
        QString Path(const QString& Name) const { return root_.filePath(Name); }
    };
    // Opens the menu a context-menu event shows over Target and returns its rows (text before the tab, enabled).
    // Run (optional) is the row to trigger before the menu closes.
    QList<QPair<QString, bool>> MenuRows(QWidget* Target, const QString& Run = {})
    {
        QList<QPair<QString, bool>> Rows;
        QTimer::singleShot(80, [&Rows, Run] {
            auto* Menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); if (!Menu) return;
            QAction* Chosen = nullptr;
            for (auto* Action : Menu->actions()) {
                if (Action->isSeparator()) continue;
                const auto Text = Action->text().section(QLatin1Char('\t'), 0, 0);
                Rows.append({Text, Action->isEnabled()});
                if (Text == Run) Chosen = Action;
            }
            Menu->close();
            if (Chosen) Chosen->trigger();
        });
        QContextMenuEvent Event(QContextMenuEvent::Mouse, Target->rect().center(), Target->mapToGlobal(Target->rect().center()));
        QApplication::sendEvent(Target, &Event);
        return Rows;
    }
    bool Has(const QList<QPair<QString, bool>>& Rows, const QString& Text, bool Enabled)
    { for (const auto& Row : Rows) if (Row.first == Text) return Row.second == Enabled; return false; }
}
class DocumentDockingTests final : public QObject
{
    Q_OBJECT
private slots:
    void documentsAreCentralDocks()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("a.txt"), "alpha\r\n")); QVERIFY(Put(Settings.Path("b.txt"), "beta\r\n"));
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        test::DocumentTabs Tabs(&Widget); QVERIFY(Tabs);
        // Without documents the start page fills the empty central area.
        auto* Start = Widget.findChild<QWidget*>("documentStartPage"); QVERIFY(Start); QTRY_VERIFY(Start->isVisible());
        // Tool windows exist from the start: output is pinned to the bottom side bar, the rest are closed.
        for (const auto* Kind : {"search", "outline", "bookTree", "problems", "output"}) QVERIFY2(Tabs.tool(QString::fromLatin1(Kind)), Kind);
        for (const auto* Kind : {"search", "outline", "bookTree", "problems"}) QVERIFY2(Tabs.tool(QString::fromLatin1(Kind))->isClosed(), Kind);
        QVERIFY(Tabs.tool("output")->isAutoHidden());
        const auto A = Widget.Handle({{"action", "open"}, {"path", Settings.Path("a.txt")}})["documentId"].toString();
        const auto B = Widget.Handle({{"action", "open"}, {"path", Settings.Path("b.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, A) && Ready(Widget, B));
        QCOMPARE(Tabs.count(), 2); QCOMPARE(Tabs.tabText(0), QString("a.txt")); QCOMPARE(Tabs.tabText(1), QString("b.txt"));
        QVERIFY(Tabs.dock(0)->isInCentralArea()); QCOMPARE(Tabs.dock(0)->dockArea(), Tabs.dock(1)->dockArea());
        QVERIFY(Tabs.dock(0)->isDocument()); QCOMPARE(Active(Widget), B); QVERIFY(!Start->isVisible());
        // An edit marks the tab like the old tab bar did.
        QVERIFY(!Widget.Handle({{"action", "replaceText"}, {"documentId", B}, {"text", "changed"}}).contains("error"));
        QTRY_VERIFY(Tabs.tabText(1).endsWith(" *"));
        // The tab close button goes through the save prompt.
        int Prompts = 0; Widget.SetClosePrompt([&](const QString&) { ++Prompts; return int(QMessageBox::Discard); });
        Tabs.requestClose(1); QTRY_COMPARE(Tabs.count(), 1); QCOMPARE(Prompts, 1); QTRY_COMPARE(Active(Widget), A);
        Tabs.requestClose(0); QTRY_COMPARE(Tabs.count(), 0); QTRY_VERIFY(Start->isVisible());
    }
    void splitMoveAndFocusFollow()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("a.txt"), "alpha\r\n")); QVERIFY(Put(Settings.Path("b.txt"), "beta\r\n"));
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget)); QApplication::setActiveWindow(&Widget);
        test::DocumentTabs Tabs(&Widget);
        const auto A = Widget.Handle({{"action", "open"}, {"path", Settings.Path("a.txt")}})["documentId"].toString();
        const auto B = Widget.Handle({{"action", "open"}, {"path", Settings.Path("b.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, A) && Ready(Widget, B));
        auto* Next = Widget.findChild<QAction*>("tab.next"); QVERIFY(Next); QTRY_VERIFY(Next->isEnabled());
        // Split: the two documents sit side by side in two central groups.
        QVERIFY(Tabs.manager()->moveDockWidget(Tabs.dock(1), DropArea::Right, Tabs.dock(0)->dockArea()));
        QTRY_COMPARE(Tabs.manager()->dockContainer()->centralAreas().size(), 2);
        QVERIFY(Tabs.dock(0)->isInCentralArea() && Tabs.dock(1)->isInCentralArea());
        // The focused document is the active one: status bar, menus and title follow it.
        qobject_cast<DocumentViewPane*>(Tabs.widget(0))->Focus(); QTRY_COMPARE(Active(Widget), A);
        QTRY_COMPARE(Widget.windowTitle(), QString("a.txt — HyperEditor"));
        qobject_cast<DocumentViewPane*>(Tabs.widget(1))->Focus(); QTRY_COMPARE(Active(Widget), B);
        // Each group holds one document now: next/previous tab cycles within the focused group only.
        QTRY_VERIFY(!Next->isEnabled());
        // A floating document window still activates through the workspace.
        Tabs.dock(0)->setFloating(); QTRY_VERIFY(Tabs.dock(0)->isFloating());
        Widget.Handle({{"action", "activate"}, {"documentId", A}}); QTRY_COMPARE(Active(Widget), A);
        Tabs.dock(0)->setFloating(); // already floating: no change
        QVERIFY(Tabs.manager()->moveDockWidget(Tabs.dock(0), DropArea::Center, Tabs.dock(1)->dockArea()));
        QTRY_VERIFY(!Tabs.dock(0)->isFloating()); QCOMPARE(Tabs.dock(0)->dockArea(), Tabs.dock(1)->dockArea());
        QTRY_VERIFY(Next->isEnabled());
    }
    void documentTabMenu()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("a.txt"), "alpha\r\n")); QVERIFY(Put(Settings.Path("b.txt"), "beta\r\n"));
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget)); QApplication::setActiveWindow(&Widget);
        test::DocumentTabs Tabs(&Widget);
        const auto A = Widget.Handle({{"action", "open"}, {"path", Settings.Path("a.txt")}})["documentId"].toString();
        const auto B = Widget.Handle({{"action", "open"}, {"path", Settings.Path("b.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, A) && Ready(Widget, B));
        QTRY_VERIFY(Tabs.tabButton(0));
        // The document tab menu replaces the dock's own menu: the old items plus split · move · detach.
        const auto Rows = MenuRows(Tabs.tabButton(0));
        QVERIFY2(Has(Rows, QString::fromUtf8("오른쪽으로 분할"), true), "split right");
        QVERIFY(Has(Rows, QString::fromUtf8("아래로 분할"), true)); QVERIFY(Has(Rows, QString::fromUtf8("새 창으로 떼어 내기"), true));
        QVERIFY(Has(Rows, QString::fromUtf8("다른 묶음으로 옮기기"), false)); QVERIFY(Has(Rows, QString::fromUtf8("다른 탭 닫기"), true));
        QVERIFY(Has(Rows, QString::fromUtf8("경로 복사"), true)); QVERIFY(Has(Rows, QString::fromUtf8("다시 읽기"), true));
        QCOMPARE(Active(Widget), A);
        // Running "오른쪽으로 분할" from the menu splits like a drop at the right of the group.
        MenuRows(Tabs.tabButton(0), QString::fromUtf8("오른쪽으로 분할"));
        QTRY_COMPARE(Tabs.manager()->dockContainer()->centralAreas().size(), 2);
        QVERIFY(Tabs.dock(0)->dockArea() != Tabs.dock(1)->dockArea());
        // Now each group holds one document: splitting is off, moving to the other group is on.
        const auto After = MenuRows(Tabs.tabButton(0));
        QVERIFY(Has(After, QString::fromUtf8("오른쪽으로 분할"), false)); QVERIFY(Has(After, QString::fromUtf8("다른 묶음으로 옮기기"), true));
    }
    void outlineToolWindowFollowsDocument()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("guide.md"), "# 사용 설명서\n\n## 설치\n\n## 도킹 배치\n")); QVERIFY(Put(Settings.Path("notes.txt"), "plain\r\n"));
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        test::DocumentTabs Tabs(&Widget);
        const auto Guide = Widget.Handle({{"action", "open"}, {"path", Settings.Path("guide.md")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, Guide));
        auto* Host = Widget.findChild<DocumentToolHost*>("documentTool.outline"); QVERIFY(Host);
        auto* View = Widget.findChild<MarkdownDocumentView*>(); QVERIFY(View);
        // The view's outline column moved into the 개요 tool window and follows the focused document.
        QTRY_VERIFY(Host->CurrentPanel() && Host->CurrentPanel()->findChild<QLineEdit*>("markdownOutlineFilter"));
        // The view's own 개요 button (its toolbar sits in the pane's toolbar row); the window has a command of the same name.
        QAction* Toggle = nullptr;
        for (auto* Action : Widget.findChildren<QAction*>("markdown.toggleToc")) if (Action->isCheckable() && qobject_cast<QToolBar*>(Action->parent())) Toggle = Action;
        QVERIFY(Toggle);
        auto* Outline = Tabs.tool("outline");
        if (Toggle->isChecked()) { QTRY_VERIFY(!Outline->isClosed()); Toggle->trigger(); }
        QTRY_VERIFY(Outline->isClosed());
        // The view's 개요 button opens and closes the tool window …
        Toggle->trigger(); QTRY_VERIFY(!Outline->isClosed()); QVERIFY(Toggle->isChecked());
        // … and closing the tool window turns the button off.
        Outline->toggleView(false); QTRY_VERIFY(!Toggle->isChecked());
        Outline->toggleView(true); QTRY_VERIFY(Toggle->isChecked());
        // A document without an outline shows the empty note; the tool window stays where it is.
        const auto Notes = Widget.Handle({{"action", "open"}, {"path", Settings.Path("notes.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, Notes)); QTRY_COMPARE(Active(Widget), Notes);
        QTRY_VERIFY(!Host->CurrentPanel()); QVERIFY(!Outline->isClosed());
        Widget.Handle({{"action", "activate"}, {"documentId", Guide}}); QTRY_VERIFY(Host->CurrentPanel());
        // Closing the Markdown document removes its panel from the host.
        QPointer<QWidget> Panel = Host->CurrentPanel();
        Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        Tabs.requestClose(0); QTRY_COMPARE(Tabs.count(), 1);
        QTRY_VERIFY(!Panel); QVERIFY(!Host->CurrentPanel());
    }
    void previewBesideRoundTrip()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("guide.md"), "# 제목\n\n본문\n"));
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        test::DocumentTabs Tabs(&Widget);
        const auto Guide = Widget.Handle({{"action", "open"}, {"path", Settings.Path("guide.md")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, Guide));
        auto* Beside = Widget.findChild<QAction*>("view.previewBeside"); QVERIFY(Beside); QTRY_VERIFY(Beside->isEnabled());
        auto* Preview = Widget.findChild<MarkdownProcessView*>(); QVERIFY(Preview);
        auto* Pane = qobject_cast<DocumentViewPane*>(Tabs.widget(0)); QVERIFY(Pane && Pane->isAncestorOf(Preview));
        Beside->trigger();
        auto* Dock = Tabs.manager()->findDockWidget(QStringLiteral("preview:") + Guide); QVERIFY(Dock);
        // The preview column now lives in its own document tab beside the source group; the source keeps the focus.
        QVERIFY(Dock->isAncestorOf(Preview)); QVERIFY(!Pane->isAncestorOf(Preview));
        QVERIFY(Dock->isInCentralArea()); QVERIFY(Dock->dockArea() != Tabs.dock(0)->dockArea());
        QTRY_COMPARE(Dock->title(), QString::fromUtf8("guide.md 미리보기"));
        QCOMPARE(Active(Widget), Guide); QCOMPARE(Tabs.count(), 1);
        auto* View = Widget.findChild<MarkdownDocumentView*>(); QVERIFY(View);
        QVERIFY(!View->CanRun("markdown.preview")); QVERIFY(!View->CanRun("markdown.source"));
        // Focusing the preview keeps its source the active document.
        Dock->bringToFront(); QApplication::processEvents(); QCOMPARE(Active(Widget), Guide);
        // Closing the preview tab returns the column to the view.
        Dock->requestClose(); QTRY_VERIFY(Pane->isAncestorOf(Preview));
        QTRY_VERIFY(!Tabs.manager()->findDockWidget(QStringLiteral("preview:") + Guide) || Tabs.manager()->findDockWidget(QStringLiteral("preview:") + Guide)->isClosed());
        QVERIFY(View->CanRun("markdown.preview"));
        // Open it again and close the document: both tabs go.
        Beside->trigger(); QVERIFY(Tabs.manager()->findDockWidget(QStringLiteral("preview:") + Guide));
        Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        Tabs.requestClose(0); QTRY_COMPARE(Tabs.count(), 0);
        QTRY_VERIFY(!Tabs.manager()->findDockWidget(QStringLiteral("preview:") + Guide));
    }
    void nativePreviewSurvivesMoves()
    {
        // The preview is another process's window under a native host widget (SetParent). Moving its dock — another
        // group, a floating window, back, and ‘옆에 미리보기’ — reparents ancestors only: the helper window must stay
        // a child of the same host (a recreated host would destroy it; EPUB and HWP views close on WinIdChange).
        if (!QFile::exists(QCoreApplication::applicationDirPath() + "/hc-markdown-render.exe")) QSKIP("Markdown preview helper not deployed");
        Profile Settings; QVERIFY(Put(Settings.Path("guide.md"), "# 제목\n\n본문\n")); QVERIFY(Put(Settings.Path("b.txt"), "beta\r\n"));
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        test::DocumentTabs Tabs(&Widget);
        const auto Guide = Widget.Handle({{"action", "open"}, {"path", Settings.Path("guide.md")}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Guide));
        auto* Preview = Widget.findChild<MarkdownProcessView*>(); QVERIFY(Preview);
        // Show source and preview side by side so the helper starts and attaches whatever the saved mode was.
        auto* View = Widget.findChild<MarkdownDocumentView*>(); QVERIFY(View); View->Run("markdown.split");
        const auto Host = reinterpret_cast<HWND>(Preview->winId());
        HWND Child = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((Child = GetWindow(Host, GW_CHILD)) != nullptr, 30000);
        DWORD Pid = 0; GetWindowThreadProcessId(Child, &Pid); QVERIFY(Pid != GetCurrentProcessId());
        const auto Attached = [&](QWidget* Root) { return IsWindow(Child) && GetParent(Child) == Host && reinterpret_cast<HWND>(Preview->winId()) == Host
            && GetAncestor(Child, GA_ROOT) == reinterpret_cast<HWND>(Root->winId()); };
        QVERIFY(Attached(&Widget));
        const auto B = Widget.Handle({{"action", "open"}, {"path", Settings.Path("b.txt")}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, B));
        // Another group.
        QVERIFY(Tabs.manager()->moveDockWidget(Tabs.dock(0), DropArea::Right, Tabs.dock(1)->dockArea())); QApplication::processEvents();
        QTRY_VERIFY(Attached(&Widget));
        // A floating window and back.
        Tabs.dock(0)->setFloating(); QTRY_VERIFY(Tabs.dock(0)->isFloating());
        QTRY_VERIFY(Attached(Tabs.dock(0)->window()));
        QVERIFY(Tabs.manager()->moveDockWidget(Tabs.dock(0), DropArea::Center, Tabs.dock(1)->dockArea()));
        QTRY_VERIFY(!Tabs.dock(0)->isFloating()); QTRY_VERIFY(Attached(&Widget));
        // The preview column in its own tab beside the source, and back.
        Widget.Handle({{"action", "activate"}, {"documentId", Guide}}); QTRY_COMPARE(Active(Widget), Guide);
        auto* Beside = Widget.findChild<QAction*>("view.previewBeside"); QTRY_VERIFY(Beside->isEnabled()); Beside->trigger();
        auto* Dock = Tabs.manager()->findDockWidget(QStringLiteral("preview:") + Guide); QVERIFY(Dock);
        QTRY_VERIFY(Attached(&Widget)); QVERIFY(Dock->isAncestorOf(Preview));
        Dock->requestClose(); QTRY_VERIFY(Tabs.dock(0)->isAncestorOf(Preview)); QTRY_VERIFY(Attached(&Widget));
        Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
    }
    void layoutPersistsAndPerspectives()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("a.txt"), "alpha\r\n"));
        QJsonObject Stored;
        {
            DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
            QVERIFY(QTest::qWaitForWindowExposed(&Widget));
            test::DocumentTabs Tabs(&Widget);
            const auto A = Widget.Handle({{"action", "open"}, {"path", Settings.Path("a.txt")}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, A));
            // The user moves 문제 to the left edge and opens it.
            auto* Problems = Tabs.tool("problems"); Problems->toggleView(true);
            QVERIFY(Tabs.manager()->moveDockWidget(Problems, DropArea::Left)); QTRY_VERIFY(!Problems->isClosed());
            // Every way of closing the window passes RequestClose, which stores the layout (HotExit may finish later).
            QSignalSpy Saved(&Widget, &DocumentWorkspaceWidget::ChromeSettingsChanged);
            Widget.RequestClose(); QTRY_VERIFY(!Saved.isEmpty());
            Stored = Saved.last().first().toJsonObject();
        }
        QVERIFY(!Stored["dockLayout"].toString().isEmpty());
        // A new window restores the tool windows' places (documents are not part of the layout).
        Stored["dockPerspectives"] = QJsonObject{{"검토용", Stored["dockLayout"]}};
        Stored["dockLocked"] = true;
        DocumentWorkspaceWidget Next(Settings.Path("recovery2")); Next.resize(1200, 800);
        QVERIFY(Next.ApplyChromeSettings(Stored)); Next.show(); QVERIFY(QTest::qWaitForWindowExposed(&Next));
        test::DocumentTabs Tabs(&Next);
        auto* Problems = Tabs.tool("problems"); QVERIFY(Problems); QTRY_VERIFY(!Problems->isClosed());
        QVERIFY(Problems->dockArea()); QVERIFY(!Problems->dockArea()->isCentral());
        auto* Placeholder = Tabs.manager()->dockContainer()->centralPlaceholder(); QVERIFY(Placeholder);
        QVERIFY(Problems->dockArea()->mapTo(&Next, QPoint()).x() < Placeholder->mapTo(&Next, QPoint()).x());
        QVERIFY(Tabs.manager()->perspectiveNames().contains(QString::fromUtf8("검토용")));
        QVERIFY(Tabs.manager()->lockedFeatures() != fm::dock::DockWidget::NoFeatures);
        // The layout button names the layout in use.
        auto* Button = Next.findChild<QToolButton*>("documentLayoutButton"); QVERIFY(Button); QVERIFY(Button->text().startsWith(QString::fromUtf8("창 배치: ")));
    }
    void searchToolWindowPlacement()
    {
        // 파일에서 찾기 = 도구 창 "tool:search". 위치(분리 창 · 콘텐츠 탭 · 하단 패널)는 그 자리로 옮기고,
        // 닫았다 다시 열면 마지막 자리에서 열린다.
        Profile Settings; QVERIFY(Put(Settings.Path("a.txt"), "hello world\r\n"));
        {
        DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1200, 800); Widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        test::DocumentTabs Tabs(&Widget);
        const auto A = Widget.Handle({{"action", "open"}, {"path", Settings.Path("a.txt")}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, A));
        auto* Find = Widget.findChild<QAction*>("document.findInFiles"); QVERIFY(Find); Find->trigger();
        auto* Panel = Widget.findChild<search::Panel*>(); QVERIFY(Panel); QTRY_VERIFY(Panel->isVisible());
        auto* Dock = Tabs.tool("search"); QVERIFY(Dock); QVERIFY(Dock->isAncestorOf(Panel)); QVERIFY(!Dock->isClosed());
        emit Panel->PlacementRequested("floating"); QTRY_VERIFY(Dock->isFloating()); QVERIFY(Panel->isVisible());
        emit Panel->PlacementRequested("tab"); QTRY_VERIFY(!Dock->isFloating()); QVERIFY(Dock->isInCentralArea()); QVERIFY(Dock->isCurrentTab());
        QCOMPARE(Dock->dockArea(), Tabs.dock(0)->dockArea());
        // The document stays the active one while the search tab is in front of it.
        QCOMPARE(Active(Widget), A);
        emit Panel->PlacementRequested("bottom"); QTRY_VERIFY(!Dock->isInCentralArea()); QVERIFY(!Dock->isFloating()); QVERIFY(Panel->isVisible());
        // Closing hides it; Ctrl+Shift+F reopens it where it was.
        emit Panel->CloseRequested(); QTRY_VERIFY(Dock->isClosed());
        Find->trigger(); QTRY_VERIFY(!Dock->isClosed()); QVERIFY(!Dock->isInCentralArea()); QTRY_VERIFY(Panel->isVisible());
        // 보기 › 도구 창 shows it checked.
        auto* Menu = Widget.findChild<QMenu*>("documentToolMenu"); QVERIFY(Menu); emit Menu->aboutToShow();
        auto* Item = Menu->findChild<QAction*>("view.tool.search"); QVERIFY(Item); QVERIFY(Item->isChecked());
        Item->trigger(); QTRY_VERIFY(Dock->isClosed());
        }
        // The panel's workers stop with the window.
        QTRY_VERIFY_WITH_TIMEOUT(search::Panel::AllStopped(), 10000);
    }
    void hostedHasNoFloating()
    {
        Profile Settings; QVERIFY(Put(Settings.Path("a.txt"), "alpha\r\n"));
        DocumentWorkspaceWidget Viewer(Settings.Path("recovery"), true); Viewer.resize(900, 600); Viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Viewer));
        test::DocumentTabs Tabs(&Viewer);
        QVERIFY(Tabs.manager()->testConfigFlag(fm::dock::DockManager::NoTopLevelWindows));
        // Narrow host: tool windows start in the side bars; only 개요 and 파일에서 찾기 show a tab.
        QVERIFY(Tabs.tool("outline")->isAutoHidden() && !Tabs.tool("outline")->isClosed());
        QVERIFY(Tabs.tool("search")->isAutoHidden() && !Tabs.tool("search")->isClosed());
        QVERIFY(Tabs.tool("problems")->isClosed());
        const auto A = Viewer.Handle({{"action", "open"}, {"path", Settings.Path("a.txt")}})["documentId"].toString(); QTRY_VERIFY(Ready(Viewer, A));
        Tabs.dock(0)->setFloating(); QApplication::processEvents(); QVERIFY(!Tabs.dock(0)->isFloating());
        QVERIFY(!Viewer.findChild<QToolButton*>("documentLayoutButton"));
    }
};
QTEST_MAIN(DocumentDockingTests)
#include "tstDocumentDocking.moc"
