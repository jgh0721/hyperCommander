// Menus, toolbar rows, status pickers, recent items and progress of the document workspace
// (plan 261104: the prototype is the specification).
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/documentStatusBar.hpp"
#include "UniqueLibs/document/documentStatusPopup.hpp"
#include "UniqueLibs/document/documentProgressOverlay.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include <QClipboard>
#include <QImage>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QCheckBox>

using namespace hc::document;

namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    bool Ready(const DocumentWorkspaceWidget& Workspace, const QString& Id)
    {
        for (const auto& Item : Workspace.Snapshot()["documents"].toArray())
            if (Item.toObject()["documentId"].toString() == Id) return Item.toObject()["lifecycle"].toString() == "ready";
        return false;
    }
    QMenu* TopMenu(QWidget& Workspace, const QString& Title)
    {
        auto* Bar = Workspace.findChild<QMenuBar*>("mainMenu");
        for (auto* Action : Bar ? Bar->actions() : QList<QAction*>{}) if (Action->menu() && Action->text() == Title) return Action->menu();
        return nullptr;
    }
    QMenu* SubMenu(QMenu* Menu, const QString& Title)
    {
        for (auto* Action : Menu ? Menu->actions() : QList<QAction*>{}) if (Action->menu() && Action->text() == Title) return Action->menu();
        return nullptr;
    }
    QString FieldText(DocumentStatusBar* Bar, const QString& Id)
    {
        for (const auto& Field : Bar->Fields()) if (Field.id_ == Id) return Field.text_;
        return {};
    }
    bool IsPicker(DocumentStatusBar* Bar, const QString& Id)
    {
        for (const auto& Field : Bar->Fields()) if (Field.id_ == Id) return Field.picker_;
        return false;
    }
}

class DocumentChromeTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir root_;
    QString open(DocumentWorkspaceWidget& Workspace, const QString& Path)
    {
        const auto Id = Workspace.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
        if (!QTest::qWaitFor([&] { return Ready(Workspace, Id); }, 15000)) return {};
        QCoreApplication::processEvents();
        return Id;
    }
private slots:
    void contextMenuIsNotTheActionList()
    {
        DocumentWorkspaceWidget Workspace(root_.filePath("recovery-a"));
        // Shortcut actions are window actions; the workspace must not list them as a context menu.
        QVERIFY(Workspace.contextMenuPolicy() != Qt::ActionsContextMenu);
    }
    void textModeMenuAndPickers()
    {
        const auto Path = root_.filePath("sample.cpp");
        QVERIFY(Put(Path, QByteArray::fromHex("efbbbf") + "int main()\r\n{\r\n    return 0;\r\n}\r\n"));
        DocumentWorkspaceWidget Workspace(root_.filePath("recovery-b")); Workspace.resize(1100, 700); Workspace.show();
        Workspace.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        QVERIFY(!open(Workspace, Path).isEmpty());
        auto* Mode = TopMenu(Workspace, QStringLiteral("텍스트(&X)"));
        QVERIFY2(Mode, "텍스트(X) mode menu");
        for (const auto* Title : {"언어", "인코딩", "줄 끝", "들여쓰기"}) QVERIFY2(SubMenu(Mode, QString::fromUtf8(Title)), Title);
        // Encoding: reinterpretation and conversion are separate headed groups.
        auto* Encoding = SubMenu(Mode, QStringLiteral("인코딩"));
        emit Encoding->aboutToShow();
        QStringList Headings; for (auto* Action : Encoding->actions()) if (!Action->isEnabled() && !Action->isSeparator()) Headings << Action->text();
        QVERIFY(Headings.join('|').contains(QStringLiteral("다시 읽기")));
        QVERIFY(Headings.join('|').contains(QStringLiteral("변환하여 저장")));
        // Status fields: values are pickers, the right-hand group starts at 언어.
        auto* Bar = static_cast<DocumentStatusBar*>(Workspace.findChild<QWidget*>("documentStatusBar")); QVERIFY(Bar);
        QTRY_VERIFY(!FieldText(Bar, "encoding").isEmpty());
        for (const auto* Id : {"position", "language", "encoding", "eol", "indent"}) QVERIFY2(IsPicker(Bar, Id), Id);
        QVERIFY(FieldText(Bar, "encoding").contains("BOM"));
        QCOMPARE(FieldText(Bar, "eol"), QStringLiteral("CRLF"));
        // A picker opens its popover above the status bar.
        auto* Button = Bar->findChild<QToolButton*>("status.encoding"); QVERIFY(Button);
        Button->click();
        DocumentStatusPopup* Popup = nullptr;
        QTRY_VERIFY((Popup = Button->findChild<DocumentStatusPopup*>()) != nullptr);
        QVERIFY(Popup->Title().startsWith(QStringLiteral("인코딩")));
        Popup->close();
        // Items of other modes stay out of the text menus.
        auto* File = TopMenu(Workspace, QStringLiteral("파일(&F)")); QVERIFY(File);
        emit File->aboutToShow();
        for (auto* Action : File->actions()) if (Action->isVisible()) QVERIFY2(!Action->text().contains(QStringLiteral("EPUB 편집 초안")), qPrintable(Action->text()));
        QVERIFY(File->findChild<QAction*>("recentHeading") || [&] { for (auto* A : File->actions()) if (A->objectName() == "recentHeading") return true; return false; }());
    }
    void singleToolbarRow()
    {
        const auto Path = root_.filePath("row.txt"); QVERIFY(Put(Path, "row\r\n"));
        DocumentWorkspaceWidget Workspace(root_.filePath("recovery-c")); Workspace.resize(1100, 700); Workspace.show();
        Workspace.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        auto* Common = Workspace.findChild<QToolBar*>("mainToolbar"); QVERIFY(Common);
        QVERIFY(Common->isVisible());
        QVERIFY(!open(Workspace, Path).isEmpty());
        auto* Pane = Workspace.findChild<DocumentViewPane*>(); QVERIFY(Pane);
        auto* Row = Pane->PrimaryToolbar(); QVERIFY(Row);
        // One row: the view's toolbar starts with the shared common actions; the top toolbar steps aside.
        QTRY_VERIFY(!Common->isVisible());
        bool Separator = false; for (auto* Action : Row->actions()) Separator |= Action->objectName() == "documentCommonSeparator";
        QVERIFY(Separator);
        for (auto* Action : Common->actions()) if (!Action->isSeparator()) { QVERIFY(Row->actions().contains(Action)); break; }
    }
    void reinterpretKeepsBytes()
    {
        const auto Path = root_.filePath("korean.txt");
        const auto Bytes = QByteArray::fromHex("b0a1b3aa0d0a"); // "가나" in CP949
        QVERIFY(Put(Path, Bytes));
        DocumentWorkspaceWidget Workspace(root_.filePath("recovery-d")); Workspace.resize(900, 600); Workspace.show();
        Workspace.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        QVERIFY(!open(Workspace, Path).isEmpty());
        auto* Bar = static_cast<DocumentStatusBar*>(Workspace.findChild<QWidget*>("documentStatusBar"));
        QTRY_VERIFY(FieldText(Bar, "encoding").contains(EncodingInfo("CP949").label_));
        auto* Pane = Workspace.findChild<DocumentViewPane*>(); QVERIFY(Pane);
        emit Pane->ReinterpretRequested(QStringLiteral("ISO-8859-1"));
        QTRY_VERIFY_WITH_TIMEOUT(FieldText(Bar, "encoding").contains(EncodingInfo("ISO-8859-1").label_), 15000);
        QFile Disk(Path); QVERIFY(Disk.open(QIODevice::ReadOnly)); QCOMPARE(Disk.readAll(), Bytes);
    }
    void progressAndRecent()
    {
        const auto Path = root_.filePath("recent.txt"); QVERIFY(Put(Path, "recent\r\n"));
        {
            DocumentWorkspaceWidget Workspace(root_.filePath("recovery-e")); Workspace.resize(900, 600); Workspace.show();
            Workspace.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
            QVERIFY(Workspace.findChild<DocumentProgressOverlay*>("documentProgress"));
            QVERIFY(!open(Workspace, Path).isEmpty());
            auto* File = TopMenu(Workspace, QStringLiteral("파일(&F)")); QVERIFY(File);
            emit File->aboutToShow();
            bool Listed = false; for (auto* Action : File->actions()) Listed |= Action->objectName() == "recentItem1" && Action->toolTip().endsWith("recent.txt");
            QVERIFY(Listed);
        }
        // The store writes recent-documents.json in the settings folder for HyperViewer and the next start.
        const auto Store = qApp->property("documentViewsConfigDir").toString() + "/recent-documents.json";
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Store), 5000);
        QFile Json(Store); QVERIFY(Json.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(Json.readAll()).contains("recent.txt"));
    }
    void recentSettings()
    {
        // 설정 › 공통 › 최근 항목 (plan 6.6): record switch, menu count 0–15 and the clear request.
        const auto Path = qApp->property("documentViewsConfigDir").toString() + "/settings-recent.json";
        DocumentSettingsDialog Dialog(Path, {});
        auto* Enabled = Dialog.findChild<QCheckBox*>("documentViews/recent/enabled"); QVERIFY(Enabled); QVERIFY(Enabled->isChecked());
        auto* Count = Dialog.findChild<QSpinBox*>("documentViews/recent/menuCount"); QVERIFY(Count);
        QCOMPARE(Count->minimum(), 0); QCOMPARE(Count->maximum(), 15); QCOMPARE(Count->value(), 9);
        auto* Clear = Dialog.findChild<QPushButton*>("settingsClearRecent"); QVERIFY(Clear);
        QSignalSpy Requests(&Dialog, &DocumentSettingsDialog::ClearRecentRequested);
        Clear->click(); QCOMPARE(Requests.count(), 1);
    }
    void imageCopyAndSlideshow()
    {
        QTemporaryDir Folder;
        for (int I = 1; I <= 3; ++I) { QImage Image(64, 40, QImage::Format_RGB32); Image.fill(QColor::fromHsv(I * 90, 200, 200)); QVERIFY(Image.save(Folder.filePath(QString("%1.png").arg(I)))); }
        DocumentWorkspaceWidget Workspace(root_.filePath("recovery-f")); Workspace.resize(900, 600); Workspace.show();
        Workspace.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        const auto Id = open(Workspace, Folder.filePath("1.png")); QVERIFY(!Id.isEmpty());
        auto* Pane = Workspace.findChild<DocumentViewPane*>(); QVERIFY(Pane);
        // 이미지 복사 without a selection copies the displayed image.
        QGuiApplication::clipboard()->clear();
        QVERIFY(Pane->RunViewCommand("edit.copy"));
        QCOMPARE(QGuiApplication::clipboard()->image().size(), QSize(64, 40));
        // The slideshow keeps running across the view reload of each step and stops on a manual step.
        const auto PathOf = [&] { for (const auto& Item : Workspace.Snapshot()["documents"].toArray()) if (Item.toObject()["documentId"].toString() == Id) return Item.toObject()["path"].toString(); return QString(); };
        Pane->SetSlideshow(true); QVERIFY(Pane->SlideshowRunning());
        QTRY_VERIFY_WITH_TIMEOUT(PathOf().endsWith("2.png"), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(PathOf().endsWith("3.png"), 8000);
        QVERIFY(Pane->SlideshowRunning());
        QVERIFY(Pane->RunViewCommand("image.previous"));
        QVERIFY(!Pane->SlideshowRunning());
    }
};

int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    QTemporaryDir Config; App.setProperty("documentViewsConfigDir", Config.path());
    DocumentChromeTests Tests;
    return QTest::qExec(&Tests, Argc, Argv);
}

#include "tstDocumentChrome.moc"
