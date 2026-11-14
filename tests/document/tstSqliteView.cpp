#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "UniqueLibs/document/documentStorageAdapter.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/sqlite/sqliteConnection.hpp"
#include "UniqueLibs/document/sqlite/sqliteDocumentView.hpp"
#include "UniqueLibs/document/sqlite/sqliteGrid.hpp"
#include "UniqueLibs/document/sqlite/sqliteQueryTab.hpp"
#include "UniqueLibs/document/sqlite/sqliteResultModel.hpp"
#include "text/ScintillaQtDirectBackend.h"

#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <Scintilla.h>

// SQLite 문서가 문서 창에 붙는 길(설계 261113 3 · 4절): 파일 분류, F4(HyperEditor) 편집기, F3(HyperViewer) 보기 창,
// Ctrl+Q 빠른 보기(hc.quickPreview, 열람만). 저장 · 인쇄 명령은 SQLite 에 쓰지 않는다. 문서를 닫으면 연결이 모두 닫혀
// 파일을 곧바로 옮길 수 있어야 한다(탐색기는 열린 문서를 닫고 이름을 바꾼다, 설계 4.6).
using namespace hc::document;

namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes)
    {
        QFile File(Path);
        return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
    }

    bool MakeDatabase(const QString& Path)
    {
        hc::sqlite::OpenOptions Options;
        Options.create_ = true;
        QString Error;
        auto Db = hc::sqlite::Connection::Open(Path, Options, &Error);
        return Db && Db->Exec(QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT);"
                                              "INSERT INTO notes(body) VALUES('first'), ('second'), (NULL);"), &Error);
    }

    QJsonObject Doc(const QJsonObject& Snapshot, const QString& Id)
    {
        for (const auto& Value : Snapshot["documents"].toArray())
            if (Value.toObject()["documentId"].toString() == Id) return Value.toObject();
        return {};
    }

    // 파일을 옆 이름으로 옮겼는가. QTRY_VERIFY 는 성공한 뒤 식을 한 번 더 평가하므로 여러 번 불러도 같은 답을 내야 한다.
    bool Moved(const QString& Path)
    {
        const auto Target = Path + QStringLiteral(".moved");
        return QFile::exists(Target) || QFile::rename(Path, Target);
    }
}

class SqliteViewTests final : public QObject
{
    Q_OBJECT
private slots:
    void classifiesDatabaseFiles()
    {
        QTemporaryDir Root;
        QVERIFY(MakeDatabase(Root.filePath(QStringLiteral("music.db"))));
        QVERIFY(MakeDatabase(Root.filePath(QStringLiteral("History"))));
        QVERIFY(Put(Root.filePath(QStringLiteral("Thumbs.db")), QByteArray::fromHex("D0CF11E0A1B11AE1") + QByteArray(504, '\0')));
        QVERIFY(Put(Root.filePath(QStringLiteral("notes.txt")), QByteArray("SQLite format 3", 16) + QByteArray("text after the header\n")));
        QVERIFY(Put(Root.filePath(QStringLiteral("empty.sqlite")), {}));

        const auto Db = ReadDocumentFile(Root.filePath(QStringLiteral("music.db")));
        QVERIFY(Db.HasValue());
        QCOMPARE(Db.Value().viewId_, QStringLiteral("sqlite"));
        QVERIFY(Db.Value().bytes_.isEmpty());
        QCOMPARE(Db.Value().hash_.size(), 32);
        // 확장자가 없는 파일은 머리가 평문 SQLite 면 SQLite(브라우저 History 같은 파일).
        const auto History = ReadDocumentFile(Root.filePath(QStringLiteral("History")));
        QVERIFY(History.HasValue());
        QCOMPARE(History.Value().viewId_, QStringLiteral("sqlite"));
        const auto Empty = ReadDocumentFile(Root.filePath(QStringLiteral("empty.sqlite")));
        QVERIFY(Empty.HasValue());
        QCOMPARE(Empty.Value().viewId_, QStringLiteral("sqlite"));
        // .db 이지만 SQLite 가 아니면 텍스트로 넘기지 않고 실패한다.
        const auto Thumbs = ReadDocumentFile(Root.filePath(QStringLiteral("Thumbs.db")));
        QVERIFY(!Thumbs.HasValue());
        QCOMPARE(QString::fromStdString(Thumbs.Failure().code_), QStringLiteral("document-sqlite-not-database"));
        // 아는 문서 확장자는 머리가 SQLite 여도 SQLite 로 열지 않는다(텍스트 판정은 텍스트 규칙을 따른다).
        const auto Text = ReadDocumentFile(Root.filePath(QStringLiteral("notes.txt")));
        QVERIFY(!Text.HasValue() || Text.Value().viewId_ != QStringLiteral("sqlite"));
    }

    void viewReleasesFileWhenClosed()
    {
        // 작업 창이 닫히면 GUI 연결 · 행 수 세기 · keyset 작업 스레드 연결이 모두 닫힌다. 2026-11-14: 행 수 세기를 멈출 때
        // 잠긴 뮤텍스를 가진 작업 객체를 놓아 작업 스레드가 멈추고 그 연결이 남던 결함을 이 시험으로 잡았다.
        QTemporaryDir Root;
        const auto Path = Root.filePath(QStringLiteral("direct.db"));
        QVERIFY(MakeDatabase(Path));
        {
            hc::sqlite::DocumentView View(Path, hc::sqlite::DocumentView::Mode::Editor);
            View.Open();
            QVERIFY(View.IsReady());
            View.OpenObject(QStringLiteral("main"), QStringLiteral("notes"));
            QTRY_COMPARE(View.CurrentModel()->rowCount(), 3);
        }
        QVERIFY(Moved(Path));
    }

    void editorOpensWorkWindow()
    {
        QTemporaryDir Root;
        const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Root.path());
        const auto Restore = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); });
        const auto Path = Root.filePath(QStringLiteral("music.db"));
        QVERIFY(MakeDatabase(Path));
        {
        DocumentWorkspaceWidget Window(Root.filePath(QStringLiteral("recovery")), false);
        Window.resize(1280, 800);
        Window.show();
        const auto Id = Window.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
        QVERIFY(!Id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(Doc(Window.Snapshot(), Id)["lifecycle"].toString(), QStringLiteral("ready"), 20000);
        auto* View = Window.findChild<hc::sqlite::DocumentView*>();
        QVERIFY(View);
        QTRY_VERIFY(View->IsReady());
        QCOMPARE(int(View->ViewMode()), int(hc::sqlite::DocumentView::Mode::Editor));
        QVERIFY(!View->IsReadOnly());
        auto* Pane = Window.findChild<DocumentViewPane*>();
        QVERIFY(Pane);
        QCOMPARE(int(Pane->Kind()), int(DocumentViewKind::Sqlite));
        QVERIFY(Pane->ViewState()["ready"].toBool());
        bool Database = false, Mode = false;
        for (const auto& Field : Pane->StatusFields()) Database |= Field.id_ == QStringLiteral("sqliteDatabase") && Field.text_.contains(QStringLiteral("music.db"));
        QVERIFY(Database);
        // 저장 · 다른 이름으로 저장 · 인쇄는 SQLite 에 쓰지 않는다.
        QTRY_VERIFY(!Window.findChild<QAction*>(QStringLiteral("saveDocument"))->isEnabled());
        QVERIFY(!Window.findChild<QAction*>(QStringLiteral("saveDocumentAs"))->isEnabled());
        View->OpenObject(QStringLiteral("main"), QStringLiteral("notes"));
        QCOMPARE(View->OpenTables(), QStringList{QStringLiteral("notes")});
        QTRY_COMPARE(View->CurrentModel()->rowCount(), 3);
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { QTest::qWait(300); Window.grab().save(Shot + QStringLiteral("-window.png")); }
        // 작업 공간의 저장 상태(‘편집 · 사본으로 저장’)는 SQLite 에 붙이지 않는다.
        for (const auto& Field : Pane->StatusFields()) Mode |= Field.id_ == QStringLiteral("mode");
        QVERIFY(!Mode);
        QVERIFY(!Window.Handle({{"action", "close"}, {"documentId", Id}, {"discard", true}}).contains("error"));
        QTRY_VERIFY(Window.Snapshot()["documents"].toArray().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(Moved(Path));
        }
        // 창이 닫히며 복구 색인을 쓴다. 다 쓰고 나서 임시 폴더를 지운다.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(Root.remove());
    }

    void shortcutsReachWorkWindow()
    {
        // 문서 창 단축키 라우터가 작업 창 범위(sqlite · sqlite.query · sqlite.schema)로 명령을 보낸다. 커밋 · 중지 · 다시 읽기는
        // 공통 명령(Ctrl+S · Ctrl+. · Ctrl+R)이 SQLite 에 맞게 동작한다(설계 4.5).
        QTemporaryDir Root;
        const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Root.path());
        const auto Restore = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); });
        const auto Path = Root.filePath(QStringLiteral("keys.db"));
        QVERIFY(MakeDatabase(Path));
        QVERIFY(DocumentShortcutRegistry::Issues({}).isEmpty());
        {
        DocumentWorkspaceWidget Window(Root.filePath(QStringLiteral("recovery")), false);
        Window.resize(1280, 800);
        Window.show();
        Window.activateWindow();
        const auto Id = Window.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
        QTRY_COMPARE_WITH_TIMEOUT(Doc(Window.Snapshot(), Id)["lifecycle"].toString(), QStringLiteral("ready"), 20000);
        auto* View = Window.findChild<hc::sqlite::DocumentView*>();
        QVERIFY(View);
        QTRY_VERIFY(View->IsReady());
        const auto Press = [](QWidget* Target, Qt::Key Key, Qt::KeyboardModifiers Modifiers = Qt::NoModifier) {
            Target->setFocus(Qt::OtherFocusReason);
            QCoreApplication::processEvents();
            QTest::keyClick(Target, Key, Modifiers);
        };

        // 스키마 트리에서 Ctrl+T: 새 쿼리 탭.
        auto* Tree = View->findChild<QTreeWidget*>(QStringLiteral("sqliteSchemaTree"));
        QVERIFY(Tree);
        Press(Tree, Qt::Key_T, Qt::ControlModifier);
        QTRY_COMPARE(View->TabCount(), 1);
        auto* Query = qobject_cast<hc::sqlite::QueryTab*>(View->CurrentTab());
        QVERIFY(Query);
        auto* Editor = Query->FocusTarget();

        // 쿼리 편집기에서 F5(전체) · Ctrl+Enter(현재 문).
        Query->SetText(QStringLiteral("SELECT 42 AS answer;\nSELECT 7 AS other;"));
        Press(Editor, Qt::Key_F5);
        QTRY_VERIFY(Query->CurrentGrid() && !Query->Running());
        auto* ResultTabs = Query->Result()->findChild<QTabBar*>();
        QCOMPARE(ResultTabs->count(), 3);
        Query->Editor()->Send(SCI_GOTOPOS, 0);
        Press(Editor, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!Query->Running() && ResultTabs->count() == 2);
        QCOMPARE(Query->CurrentGrid()->model()->index(0, 0).data(hc::sqlite::ValueRole).toInt(), 42);

        // Ctrl+/: 주석 전환.
        Query->Editor()->Send(SCI_GOTOPOS, 0);
        Press(Editor, Qt::Key_Slash, Qt::ControlModifier);
        QVERIFY(Query->Text().startsWith(QStringLiteral("-- SELECT 42")));

        // Ctrl+R: 문서를 다시 열지 않고 스키마만 다시 읽는다(쿼리 탭은 그대로).
        {
            hc::sqlite::OpenOptions Options;
            QString Error;
            auto Other = hc::sqlite::Connection::Open(Path, Options, &Error);
            QVERIFY(Other && Other->Exec(QStringLiteral("CREATE TABLE added(a);"), &Error));
        }
        Press(Editor, Qt::Key_R, Qt::ControlModifier);
        QTRY_VERIFY(View->Schema().Find(QStringLiteral("main"), QStringLiteral("added")));
        QCOMPARE(Window.findChild<hc::sqlite::DocumentView*>(), View);
        QCOMPARE(View->TabCount(), 1);

        // Ctrl+.: 끝나지 않는 문을 멈춘다.
        Query->SetText(QStringLiteral("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n) SELECT count(*) FROM n;"));
        Press(Editor, Qt::Key_F5);
        QTRY_VERIFY(Query->Running());
        QTRY_VERIFY(Window.findChild<QAction*>(QStringLiteral("cancelDocument"))->isEnabled());
        QTest::qWait(100);
        Press(Editor, Qt::Key_Period, Qt::ControlModifier);
        QTRY_VERIFY(!Query->Running());

        // 스키마 트리에서 Ctrl+Enter: 새 쿼리 탭에 SELECT.
        for (QTreeWidgetItemIterator It(Tree); *It; ++It)
            if ((*It)->data(0, Qt::UserRole + 5).toString() == QStringLiteral("notes")) { Tree->setCurrentItem(*It); break; }
        Press(Tree, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(View->TabCount(), 2);
        auto* Selected = qobject_cast<hc::sqlite::QueryTab*>(View->CurrentTab());
        QVERIFY(Selected && Selected != Query);
        QTRY_VERIFY(Selected->CurrentGrid() && !Selected->Running());
        QCOMPARE(Selected->CurrentGrid()->model()->rowCount(), 3);

        // Ctrl+F4: 쿼리 탭 닫기(저장하지 않은 이름 없는 쿼리는 묻지 않는다).
        Press(Selected->FocusTarget(), Qt::Key_F4, Qt::ControlModifier);
        QTRY_COMPARE(View->TabCount(), 1);

        QVERIFY(!Window.Handle({{"action", "close"}, {"documentId", Id}, {"discard", true}}).contains("error"));
        QTRY_VERIFY(Window.Snapshot()["documents"].toArray().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(Moved(Path));
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(Root.remove());
    }

    void hostedViewerAndQuickPreview()
    {
        QTemporaryDir Root;
        const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Root.path());
        const auto Restore = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); });
        for (const bool Quick : {false, true}) {
            const auto Path = Root.filePath(Quick ? QStringLiteral("quick.db") : QStringLiteral("viewer.db"));
            QVERIFY(MakeDatabase(Path));
            DocumentWorkspaceWidget Viewer(Root.filePath(Quick ? QStringLiteral("quick") : QStringLiteral("viewer")), true);
            if (Quick) Viewer.setProperty("hc.quickPreview", true);
            Viewer.resize(900, 600);
            Viewer.show();
            const auto Id = Viewer.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
            QVERIFY(!Id.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(Doc(Viewer.Snapshot(), Id)["lifecycle"].toString(), QStringLiteral("ready"), 20000);
            auto* View = Viewer.findChild<hc::sqlite::DocumentView*>();
            QVERIFY(View);
            QTRY_VERIFY(View->IsReady());
            QCOMPARE(int(View->ViewMode()), int(Quick ? hc::sqlite::DocumentView::Mode::QuickPreview : hc::sqlite::DocumentView::Mode::Viewer));
            QCOMPARE(View->IsReadOnly(), Quick);
            QVERIFY(!Viewer.findChild<DocumentViewPane*>()->IsBusy());
            QVERIFY(!Viewer.Handle({{"action", "close"}, {"documentId", Id}, {"discard", true}}).contains("error"));
            QTRY_VERIFY(Viewer.Snapshot()["documents"].toArray().isEmpty());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QTRY_VERIFY(Moved(Path));
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(Root.remove());
    }
};

QTEST_MAIN(SqliteViewTests)
#include "tstSqliteView.moc"
