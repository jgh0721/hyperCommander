#include "stdafx.h"
#include "HyperCommander/UIs/dialogShell.hpp"
#include "HyperCommander/UIs/fileToolsDialogs.hpp"
#include "HyperCommander/UIs/settingsDraft.hpp"
#include "UniqueLibs/settings/fileColorRules.hpp"
#include <fmwidgets/Button.h>
#include <fmwidgets/ReorderList.h>
#include <fmwidgets/TransferGraph.h>
#include <QStringListModel>
#include <QStandardItemModel>
#include <QScrollBar>
#include <fmwidgets/WrapLayout.h>
#include <fmwidgets/WrapCheckBox.h>
#include <QAccessible>
#include <fmstyle/ThemeManager.h>
#include <QtTest>
#include "UniqueLibs/permissionService.hpp"
#include <QApplication>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QSpinBox>
#include <QScrollArea>
#include <QLabel>
#include <QListView>
#include <QInputMethodEvent>

class DialogFoundationTests : public QObject
{
    Q_OBJECT
private slots:
    void footerMessagesKeepGeometry_data()
    {
        QTest::addColumn<bool>("Dark"); QTest::addColumn<int>("Points");
        for (const bool Dark : {false, true}) for (const int Points : {11, 18})
            QTest::newRow(qPrintable(QString("%1-%2").arg(Dark ? "dark" : "light").arg(Points))) << Dark << Points;
    }
    void footerMessagesKeepGeometry()
    {
        QFETCH(bool, Dark); QFETCH(int, Points);
        const auto Original = qApp->font(); const auto Restore = qScopeGuard([&] { qApp->setFont(Original); });
        auto& Theme = fm::style::ThemeManager::instance(); Theme.setDesign(fm::style::Design::Watercolor);
        Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        if (!Theme.isInstalled()) Theme.install(*qApp);
        auto Font = Original; Font.setPointSize(Points); qApp->setFont(Font);
        QTemporaryDir Root; QVERIFY(Root.isValid());
        QFile Source(Root.filePath("source.txt")); QVERIFY(Source.open(QIODevice::WriteOnly)); Source.write("fixture"); Source.close();
        for (const auto& Kind : {"copy", "move", "rename", "mkdir", "input", "extract", "new-file"}) {
            std::unique_ptr<hc::ui::DialogShell> Dialog;
            hc::ui::FileOperationDialog* Operation = nullptr;
            if (QString(Kind) == "input") Dialog = std::make_unique<hc::ui::FileInputDialog>(hc::ui::FileDialogRequest{"입력", {}, "경로", Root.path(), true});
            else if (QString(Kind) == "extract") Dialog = std::make_unique<hc::ui::ArchiveExtractDialog>(QStringList{"source.zip"}, Root.path(), false, nullptr, Root.path());
            else if (QString(Kind) == "new-file") Dialog = std::make_unique<hc::ui::NewFileDialog>(Root.path(), true);
            else {
                hc::ui::FileOperationDialogRequest Request; Request.command_ = "hc.file." + QString::fromLatin1(Kind);
                Request.source_ = Root.path(); Request.relativeBase_ = Root.path(); Request.names_ = {"source.txt"};
                Request.initialValue_ = QString(Kind) == "rename" ? "source.txt" : QString(Kind) == "mkdir" ? "new-folder" : Root.path();
                Request.nativeSource_ = Request.nativeDestination_ = true;
                auto Value = std::make_unique<hc::ui::FileOperationDialog>(Request); Operation = Value.get(); Dialog = std::move(Value);
            }
            Dialog->show(); QVERIFY(QTest::qWaitForWindowExposed(Dialog.get()));
            auto* Edit = Dialog->findChild<QLineEdit*>(Operation ? "hc.operation.editor" : QString(Kind) == "input" ? "hc.dialog.input.editor" : QString(Kind) == "extract" ? "hc.archive.extract.path" : "hc.newFile.name");
            QVERIFY(Edit);
            const auto Size = Dialog->size();
            const auto ButtonRect = [&](QWidget* Widget) { return QRect(Widget->mapTo(Dialog.get(), QPoint()), Widget->size()); };
            const auto Accept = ButtonRect(Dialog->AcceptButton()), Cancel = ButtonRect(Dialog->CancelButton());
            const auto EditorTop = Edit->mapTo(Dialog.get(), QPoint()).y();
            const QStringList Inputs{QString{}, ".\\", "bad?name", Root.path(), "new-file.txt", QString(".\\") + QString(220, 'a') + "\\"};
            for (const auto& Input : Inputs) {
                Edit->setText(Input); QCoreApplication::processEvents();
                QCOMPARE(Dialog->size(), Size); QCOMPARE(ButtonRect(Dialog->AcceptButton()), Accept); QCOMPARE(ButtonRect(Dialog->CancelButton()), Cancel);
                if (Operation && !Operation->ResolvedTarget().isEmpty()) {
                    auto* Summary = Dialog->findChild<QLabel*>("hc.operation.targetSummary"); QVERIFY(Summary);
                    if (!Summary->isHidden()) QTRY_COMPARE(Summary->property("target").toString(), Operation->ResolvedTarget());
                }
                QCoreApplication::processEvents(); QCOMPARE(Dialog->size(), Size);
                QCOMPARE(ButtonRect(Dialog->AcceptButton()), Accept); QCOMPARE(ButtonRect(Dialog->CancelButton()), Cancel);
                QCOMPARE(Edit->mapTo(Dialog.get(), QPoint()).y(), EditorTop);
            }
            auto* Message = Dialog->findChild<QLabel*>("hc.dialog.message"); QVERIFY(Message);
            const auto Text = QString("<provider> & ") + QString(300, 'x') + "\n다음 줄 전체 안내";
            Dialog->SetMessage(Text, true); QCoreApplication::processEvents();
            QCOMPARE(Message->text(), Text); QCOMPARE(Message->toolTip(), Text); QCOMPARE(Message->textFormat(), Qt::PlainText);
            QCOMPARE(Message->font(), Dialog->font()); QVERIFY(Message->isVisible());
            const auto MessageRect = ButtonRect(Message);
            QVERIFY(MessageRect.right() < Cancel.left()); QVERIFY(qAbs(MessageRect.center().y() - Cancel.center().y()) <= 1);
            QCOMPARE(Dialog->size(), Size); QCOMPARE(ButtonRect(Dialog->AcceptButton()), Accept);
            const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
            if (!Artifacts.isEmpty() && QString(Kind) == "move") {
                QDir().mkpath(Artifacts);
                Edit->clear();
                Dialog->SetMessage("이름 또는 경로를 입력하세요.", true); QCoreApplication::processEvents();
                QVERIFY(Dialog->grab().save(QDir(Artifacts).filePath(QString("footer-%1-%2-error.png").arg(Dark).arg(Points))));
                Dialog->SetMessage({}); QCoreApplication::processEvents();
                QVERIFY(Dialog->grab().save(QDir(Artifacts).filePath(QString("footer-%1-%2-clear.png").arg(Dark).arg(Points))));
            }
            Dialog->SetMessage({}); QCoreApplication::processEvents(); QVERIFY(!Message->isVisible());
            QCOMPARE(Dialog->size(), Size); QCOMPARE(ButtonRect(Dialog->AcceptButton()), Accept); QCOMPARE(ButtonRect(Dialog->CancelButton()), Cancel);
            Dialog->resize(Size + QSize(30, 20)); QCoreApplication::processEvents(); const auto UserSize = Dialog->size();
            Edit->setText("changed-name.txt"); QTest::qWait(250); QCOMPARE(Dialog->size(), UserSize);
        }
    }
    void nativeCompletionPaths_data()
    {
        QTest::addColumn<QString>("Input"); QTest::addColumn<QString>("Expected");
        QTest::newRow("bare-relative") << "Fe" << "Feedback\\";
        QTest::newRow("dot-backslash") << ".\\Fe" << ".\\Feedback\\";
        QTest::newRow("dot-slash") << "./Fe" << "./Feedback/";
        QTest::newRow("parent") << "..\\Fe" << "..\\Feedback\\";
        QTest::newRow("absolute-slash") << "{root}/current/Fe" << "{root}/current/Feedback/";
        QTest::newRow("absolute-backslash") << "{native}\\current\\Fe" << "{native}\\current\\Feedback\\";
        QTest::newRow("case-insensitive") << "fe" << "Feedback\\";
    }
    void nativeCompletionPaths()
    {
        QFETCH(QString, Input); QFETCH(QString, Expected);
        QTemporaryDir Root; QVERIFY(Root.isValid());
        QVERIFY(QDir(Root.path()).mkpath("current/Feedback")); QVERIFY(QDir(Root.path()).mkdir("Feedback"));
        Input.replace("{root}", Root.path()).replace("{native}", QDir::toNativeSeparators(Root.path()));
        Expected.replace("{root}", Root.path()).replace("{native}", QDir::toNativeSeparators(Root.path()));
        hc::ui::FileOperationDialogRequest Request; Request.command_ = "hc.file.move";
        Request.source_ = Root.filePath("current"); Request.relativeBase_ = Request.source_;
        Request.initialValue_ = "D:/different-target"; Request.names_ = {"source.txt"}; Request.nativeDestination_ = true;
        hc::ui::FileOperationDialog Dialog(Request); Dialog.show(); Dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&Dialog));
        auto* Edit = Dialog.findChild<QLineEdit*>("hc.operation.editor"); QVERIFY(Edit);
        auto* Popup = Edit->findChild<QListView*>("hc.pathCompletion.popup"); QVERIFY(Popup);
        QTRY_VERIFY(Edit->hasFocus()); Edit->setText(Input.left(Input.size() - 1));
        QTest::keyClicks(Edit, Input.right(1)); QTRY_VERIFY(Popup->isVisible());
        QCOMPARE(Popup->currentIndex().data().toString(), Expected); QCOMPARE(Edit->text(), Input);
        QSignalSpy Accepted(&Dialog, &QDialog::accepted);
        QTest::keyClick(Edit, Qt::Key_Return); QCOMPARE(Edit->text(), Expected); QCOMPARE(Accepted.count(), 0);
        QTest::keyClick(Edit, Qt::Key_Escape); QVERIFY(Dialog.isVisible()); QVERIFY(!Popup->isVisible());
        QTRY_VERIFY(Dialog.AcceptButton()->isEnabled()); QTest::keyClick(Edit, Qt::Key_Return);
        QCOMPARE(Accepted.count(), 1);
    }
    void nativeCompletionKeyboardAndUndo()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        QVERIFY(QDir(Root.path()).mkdir("Feedback")); QVERIFY(QDir(Root.path()).mkdir("Feedmore"));
        hc::ui::FileOperationDialogRequest Request; Request.command_ = "hc.file.copy"; Request.source_ = Root.path();
        Request.names_ = {"source.txt"}; Request.nativeDestination_ = true;
        hc::ui::FileOperationDialog Dialog(Request); Dialog.show(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
        auto* Edit = Dialog.findChild<QLineEdit*>("hc.operation.editor"); QVERIFY(Edit);
        auto* Popup = Edit->findChild<QListView*>("hc.pathCompletion.popup"); QVERIFY(Popup);
        QTest::keyClicks(Edit, "Fe"); QTRY_VERIFY(Popup->isVisible()); QCOMPARE(Popup->model()->rowCount(), 2);
        QTest::keyClick(Edit, Qt::Key_Down); QCOMPARE(Popup->currentIndex().row(), 1); QCOMPARE(Edit->text(), QString("Fe"));
        QTest::keyClick(Edit, Qt::Key_Up); QCOMPARE(Popup->currentIndex().row(), 0);
        QTest::keyClick(Edit, Qt::Key_PageDown); QCOMPARE(Popup->currentIndex().row(), 1);
        QTest::keyClick(Edit, Qt::Key_PageUp); QCOMPARE(Popup->currentIndex().row(), 0);
        QTest::keyClick(Edit, Qt::Key_Tab); QCOMPARE(Edit->text(), QString("Feedback\\")); QVERIFY(Edit->hasFocus());
        QTest::keyClick(Edit, Qt::Key_Z, Qt::ControlModifier); QCOMPARE(Edit->text(), QString("Fe"));
        QTest::keyClick(Edit, Qt::Key_Down); QTRY_VERIFY(Popup->isVisible()); QTest::keyClick(Edit, Qt::Key_Escape);
        QVERIFY(Dialog.isVisible()); QVERIFY(!Popup->isVisible()); QCOMPARE(Edit->text(), QString("Fe"));
        QTest::keyClick(Edit, Qt::Key_Down); QTRY_VERIFY(Popup->isVisible());
        QTest::keyClick(Edit, Qt::Key_Escape); QTest::keyClick(Edit, Qt::Key_Escape); QVERIFY(!Dialog.isVisible());
    }
    void nativeCompletionFilesSuffixAndTheme()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid()); QVERIFY(QDir(Root.path()).mkdir("Feedback"));
        QFile File(Root.filePath("Feedback.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.close();
        hc::ui::FileOperationDialogRequest Request; Request.command_ = "hc.file.copy"; Request.source_ = Root.path();
        Request.names_ = {"source.txt"}; Request.nativeDestination_ = true;
        hc::ui::FileOperationDialog Dialog(Request); Dialog.show(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
        auto* Edit = Dialog.findChild<QLineEdit*>("hc.operation.editor"); QVERIFY(Edit);
        auto* Popup = Edit->findChild<QListView*>("hc.pathCompletion.popup"); QVERIFY(Popup);
        QTest::keyClicks(Edit, "Fe"); QTRY_VERIFY(Popup->isVisible()); QCOMPARE(Popup->model()->rowCount(), 2);
        QCOMPARE(Popup->model()->index(0, 0).data().toString(), QString("Feedback\\"));
        QCOMPARE(Popup->model()->index(1, 0).data().toString(), QString("Feedback.txt"));
        auto Font = Edit->font(); Font.setPointSize(19); Edit->setFont(Font); QCOMPARE(Popup->font(), Edit->font());
        auto Palette = Edit->palette(); Palette.setColor(QPalette::Base, QColor(24, 30, 36)); Palette.setColor(QPalette::Text, Qt::white);
        Edit->setPalette(Palette); QCOMPARE(Popup->palette().color(QPalette::Base), Edit->palette().color(QPalette::Base));
        QCOMPARE(Popup->palette().color(QPalette::Text), Edit->palette().color(QPalette::Text));
        QSignalSpy Accepted(&Dialog, &QDialog::accepted); QTest::keyClick(Edit, Qt::Key_Down);
        QTest::keyClick(Edit, Qt::Key_Enter, Qt::KeypadModifier); QCOMPARE(Edit->text(), QString("Feedback.txt")); QCOMPARE(Accepted.count(), 0);
        Edit->setText("Fe\\keep.txt"); Edit->setCursorPosition(2); QTest::keyClick(Edit, Qt::Key_Space, Qt::ControlModifier);
        QTRY_VERIFY(Popup->isVisible()); QCOMPARE(Popup->model()->rowCount(), 1); QTest::keyClick(Edit, Qt::Key_Return);
        QCOMPARE(Edit->text(), QString("Feedback\\keep.txt")); QCOMPARE(Accepted.count(), 0);
    }
    void nativeCompletionDirectoriesAndStaleResults()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid()); QVERIFY(QDir(Root.path()).mkpath("Feedback/Child"));
        QFile File(Root.filePath("Feedback.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.close();
        hc::ui::ArchiveExtractDialog Dialog({"source.zip"}, Root.path(), false, nullptr, Root.path());
        Dialog.show(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
        auto* Edit = Dialog.findChild<QLineEdit*>("hc.archive.extract.path"); QVERIFY(Edit);
        auto* Popup = Edit->findChild<QListView*>("hc.pathCompletion.popup"); QVERIFY(Popup);
        Edit->clear(); QTest::keyClicks(Edit, "Fe"); QTRY_VERIFY(Popup->isVisible()); QCOMPARE(Popup->model()->rowCount(), 1);
        QTest::keyClick(Edit, Qt::Key_Return); QTRY_VERIFY(Popup->isVisible());
        QCOMPARE(Popup->currentIndex().data().toString(), QString("Feedback\\Child\\"));
        Edit->setText("missing-prefix"); QTest::qWait(200); QVERIFY(!Popup->isVisible());
        Edit->setText("Fe"); QTest::keyClick(Edit, Qt::Key_Space, Qt::ControlModifier);
        Edit->setText("missing-prefix"); QTest::qWait(200); QVERIFY(!Popup->isVisible());
        Edit->setText("Fe"); QTest::keyClick(Edit, Qt::Key_Space, Qt::ControlModifier);
        QTest::keyClick(Edit, Qt::Key_Escape); QTest::qWait(200); QVERIFY(!Popup->isVisible()); QVERIFY(Dialog.isVisible());
        QInputMethodEvent Preedit(QString::fromUtf8("피"), {}); QApplication::sendEvent(Edit, &Preedit);
        QSignalSpy Accepted(&Dialog, &QDialog::accepted); QTest::keyClick(Edit, Qt::Key_Return); QCOMPARE(Accepted.count(), 0);
        QInputMethodEvent Commit; Commit.setCommitString(QString::fromUtf8("피")); QApplication::sendEvent(Edit, &Commit);
        QVERIFY(!Edit->property("hc.pathCompletion.composing").toBool());
    }
    void nativeCompletionThemeAndMouse_data()
    {
        QTest::addColumn<bool>("Watercolor"); QTest::addColumn<bool>("Dark");
        QTest::newRow("standard-light") << false << false;
        QTest::newRow("standard-dark") << false << true;
        QTest::newRow("watercolor-light") << true << false;
        QTest::newRow("watercolor-dark") << true << true;
    }
    void nativeCompletionThemeAndMouse()
    {
        QFETCH(bool, Watercolor); QFETCH(bool, Dark);
        auto& Theme = fm::style::ThemeManager::instance();
        const auto OldDesign = Theme.design(); const auto OldScheme = Theme.scheme();
        const auto OldFamily = Theme.monoFontFamily(); const int OldSize = Theme.monoFontPx();
        const auto Restore = qScopeGuard([&] { Theme.setDesign(OldDesign); Theme.setScheme(OldScheme); Theme.setMonoFont(OldFamily, OldSize); });
        Theme.setDesign(Watercolor ? fm::style::Design::Watercolor : fm::style::Design::Standard);
        Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        if (!Theme.isInstalled()) Theme.install(*qApp);
        QTemporaryDir Root; QVERIFY(Root.isValid()); QVERIFY(QDir(Root.path()).mkdir(QString::fromUtf8("피드백")));
        hc::ui::ArchiveExtractDialog Dialog({"source.zip"}, Root.path(), false, nullptr, Root.path());
        Dialog.show(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
        auto* Edit = Dialog.findChild<QLineEdit*>("hc.archive.extract.path"); QVERIFY(Edit);
        auto* Popup = Edit->findChild<QListView*>("hc.pathCompletion.popup"); QVERIFY(Popup);
        Theme.setMonoFont("Consolas", 24); QTest::qWait(50); Edit->clear();
        QInputMethodEvent Commit; Commit.setCommitString(QString::fromUtf8("피")); QApplication::sendEvent(Edit, &Commit);
        QTRY_VERIFY(Popup->isVisible()); QCOMPARE(Popup->font(), Edit->font()); QCOMPARE(Edit->font().pointSizeF(), 18.0);
        QVERIFY(Edit->font().families().contains("Consolas"));
        QCOMPARE(Popup->palette().color(QPalette::Base), Theme.colors().toPalette().color(QPalette::Base));
        QCOMPARE(Popup->palette().color(QPalette::Highlight), Theme.colors().toPalette().color(QPalette::Highlight));
        QCOMPARE(Popup->currentIndex().data().toString(), QString::fromUtf8("피드백\\"));
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) { QDir().mkpath(Artifacts); QVERIFY(Popup->grab().save(QDir(Artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".png"))); }
        QSignalSpy Accepted(&Dialog, &QDialog::accepted);
        QTest::mouseClick(Popup->viewport(), Qt::LeftButton, {}, Popup->visualRect(Popup->currentIndex()).center());
        QCOMPARE(Edit->text(), QString::fromUtf8("피드백\\")); QCOMPARE(Accepted.count(), 0);
    }
    void transferTimeAxisRollsWithoutLosingProgress()
    {
        fm::ui::TransferGraph Graph; Graph.start(200000); Graph.setAxis(fm::ui::TransferGraph::Time);
        Graph.addSample(0,0); Graph.addSample(100000,55000); const auto Span=Graph.timeWindowEndMs()-Graph.timeWindowStartMs();
        Graph.addSample(130000,70000); QVERIFY(Graph.timeWindowStartMs()>0); QCOMPARE(Graph.timeWindowEndMs()-Graph.timeWindowStartMs(),Span);
        QCOMPARE(Graph.bytesDone(),130000ll); QVERIFY(Graph.currentSpeed()>0); Graph.setPaused(true); Graph.addSample(130000,70500); QVERIFY(Graph.isPaused());
        Graph.setAxis(fm::ui::TransferGraph::Progress); QCOMPARE(Graph.totalBytes(),200000ll); QCOMPARE(Graph.bytesDone(),130000ll);
        Graph.setTimeWindowMs(10000); QCOMPARE(Graph.timeWindowEndMs()-Graph.timeWindowStartMs(),10000ll);
        Graph.start(500000); QCOMPARE(Graph.timeWindowStartMs(),0ll); QCOMPARE(Graph.bytesDone(),0ll);
    }
    void reorderListsPreserveIdentityAndCancel()
    {
        fm::ui::ReorderListWidget List; List.resize(280, 180);
        for (int Row = 0; Row < 30; ++Row) {
            auto* Item = new QListWidgetItem(QString::number(Row), &List);
            Item->setData(Qt::UserRole, "id." + QString::number(Row)); Item->setCheckState(Row % 2 ? Qt::Unchecked : Qt::Checked);
        }
        List.show(); QVERIFY(QTest::qWaitForWindowExposed(&List)); QTest::qWait(30);
        auto* Original = List.item(0); List.setCurrentItem(Original);
        QSignalSpy Moved(List.model(), &QAbstractItemModel::rowsMoved);
        const auto Send = [&](QEvent::Type Type, QPoint Point, Qt::MouseButton Button, Qt::MouseButtons Buttons) {
            QMouseEvent Event(Type, Point, List.viewport()->mapToGlobal(Point), Button, Buttons, Qt::NoModifier);
            QApplication::sendEvent(List.viewport(), &Event);
        };
        const auto Start = List.HandleRect(0).center();
        const auto Target = List.visualItemRect(List.item(3)).bottomLeft() + QPoint(15, -1);
        Send(QEvent::MouseButtonPress, Start, Qt::LeftButton, Qt::LeftButton);
        Send(QEvent::MouseMove, Target, Qt::NoButton, Qt::LeftButton);
        Send(QEvent::MouseButtonRelease, Target, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(Moved.count(), 1); QCOMPARE(List.item(3), Original); QCOMPARE(List.currentItem(), Original);
        QCOMPARE(Original->checkState(), Qt::Checked); QCOMPARE(Original->data(Qt::UserRole).toString(), QString("id.0"));
        QTest::keyClick(&List, Qt::Key_Up, Qt::AltModifier); QCOMPARE(List.row(Original), 2);
        const auto Before = List.item(0);
        Send(QEvent::MouseButtonPress, List.HandleRect(0).center(), Qt::LeftButton, Qt::LeftButton);
        Send(QEvent::MouseMove, Target, Qt::NoButton, Qt::LeftButton);
        QTest::keyClick(&List, Qt::Key_Escape);
        Send(QEvent::MouseButtonRelease, Target, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(List.item(0), Before);
        Send(QEvent::MouseButtonPress, List.HandleRect(0).center(), Qt::LeftButton, Qt::LeftButton);
        Send(QEvent::MouseMove, QPoint(12, List.viewport()->height() - 2), Qt::NoButton, Qt::LeftButton);
        QTest::qWait(160); QVERIFY(List.verticalScrollBar()->value() > 0);
        Send(QEvent::MouseButtonRelease, QPoint(-8, -8), Qt::LeftButton, Qt::NoButton); QCOMPARE(List.item(0), Before);
        List.scrollToTop(); QTest::qWait(20);
        const auto CheckedBefore = List.item(0)->checkState();
        const auto Bounds = List.visualItemRect(List.item(0));
        QTest::mouseClick(List.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(Bounds.left() + 42, Bounds.center().y()));
        QVERIFY(List.item(0)->checkState() != CheckedBefore); QCOMPARE(List.item(0), Before);
        List.setLayoutDirection(Qt::RightToLeft); QTest::qWait(30);
        QVERIFY(List.HandleRect(0).center().x() > List.viewport()->width() / 2);
        QVERIFY(!List.MoveRow(-1, 0)); QVERIFY(!List.MoveRow(0, 30));
        // Model/view variant uses real model moves; unsupported models refuse.
        fm::ui::ReorderListView View; QStringListModel Model({"a", "b", "c"});
        View.setModel(&Model); QVERIFY(View.MoveRow(0, 2)); QCOMPARE(Model.stringList(), QStringList({"b", "c", "a"}));
        QCOMPARE(View.currentIndex().data().toString(), QString("a"));
        QStandardItemModel ReadOnlyMove; ReadOnlyMove.appendRow(new QStandardItem("retained"));
        ReadOnlyMove.appendRow(new QStandardItem("also retained"));
        View.setModel(&ReadOnlyMove); QVERIFY(!View.MoveRow(0, 1)); QCOMPARE(ReadOnlyMove.rowCount(), 2);
    }


    void permissionRequestsAndIdentity()
    {
        using namespace hc::permissions;
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        auto R=Request("create",Temp.path(),"sample.txt",{},1,false);
        QCOMPARE(Validate(R), DWORD(ERROR_SUCCESS)); QCOMPARE(Execute(R), DWORD(ERROR_SUCCESS));
        QVERIFY(Execute(R)!=ERROR_SUCCESS); // No overwrite, even for approved requests.
        QFile File(Temp.filePath("sample.txt")); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Bytes=File.readAll(); File.close();
        auto Rename=Request("rename",Temp.path(),"renamed.txt",File.fileName());
        auto Stale=Rename; Stale["sourceId"]="stale"; QCOMPARE(Execute(Stale),DWORD(ERROR_FILE_INVALID));
        QCOMPARE(Execute(Rename),DWORD(ERROR_SUCCESS)); QVERIFY(!File.exists());
        File.setFileName(Temp.filePath("renamed.txt")); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(),Bytes); File.close();
        QCOMPARE(Execute(Request("mkdir",Temp.path(),"child")),DWORD(ERROR_SUCCESS)); QVERIFY(QFileInfo(Temp.filePath("child")).isDir());
        R["parentId"]="stale"; QCOMPARE(Execute(R),DWORD(ERROR_FILE_INVALID));
        R=Request("create",Temp.path(),"../escape"); QCOMPARE(Validate(R),DWORD(ERROR_INVALID_PARAMETER));
        R=Request("delete",Temp.path(),"renamed.txt"); QCOMPARE(Validate(R),DWORD(ERROR_INVALID_PARAMETER));
        R=Request("create",Temp.path(),"expired.txt"); R["utc"]=QDateTime::currentDateTimeUtc().addSecs(-121).toString(Qt::ISODateWithMs);
        QCOMPARE(Execute(R),DWORD(ERROR_TIMEOUT)); QVERIFY(!QFileInfo::exists(Temp.filePath("expired.txt")));
        R=Request("create",Temp.path(),"future.txt"); R["utc"]=QDateTime::currentDateTimeUtc().addSecs(30).toString(Qt::ISODateWithMs);
        QCOMPARE(Validate(R),DWORD(ERROR_TIMEOUT));
        R=Request("create",Temp.path(),"unknown.txt"); R.remove("source"); R["unknown"]=true; QCOMPARE(Validate(R),DWORD(ERROR_INVALID_PARAMETER));
        const auto I=Inspect(Temp.path()); QVERIFY(I.read_); QVERIFY(I.create_); QVERIFY2(!I.owner_.isEmpty(),qPrintable(I.error_));
    }
    void permissionDeniedFixtureAndRestore()
    {
        using namespace hc::permissions;
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        const auto Native=hc::filetools::NativePath(Temp.path());
        PACL Original=nullptr, Denied=nullptr; PSECURITY_DESCRIPTOR Descriptor=nullptr;
        QCOMPARE(GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(Native.utf16())),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&Original,nullptr,&Descriptor),DWORD(ERROR_SUCCESS));
        bool Changed=false;
        const auto Restore=qScopeGuard([&] { if(Changed) SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(Native.utf16())),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,Original,nullptr); if(Denied)LocalFree(Denied); LocalFree(Descriptor); });
        BYTE Sid[SECURITY_MAX_SID_SIZE]{}; DWORD Size=sizeof(Sid); QVERIFY(CreateWellKnownSid(WinWorldSid,nullptr,Sid,&Size));
        EXPLICIT_ACCESSW Entry{}; Entry.grfAccessPermissions=FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY; Entry.grfAccessMode=DENY_ACCESS; Entry.grfInheritance=NO_INHERITANCE;
        Entry.Trustee.TrusteeForm=TRUSTEE_IS_SID; Entry.Trustee.TrusteeType=TRUSTEE_IS_WELL_KNOWN_GROUP; Entry.Trustee.ptstrName=reinterpret_cast<LPWSTR>(Sid);
        QCOMPARE(SetEntriesInAclW(1,&Entry,Original,&Denied),DWORD(ERROR_SUCCESS));
        QCOMPARE(SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(Native.utf16())),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,Denied,nullptr),DWORD(ERROR_SUCCESS)); Changed=true;
        QVERIFY(!Access(Temp.path(),FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY));
        QVERIFY(!hc::filetools::Create(Temp.path(),"denied.txt",1,false).isEmpty()); QVERIFY(!QFileInfo::exists(Temp.filePath("denied.txt")));
        QCOMPARE(SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(Native.utf16())),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,Original,nullptr),DWORD(ERROR_SUCCESS)); Changed=false;
        QVERIFY(Access(Temp.path(),FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY)); QVERIFY(hc::filetools::Create(Temp.path(),"restored.txt",1,false).isEmpty());
    }
    void fileTemplatesAndNoReplace()
    {
        using namespace hc::filetools;
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        for (int I = 0; I < 8; ++I) {
            const auto Name = TemplateName("sample", I); QVERIFY2(Create(Temp.path(), Name, I, true).isEmpty(), qPrintable(Name));
            QFile File(Temp.filePath(Name)); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Data = File.readAll(); File.close();
            QCOMPARE(Data, (I ? QByteArray::fromHex("efbbbf") : QByteArray()) + Templates()[I].text_.toUtf8());
            QVERIFY(!Create(Temp.path(), Name, I, false).isEmpty()); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Data);
        }
        for (const auto& Name : {"CON.txt", "../escape", "bad:name", "tail.", "", "LPT1"}) QVERIFY(!NameError(Name).isEmpty());
        QVERIFY(!Create(Temp.filePath("missing"), "file.txt", 1, false).isEmpty());
        QVERIFY(!QFileInfo::exists(Temp.filePath("missing/file.txt")));
        QCOMPARE(TemplateName("source.cpp", 6), QString("source.json"));
        QCOMPARE(TemplateName("source.cpp", 5), QString("CMakeLists.txt"));
    }
    void renameCyclesRollbackAndStaleUndo()
    {
        using namespace hc::filetools;
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        const auto Write = [&](const QString& Name, const QByteArray& Data) { QFile F(Temp.filePath(Name)); QVERIFY(F.open(QIODevice::WriteOnly)); QCOMPARE(F.write(Data), Data.size()); };
        const auto Read = [&](const QString& Name) { QFile F(Temp.filePath(Name)); if (!F.open(QIODevice::ReadOnly)) return QByteArray(); return F.readAll(); };
        Write("a.txt", "AAA"); Write("b.txt", "BBB");
        const QList<Entry> Entries{Inspect(Temp.filePath("a.txt")), Inspect(Temp.filePath("b.txt"))};
        QList<RenameRow> Swap{{Entries[0], Temp.filePath("b.txt"), {}}, {Entries[1], Temp.filePath("a.txt"), {}}};
        auto R = Execute(Swap); QVERIFY2(R.complete_, qPrintable(R.error_)); QCOMPARE(Read("a.txt"), QByteArray("BBB")); QCOMPARE(Read("b.txt"), QByteArray("AAA"));
        QList<RenameRow> Undo{{R.current_[0], Entries[0].path_, {}}, {R.current_[1], Entries[1].path_, {}}};
        auto Back = Execute(Undo); QVERIFY2(Back.complete_, qPrintable(Back.error_)); QCOMPARE(Read("a.txt"), QByteArray("AAA"));
        auto Interrupted = Execute(Swap, [](int Step) { return Step == 3; }); QVERIFY(!Interrupted.complete_);
        QCOMPARE(Interrupted.current_[0].path_, Entries[0].path_); QCOMPARE(Interrupted.current_[1].path_, Entries[1].path_);
        QCOMPARE(Read("a.txt"), QByteArray("AAA")); QCOMPARE(Read("b.txt"), QByteArray("BBB"));
        QVERIFY(QDir(Temp.path()).entryList({".hc-rename-*"}, QDir::Files | QDir::Hidden).isEmpty());
        QList<RenameRow> Case{{Inspect(Temp.filePath("a.txt")), Temp.filePath("A.TXT"), {}}};
        auto C = Execute(Case); QVERIFY2(C.complete_, qPrintable(C.error_)); QVERIFY(QDir(Temp.path()).entryList().contains("A.TXT"));
        Write("outside.txt", "OUTSIDE"); QList<RenameRow> Collision{{C.current_[0], Temp.filePath("outside.txt"), {}}};
        QVERIFY(!Execute(Collision).complete_); QCOMPARE(Read("outside.txt"), QByteArray("OUTSIDE"));
        auto Stale = C.current_[0]; QVERIFY(QFile::rename(Stale.path_, Temp.filePath("held.txt"))); Write("A.TXT", "REPLACEMENT");
        QVERIFY(!Execute({{Stale, Temp.filePath("restored.txt"), {}}}).complete_); QCOMPARE(Read("A.TXT"), QByteArray("REPLACEMENT"));
        const auto Forward = Execute({{Inspect(Temp.filePath("b.txt")), Temp.filePath("moved.txt"), {}}}); QVERIFY(Forward.complete_);
        Write("b.txt", "NEW OWNER"); QVERIFY(!Execute({{Forward.current_[0], Temp.filePath("b.txt"), {}}}).complete_);
        QCOMPARE(Read("b.txt"), QByteArray("NEW OWNER")); QCOMPARE(Read("moved.txt"), QByteArray("BBB"));
        QVERIFY(QDir(Temp.path()).mkdir("partial")); Write("partial/x.txt", "X"); Write("partial/y.txt", "Y");
        const QList<RenameRow> Partial{{Inspect(Temp.filePath("partial/x.txt")), Temp.filePath("partial/y.txt"), {}}, {Inspect(Temp.filePath("partial/y.txt")), Temp.filePath("partial/x.txt"), {}}};
        const auto Failed = Execute(Partial, [&](int Step) { if (Step == 2) Write("partial/x.txt", "EXTERNAL"); return false; });
        QVERIFY(!Failed.complete_); QVERIFY(Failed.current_[0].path_ != Partial[0].source_.path_);
        QCOMPARE(Read("partial/x.txt"), QByteArray("EXTERNAL")); QCOMPARE(Read("partial/y.txt"), QByteArray("Y"));
        QFile Retained(Failed.current_[0].path_); QVERIFY(Retained.open(QIODevice::ReadOnly)); QCOMPARE(Retained.readAll(), QByteArray("X"));
        Rules Rule; Rule.name_ = "[N]_[C]"; Rule.extension_ = "[E]";
        auto P = Plan({Inspect(Temp.filePath("b.txt"))}, Rule); QCOMPARE(QFileInfo(P[0].target_).fileName(), QString("b_001.txt"));
        Rule.name_ = "same"; Rule.extension_ = "txt"; P = Plan(Entries, Rule); QVERIFY(!P[0].error_.isEmpty() && !P[1].error_.isEmpty());
        Rule.name_ = "[unknown]"; QVERIFY(!Plan(Entries, Rule)[0].error_.isEmpty());
        Rule.name_ = "[N]"; Rule.regex_ = true; Rule.find_ = "["; QVERIFY(!Plan(Entries, Rule)[0].error_.isEmpty());
        QVERIFY(QDir(Temp.path()).mkdir("folder")); Write("folder/child.txt", "CHILD");
        const auto Folder = Execute({{Inspect(Temp.filePath("folder")), Temp.filePath("renamedFolder"), {}}});
        QVERIFY2(Folder.complete_, qPrintable(Folder.error_)); QCOMPARE(Read("renamedFolder/child.txt"), QByteArray("CHILD"));
    }
    void r9MasksDatesAndLargeSelection()
    {
        using namespace hc::filetools;
        Entry E; E.path_ = "C:/sample/가😀나다.TXT"; E.identity_ = "stable"; E.modified_ = QDateTime(QDate(2025,12,31), QTime(23,59,59), QTimeZone::UTC);
        Rules R; R.name_ = "[N:2:2]_[C:+4]_[date]"; R.dateFormat_ = "yyyyMMdd_HHmmss"; R.dateOffsetSeconds_ = 1;
        auto P = Plan({E}, R); QVERIFY(P[0].error_.isEmpty()); QCOMPARE(QFileInfo(P[0].target_).fileName(), QString::fromUtf8("😀나_005_20260101_000000.TXT"));
        R.name_ = "[N2-3]"; QCOMPARE(QFileInfo(Plan({E}, R)[0].target_).fileName(), QString::fromUtf8("😀나.TXT"));
        R.name_ = "[N:-2:2]"; QCOMPARE(QFileInfo(Plan({E}, R)[0].target_).fileName(), QString::fromUtf8("나다.TXT"));
        R.name_ = "[N]"; R.find_ = ".TXT"; R.replace_ = ".md"; R.includeExtension_ = true;
        QCOMPARE(QFileInfo(Plan({E}, R)[0].target_).suffix(), QString("md"));
        R.includeExtension_ = false; QCOMPARE(QFileInfo(Plan({E}, R)[0].target_).suffix(), QString("TXT"));
        for (const auto& Bad : {"[N:0:2]", "[N:99:2]", "[N3-1]", "[N", "N]", "[C:999999999999999]"}) { R.name_ = Bad; QVERIFY(!Plan({E}, R)[0].error_.isEmpty()); }
        R = Rules{}; R.name_ = "[date]"; R.dateSource_ = 1; QVERIFY(!Plan({E}, R)[0].error_.isEmpty());
        E.captured_ = E.modified_; QVERIFY(Plan({E}, R)[0].error_.isEmpty());
        QList<Entry> Many; for (int I = 0; I < 2501; ++I) { auto Item = E; Item.path_ = QString("C:/sample/source%1.txt").arg(I); Many.append(Item); }
        R = Rules{}; R.name_ = "item_[C]"; P = Plan(Many, R); QCOMPARE(P.size(), 2501); QCOMPARE(QFileInfo(P.last().target_).fileName(), QString("item_2501.txt"));
        for (const auto& Row : P) QVERIFY(Row.error_.isEmpty());
        QCOMPARE(NativePath("//server/share/folder/file.txt"), QString("\\\\?\\UNC\\server\\share\\folder\\file.txt"));
        QTemporaryDir Temp; QFile File(Temp.filePath("changed.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("one"); File.close();
        const auto Old = Inspect(File.fileName()); QVERIFY(File.open(QIODevice::Append)); File.write("changed"); File.close();
        QVERIFY(!Execute({{Old, Temp.filePath("new.txt"), {}}}).complete_); QVERIFY(File.exists()); QVERIFY(!QFileInfo::exists(Temp.filePath("new.txt")));
    }
    void r9BoundedExifParsing()
    {
        using namespace hc::filetools;
        // Classic TIFF little endian: IFD0 -> Exif IFD -> DateTimeOriginal.
        auto Tiff = QByteArray::fromHex("49492a0008000000010069870400010000001a00000000000000010003900200140000002c00000000000000");
        Tiff += QByteArray("2024:02:29 12:34:56\0", 20);
        const auto Expected = QDateTime(QDate(2024,2,29), QTime(12,34,56), QTimeZone::UTC); QCOMPARE(ExifDate(Tiff), Expected);
        auto Jpeg = QByteArray::fromHex("ffd8ffe1"); const auto Length = Tiff.size()+8; Jpeg += char(Length>>8); Jpeg += char(Length&255); Jpeg += QByteArray("Exif\0\0",6); Jpeg += Tiff;
        QCOMPARE(ExifDate(Jpeg), Expected);
        const auto Big = QByteArray::fromHex("4d4d002a00000008000187690004000000010000001a00000000000190030002000000140000002c00000000") + QByteArray("2024:02:29 12:34:56\0",20);
        QCOMPARE(ExifDate(Big), Expected);
        for (int Size = 0; Size < Tiff.size(); ++Size) QVERIFY(!ExifDate(Tiff.left(Size)).isValid());
        auto Bad = Tiff; Bad[34] = char(0xff); QVERIFY(!ExifDate(Bad).isValid());
        Bad = Tiff; Bad.replace(44, 19, "2023:02:29 12:34:56"); QVERIFY(!ExifDate(Bad).isValid());
        QTemporaryDir Temp; QFile File(Temp.filePath("picture.jpg")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Jpeg); File.close(); QCOMPARE(ReadExifDate(File.fileName()), Expected);
        QVERIFY(!ReadExifDate(Temp.filePath("missing.jpg")).isValid());
    }
    void inputValidationAndKeyboard()
    {
        hc::ui::FileInputDialog Dialog({QStringLiteral("복사"), QStringLiteral("원본"), QStringLiteral("목적지"), {}, false});
        auto* Edit = Dialog.findChild<QLineEdit*>(QStringLiteral("hc.dialog.input.editor"));
        QVERIFY(Edit);
        Dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Dialog));
        Dialog.raise(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
        QSignalSpy Accepted(&Dialog, &QDialog::accepted);
        QSignalSpy Rejected(&Dialog, &QDialog::rejected);
        QVERIFY(!Dialog.AcceptButton()->isEnabled());
        QTest::keyClick(Edit, Qt::Key_Return);
        QCOMPARE(Accepted.count(), 0);
        Edit->setText(QStringLiteral("   "));
        QVERIFY(!Dialog.AcceptButton()->isEnabled());
        const QString Path = QStringLiteral("D:/fixture/<literal>&name");
        Edit->setText(Path);
        QCOMPARE(Dialog.Input(), Path);
        QVERIFY(Dialog.AcceptButton()->isEnabled());
        Edit->setFocus();
        QTest::keyClick(Edit, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), Dialog.AcceptButton());
        QTest::keyClick(Dialog.AcceptButton(), Qt::Key_Return);
        QCOMPARE(Accepted.count(), 1);
        Dialog.show();
        QTest::keyClick(&Dialog, Qt::Key_Escape);
        QCOMPARE(Rejected.count(), 1);
    }
    void relativeDestinationsAndPrimaryInput()
    {
        using namespace hc::ui;
        QCOMPARE(ResolveNativeDestination(".", "D:/source"), QString("D:/source"));
        QCOMPARE(ResolveNativeDestination(".\\child", "D:/source"), QString("D:/source/child"));
        QCOMPARE(ResolveNativeDestination("../other", "D:/source"), QString("D:/other"));
        QCOMPARE(ResolveNativeDestination(".hidden", "D:/source"), QString("D:/source/.hidden"));
        QCOMPARE(ResolveNativeDestination("../../..", "D:/source"), QString("D:/"));
        QCOMPARE(ResolveNativeDestination("../child", "//server/share/source"), QString("//server/share/child"));
        QCOMPARE(ResolveNativeDestination("../../..", "//server/share/source"), QString("//server/share"));
        QCOMPARE(ResolveNativeDestination("./child", "//?/UNC/server/share/source"), QString("//server/share/source/child"));
        QCOMPARE(ResolveNativeDestination("./child", "//?/D:/source"), QString("D:/source/child"));
        for (const auto& Invalid : {"", " ", "C:relative", "/root-relative", "//server", "bad?name", "CON.txt", "name ", "name."})
            QVERIFY2(ResolveNativeDestination(Invalid, "D:/source").isEmpty(), Invalid);
        QVERIFY(ResolveNativeDestination("./child", "relative-base").isEmpty());
        ArchiveExtractDialog Extract({"sample.zip"}, "D:/other", false, nullptr, "D:/source");
        auto* Path = Extract.findChild<QLineEdit*>("hc.archive.extract.path"); QVERIFY(Path);
        Extract.show(); QVERIFY(QTest::qWaitForWindowExposed(&Extract)); Extract.raise(); Extract.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Extract));
        QTRY_COMPARE(QApplication::focusWidget(), Path); QCOMPARE(Path->selectedText(), QString("D:/other"));
        QSignalSpy Accepted(&Extract, &QDialog::accepted);
        Path->clear(); QTest::keyClick(Path, Qt::Key_Return); QCOMPARE(Accepted.count(), 0); QVERIFY(Extract.isVisible());
        Path->setText("."); QCOMPARE(Extract.Destination(), QString("D:/source")); QVERIFY(Extract.AcceptButton()->isEnabled());
        QTest::keyClick(Path, Qt::Key_Return); QCOMPARE(Accepted.count(), 1);
        Extract.show(); QVERIFY(QTest::qWaitForWindowExposed(&Extract));
        Path->setText("../other"); QTest::keyClick(Path, Qt::Key_Enter, Qt::KeypadModifier); QCOMPARE(Accepted.count(), 2);
        for (const auto& Command : {"hc.file.copy", "hc.file.move", "hc.file.rename", "hc.file.mkdir"}) {
            FileOperationDialogRequest Request; Request.command_=Command; Request.source_="D:/source"; Request.initialValue_=QString(Command).endsWith("rename") ? "sample.txt" : "D:/other";
            Request.names_={"sample.txt"}; Request.nativeDestination_=true;
            FileOperationDialog Dialog(Request); auto* Edit=Dialog.findChild<QLineEdit*>("hc.operation.editor"); QVERIFY(Edit);
            Dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&Dialog)); Dialog.raise(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
            QTRY_COMPARE(QApplication::focusWidget(), Edit);
            Edit->setText(QString(Command).endsWith("copy") ? "." : "../other/child.txt");
            QTRY_VERIFY(Dialog.AcceptButton()->isEnabled());
            QCOMPARE(QDir::fromNativeSeparators(Dialog.ResolvedTarget()), QString(Command).endsWith("copy") ? QString("D:/source") : QString("D:/other/child.txt"));
            QSignalSpy Completed(&Dialog, &QDialog::accepted); QTest::keyClick(Edit, Qt::Key_Return); QCOMPARE(Completed.count(), 1);
        }
    }
    void compactOperationLayouts()
    {
        using namespace hc::ui;
        const auto Original=qApp->font(); const auto Restore=qScopeGuard([&]{qApp->setFont(Original);});
        auto& Theme=fm::style::ThemeManager::instance(); Theme.setDesign(fm::style::Design::Watercolor);
        if(!Theme.isInstalled()) Theme.install(*qApp);
        QTemporaryDir Root; QVERIFY(Root.isValid());
        for(const bool Dark:{false,true}) for(const int Points:{11,18}) {
            Theme.setScheme(Dark?fm::style::ThemeManager::Scheme::Dark:fm::style::ThemeManager::Scheme::Light);
            auto Font=Original; Font.setPointSize(Points); qApp->setFont(Font);
            for(const auto& Kind:{"copy","move","rename","delete"}) {
                FileOperationDialogRequest Request; Request.command_="hc.file."+QString::fromLatin1(Kind); Request.source_=Root.path();
                Request.names_={"sample.txt"}; Request.initialValue_=QString(Kind)=="rename"?QString("sample.txt"):Root.path();
                Request.nativeSource_=Request.nativeDestination_=true;
                FileOperationDialog Dialog(Request); Dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&Dialog));
                auto* Scroll=Dialog.findChild<QScrollArea*>(); QVERIFY(Scroll);
                QTest::qWait(100);
                qInfo()<<Kind<<Dark<<Points<<Dialog.size()<<Scroll->size()<<Scroll->widget()->minimumSizeHint()<<Scroll->widget()->sizeHint();
                const auto Artifacts=qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
                if(!Artifacts.isEmpty()) { QDir().mkpath(Artifacts); QVERIFY(Dialog.grab().save(QDir(Artifacts).filePath(QString("compact-%1-%2-%3.png").arg(Kind).arg(Dark).arg(Points)))); }
                QTRY_COMPARE(Scroll->verticalScrollBar()->maximum(),0);
                for(auto* Button:{Dialog.AcceptButton(),Dialog.CancelButton()}) QVERIFY(Dialog.rect().contains(QRect(Button->mapTo(&Dialog,QPoint()),Button->size())));
                if(QString(Kind)=="copy") {
                    auto* Tabs=Dialog.findChild<QTabWidget*>("hc.operation.advancedTabs"); QVERIFY(Tabs && !Tabs->isVisible());
                    auto* Toggle=Dialog.findChild<QPushButton*>("hc.operation.advanced"); QVERIFY(Toggle);
                    auto* Pattern=Dialog.findChild<QLineEdit*>("hc.operation.filter"); auto* Speed=Dialog.findChild<QSpinBox*>("hc.operation.speed");
                    const auto EditOptions=[&] { Pattern->setText("*.txt"); Speed->setValue(32); Dialog.findChild<QCheckBox*>("hc.operation.verify")->setChecked(true); };
                    bool Separate=false;
                    QTimer::singleShot(0,&Dialog,[&] { if(auto* Editor=Dialog.findChild<QDialog*>("hc.operation.advancedDialog")) { Separate=true; EditOptions(); Editor->accept(); } });
                    Toggle->click(); QCoreApplication::processEvents();
                    if(!Separate) { QTRY_VERIFY(Tabs->isVisible()); EditOptions(); }
                    auto* Summary=Dialog.findChild<QLabel*>("hc.operation.optionSummary"); QVERIFY(Summary);
                    QVERIFY(Summary->text().contains("*.txt")&&Summary->text().contains("32 MiB/s")&&Summary->text().contains("SHA-256"));
                    if(!Separate) Toggle->click(); QTRY_VERIFY(!Tabs->isVisible());
                    const auto Before=Dialog.IncludePattern(); Dialog.setMaximumHeight(Dialog.height()+10);
                    bool Opened=false;
                    QTimer::singleShot(0,&Dialog,[&] { if(auto* Editor=Dialog.findChild<QDialog*>("hc.operation.advancedDialog")) { Opened=true; Pattern->setText("*.bin"); Editor->reject(); } });
                    Toggle->click(); QVERIFY(Opened); QCOMPARE(Dialog.IncludePattern(),Before);
                }
                Dialog.reject();
            }
            for(const bool NewFile:{false,true}) {
                std::unique_ptr<DialogShell> Dialog;
                if(NewFile) Dialog=std::make_unique<NewFileDialog>(Root.path(),true);
                else Dialog=std::make_unique<ArchiveExtractDialog>(QStringList{"sample.zip","other.zip"},Root.path(),false);
                Dialog->show(); QVERIFY(QTest::qWaitForWindowExposed(Dialog.get())); QTest::qWait(50);
                auto* Scroll=Dialog->findChild<QScrollArea*>(); QVERIFY(Scroll); QTRY_COMPARE(Scroll->verticalScrollBar()->maximum(),0);
                if(NewFile) {
                    auto* Type=Dialog->findChild<QComboBox*>("hc.newFile.type"); QVERIFY(Type); QCOMPARE(Type->count(),8); Type->setCurrentIndex(6);
                    auto* Open=Dialog->findChild<QCheckBox*>("hc.newFile.open"); QVERIFY(Open); Open->setChecked(true); QTRY_COMPARE(Scroll->verticalScrollBar()->maximum(),0);
                }
                const auto Artifacts=qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
                if(!Artifacts.isEmpty()) QVERIFY(Dialog->grab().save(QDir(Artifacts).filePath(QString("compact-%1-%2-%3.png").arg(NewFile?"new-file":"extract").arg(Dark).arg(Points))));
                Dialog->reject();
            }
        }
    }
    void automaticTransferTargets()
    {
        using namespace hc::ui;
        QTemporaryDir Root; QVERIFY(Root.isValid()); QVERIFY(QDir(Root.path()).mkdir("existing")); QVERIFY(QDir(Root.path()).mkdir("source-dir"));
        for(const auto& Name:{"source.txt","target-file"}) { QFile File(Root.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("fixture"); }
        struct Case { const char* input_; const char* mode_; int count_; bool name_,parents_,valid_; bool sourceDirectory_=false; };
        const Case Cases[]{
            {".\\existing","auto",1,false,false,true}, {".\\target-file","auto",1,true,false,true},
            {".\\f","auto",1,true,false,true}, {".\\f\\","auto",1,false,true,true}, {"./f/","auto",1,false,true,true},
            {".\\f","auto",2,false,true,true}, {".\\f","folder",1,false,true,true}, {".\\f","name",1,true,false,true},
            {".\\f\\","name",1,false,true,true}, {".\\existing","name",1,true,false,false},
            {".\\target-file","folder",1,false,true,false}, {".\\target-file\\","auto",1,false,true,false},
            {".\\한글 폴더\\","auto",1,false,true,true}, {".\\new.name","auto",1,true,false,true},
            {".\\target-file","auto",1,true,false,false,true}, {".\\new-folder","auto",1,true,false,true,true}
        };
        for(const auto& Case:Cases) {
            FileOperationDialogRequest Request; Request.command_="hc.file.copy"; Request.source_=Root.path(); Request.nativeDestination_=Request.nativeSource_=true;
            Request.initialValue_=QString::fromUtf8(Case.input_); Request.names_={Case.sourceDirectory_?QString("source-dir"):QString("source.txt")}; if(Case.count_>1)Request.names_.append("second.txt");
            FileOperationDialog Dialog(Request); Dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&Dialog));
            auto* Mode=Dialog.findChild<QComboBox*>("hc.operation.targetMode"); QVERIFY(Mode); QCOMPARE(Mode->count(),3); Mode->setCurrentIndex(Mode->findData(Case.mode_));
            QTRY_COMPARE(Dialog.findChild<QLabel*>("hc.operation.targetSummary")->property("target").toString(),Dialog.ResolvedTarget());
            QCOMPARE(Dialog.TargetIsName(),Case.name_); QCOMPARE(Dialog.CreateParents(),Case.parents_); QCOMPARE(Dialog.AcceptButton()->isEnabled(),Case.valid_);
            QVERIFY(Dialog.findChild<QLabel*>("hc.operation.preview")->text().contains(QDir::toNativeSeparators(Root.path())));
            if(Case.count_>1) QVERIFY(!(Mode->model()->flags(Mode->model()->index(2,0))&Qt::ItemIsEnabled));
            if(Case.valid_ && Case.count_==1 && QString(Case.mode_)=="auto" && QString::fromUtf8(Case.input_)==".\\f") {
                const auto Out=qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if(!Out.isEmpty()) QVERIFY(Dialog.grab().save(QDir(Out).filePath("target-auto-new-name.png")));
            }
            Dialog.reject();
        }
        FileOperationDialogRequest Request; Request.command_="hc.file.copy"; Request.source_=Root.path(); Request.nativeDestination_=true; Request.names_={"source.txt"}; Request.initialValue_=Root.filePath("existing");
        FileOperationDialog Dialog(Request); auto* Edit=Dialog.findChild<QLineEdit*>("hc.operation.editor");
        QTRY_VERIFY(Dialog.AcceptButton()->isEnabled()); QVERIFY(!Dialog.TargetIsName()); Edit->setText("./pending/"); QTest::qWait(160);
        QVERIFY(QDir(Root.path()).rmdir("existing")); { QFile File(Root.filePath("existing")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("directory replaced by file"); }
        Edit->setText(Root.filePath("existing"));
        QTRY_VERIFY(Dialog.AcceptButton()->isEnabled()); QTRY_VERIFY(Dialog.TargetIsName()); QTest::qWait(200); QVERIFY(Dialog.TargetIsName()); QVERIFY(Dialog.AcceptButton()->isEnabled());
    }
    void deleteConfirmationKeyboard_data()
    {
        QTest::addColumn<QString>("Mode");
        QTest::addColumn<int>("Key");
        for (const auto& Mode : {"permanent", "recycle"}) {
            QTest::newRow(qPrintable(QString(Mode) + "-return")) << QString(Mode) << int(Qt::Key_Return);
            QTest::newRow(qPrintable(QString(Mode) + "-keypad-enter")) << QString(Mode) << int(Qt::Key_Enter);
            QTest::newRow(qPrintable(QString(Mode) + "-escape")) << QString(Mode) << int(Qt::Key_Escape);
        }
        QTest::newRow("unavailable-return") << QString("unavailable") << int(Qt::Key_Return);
        QTest::newRow("unavailable-keypad-enter") << QString("unavailable") << int(Qt::Key_Enter);
    }
    void deleteConfirmationKeyboard()
    {
        QFETCH(QString, Mode);
        QFETCH(int, Key);
        QTemporaryDir Temp;
        QVERIFY(Temp.isValid());
        QFile File(Temp.filePath("sample.txt"));
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("keyboard confirmation fixture"); File.close();
        const bool Available = Mode != "unavailable";
        hc::ui::FileOperationDialogRequest Request;
        Request.command_ = "hc.file.delete"; Request.source_ = Temp.path(); Request.names_ = {"sample.txt"};
        Request.nativeSource_ = Available;
        hc::ui::FileOperationDialog Dialog(Request);
        Dialog.findChild<QRadioButton*>("hc.operation.permanent")->setChecked(Mode == "permanent");
        Dialog.findChild<QRadioButton*>("hc.operation.recycle")->setChecked(Mode != "permanent");
        QSignalSpy Accepted(&Dialog, &QDialog::accepted), Rejected(&Dialog, &QDialog::rejected);
        Dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Dialog));
        if (Available) QTRY_VERIFY(Dialog.AcceptButton()->isEnabled());
        else QVERIFY(!Dialog.AcceptButton()->isEnabled());
        // Exercise the initial keyboard focus, without clicking the action button.
        QTest::keyClick(Dialog.focusWidget() ? Dialog.focusWidget() : &Dialog, Qt::Key(Key),
            Key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier);
        QCOMPARE(Accepted.count(), Available && Key != Qt::Key_Escape ? 1 : 0);
        QCOMPARE(Rejected.count(), Key == Qt::Key_Escape ? 1 : 0);
        if (!Available) {
            QVERIFY(Dialog.isVisible());
            QTest::keyClick(&Dialog, Qt::Key_Escape);
            QCOMPARE(Rejected.count(), 1);
        }
        QVERIFY(QFileInfo::exists(File.fileName()));
    }
    void operationValidation()
    {
        hc::ui::FileOperationDialog Rename({"hc.file.rename", "D:/source", "report.pdf", {"report.pdf"}, true});
        auto* Edit = Rename.findChild<QLineEdit*>("hc.operation.editor");
        QVERIFY(!Rename.AcceptButton()->isEnabled());
        for (const auto& Name : {"CON.txt", "a/b", "..", "name.", "name ", "x:bad"}) {
            Edit->setText(Name); QVERIFY(!Rename.AcceptButton()->isEnabled());
        }
        Edit->setText("report.txt"); QVERIFY(!Rename.AcceptButton()->isEnabled());
        for (auto* Check : Rename.findChildren<QCheckBox*>())
            if (Check->isEnabled()) Check->setChecked(true);
        QVERIFY(Rename.AcceptButton()->isEnabled());
        Edit->setText("new.pdf"); QVERIFY(Rename.AcceptButton()->isEnabled());
        hc::ui::FileOperationDialog Copy({"hc.file.copy", "D:/source", "D:/target", {"a.txt"}, true});
        auto* Policy = Copy.findChild<QComboBox*>("hc.operation.conflict");
        QCOMPARE(Copy.Conflict(), QStringLiteral("ask"));
        Policy->setCurrentIndex(1); QCOMPARE(Copy.Conflict(), QStringLiteral("replace"));
        Policy->setCurrentIndex(2); QCOMPARE(Copy.Conflict(), QStringLiteral("skip"));
        Copy.findChild<QLineEdit*>("hc.operation.editor")->setText("relative");
        QTRY_VERIFY(Copy.AcceptButton()->isEnabled());
        QCOMPARE(Copy.ResolvedTarget(), QString("D:\\source\\relative"));
        hc::ui::FileOperationDialog Delete({"hc.file.delete", "D:/source", {}, {"a.txt"}, true});
        QVERIFY(!Delete.AcceptButton()->isEnabled());
        QSignalSpy Accepted(&Delete, &QDialog::accepted);
        Delete.AcceptButton()->click(); QCOMPARE(Accepted.count(), 0);
        Delete.findChild<QRadioButton*>("hc.operation.permanent")->setChecked(true);
        QTRY_VERIFY(Delete.AcceptButton()->isEnabled());
        QCOMPARE(Delete.AcceptButton()->role(), fm::ui::Button::Danger);
        Delete.reject(); QCOMPARE(Accepted.count(), 0);
        hc::ui::FileOperationDialog Folder({"hc.file.mkdir", "D:/source", "new", {}, true});
        QVERIFY(Folder.AcceptButton()->isEnabled());
        Folder.findChild<QLineEdit*>("hc.operation.editor")->setText("a/b");
        QVERIFY(Folder.AcceptButton()->isEnabled());
        QVERIFY(Folder.CreateParents());
        Folder.findChild<QLineEdit*>("hc.operation.editor")->setText("../escape");
        QVERIFY(Folder.AcceptButton()->isEnabled());
        QCOMPARE(Folder.ResolvedTarget(), QString("D:\\escape"));
    }
    void operationVisuals_data() { visualContract_data(); }
    void operationVisuals()
    {
        QFETCH(bool, Dark);
        auto& Theme = fm::style::ThemeManager::instance();
        Theme.setDesign(fm::style::Design::Watercolor);
        Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        if (!Theme.isInstalled()) Theme.install(*qApp);
        for (const auto& Kind : {"copy", "move", "rename", "delete", "mkdir"}) {
            const auto Name = QString::fromLatin1(Kind);
            hc::ui::FileOperationDialog Dialog({"hc.file." + Name, "D:/fixture/source",
                Name == "rename" ? "sample.txt" : Name == "mkdir" ? "new-folder" : "D:/fixture/target",
                {"sample.txt", "report.csv", "subfolder"}, true});
            Dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&Dialog));
            QCoreApplication::processEvents();
            const auto Save = [&](const QString& State) {
                const auto Directory = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
                if (Directory.isEmpty()) return;
                QDir().mkpath(Directory);
                QVERIFY(Dialog.grab().save(QDir(Directory).filePath(QStringLiteral("p2-%1-%2-%3-%4.png")
                    .arg(Name, Dark ? "dark" : "light", QString::number(Dialog.devicePixelRatioF()), State))));
            };
            Save("normal");
            if (Name == "delete") {
                Dialog.findChild<QRadioButton*>("hc.operation.permanent")->setChecked(true);
                QCoreApplication::processEvents(); Save("permanent");
            }
            Dialog.resize(Dialog.minimumSize()); QCoreApplication::processEvents();
            for (auto* Button : {Dialog.AcceptButton(), Dialog.CancelButton()})
                QVERIFY(Dialog.rect().contains(QRect(Button->mapTo(&Dialog, QPoint()), Button->size())));
            Save("minimum");
            QSignalSpy Rejected(&Dialog, &QDialog::rejected);
            QTest::keyClick(&Dialog, Qt::Key_Escape); QCOMPARE(Rejected.count(), 1);
        }
    }
    void fileColorRules()
    {
        using hc::settings::FileColorRules;
        QCOMPARE(FileColorRules::Extensions(" .TXT, txt; CPP | cpp "), QStringList({"txt", "cpp"}));
        const QJsonObject A{{"id", "a"}, {"name", "문서"}, {"enabled", true}, {"join", "any"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", ".TXT,cpp,<none>"}}}},
            {"light", QJsonObject{{"foreground", "#123456"}}}, {"dark", QJsonObject{{"foreground", "#abcdef"}}}, {"font", QJsonObject{{"bold", false}}}};
        const QJsonObject B{{"id", "b"}, {"name", "이름 패턴"}, {"enabled", true}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "pattern"}, {"value", "*.txt"}}, QJsonObject{{"type", "folder"}, {"value", "false"}}}},
            {"light", QJsonObject{{"background", "#ffeeaa"}}}, {"font", QJsonObject{{"bold", true}, {"italic", true}}}};
        auto Doc = FileColorRules::Empty(); Doc["groups"] = QJsonArray{A, B};
        QVERIFY(FileColorRules::Validate(Doc).isEmpty()); FileColorRules Rules; Rules.Load(Doc);
        auto R = Rules.Evaluate("SAMPLE.TXT", 0, 20, false); QCOMPARE(R.foreground_, QColor("#123456")); QVERIFY(!R.background_.isValid());
        QVERIFY(!Rules.Evaluate("sample.txt", 0x10, 0, false).matched_);
        QVERIFY(Rules.Evaluate("README", 0, 0, false).matched_); QVERIFY(!Rules.Evaluate("archive.tar.gz", 0, 0, false).matched_);
        QCOMPARE(Rules.Evaluate("sample.txt", 0, 0, true).foreground_, QColor("#abcdef"));
        Doc["mode"] = "merge"; Rules.Load(Doc); R = Rules.Evaluate("sample.txt", 0, 0, false);
        QCOMPARE(R.background_, QColor("#ffeeaa")); QVERIFY(!R.Font(QFont()).bold()); QVERIFY(R.Font(QFont()).italic()); QCOMPARE(R.owners_["bold"], QStringLiteral("문서"));
        auto Renamed = A; Renamed["name"] = "새 이름"; Doc["groups"] = QJsonArray{Renamed, B}; Rules.Load(Doc);
        QCOMPARE(Rules.Evaluate("sample.txt", 0, 0, false).foreground_, QColor("#123456")); QCOMPARE(Renamed["id"], A["id"]);
        Renamed["enabled"] = false; Doc["groups"] = QJsonArray{Renamed, B}; Rules.Load(Doc);
        QVERIFY(!Rules.Evaluate("sample.txt", 0, 0, false).foreground_.isValid());
        Doc["groups"] = QJsonArray{A, A}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
        auto Invalid = A; Invalid["conditions"] = QJsonArray{QJsonObject{{"type", "extension"}, {"value", "*.txt"}}}; Doc["groups"] = QJsonArray{Invalid}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
        Invalid["conditions"] = QJsonArray{}; Doc["groups"] = QJsonArray{Invalid}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
        QTemporaryDir Temp; hc::settings::Repository Store(Temp.path()); auto Options = Store.Read(hc::settings::Section::Options).toObject();
        Doc["groups"] = QJsonArray{A, B}; Options["fileColorRules"] = Doc; QVERIFY(Store.Put(hc::settings::Section::Options, Options));
        hc::settings::Repository Reload(Temp.path()); QCOMPARE(Reload.Read(hc::settings::Section::Options).toObject()["fileColorRules"].toObject(), Doc);
    }
    void fileColorAutomaticDark()
    {
        using namespace hc::settings;
        QJsonObject G{{"id", "auto"}, {"name", "자동"}, {"join", "any"}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#80123456"}, {"background", "#ffeeaa"}}}, {"dark", QJsonObject{{"foreground", "#abcdef"}, {"future", 7}}}, {"font", QJsonObject{{"bold", false}}}};
        const QJsonValue Manual = G["dark"]; G["autoDark"] = true;
        auto Doc = FileColorRules::Empty(); Doc["groups"] = QJsonArray{G}; Doc = FileColorRules::Upgrade(Doc);
        FileColorRules Rules; Rules.Load(Doc); const auto Dark = Rules.Evaluate("sample.txt", 0, 1, true);
        QCOMPARE(Dark.foreground_.alpha(), 128); QVERIFY(Dark.foreground_.lightnessF() >= 0.64); QVERIFY(Dark.background_.lightnessF() <= 0.23);
        QCOMPARE(Rules.Evaluate("sample.txt", 0, 1, false).foreground_, QColor("#80123456")); QVERIFY(!Dark.Font(QFont()).bold());
        QCOMPARE(G["dark"], Manual); G["autoDark"] = false; Doc["groups"] = QJsonArray{G}; Doc["version"] = 1; Rules.Load(Doc);
        QCOMPARE(Rules.Evaluate("sample.txt", 0, 1, true).foreground_, QColor("#abcdef"));
        G["autoDark"] = true; G["light"] = QJsonObject{}; QVERIFY(FileColorRules::Colors(G, true).isEmpty());
        G["autoDark"] = "invalid"; Doc["groups"] = QJsonArray{G}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
    }
    void fileColorConditionsV2()
    {
        using namespace hc::settings;
        const auto Document = [](const QJsonObject& C) { auto D = FileColorRules::Empty(); D["version"] = 2;
            D["groups"] = QJsonArray{QJsonObject{{"id", "criterion"}, {"name", "조건"}, {"enabled", true}, {"join", "all"}, {"conditions", QJsonArray{C}}}}; return D; };
        FileColorRules Rules;
        QVERIFY(!FileColorRules::MetadataTime(0)); QVERIFY(!FileColorRules::MetadataTime(-11644473600000LL));
        const auto Load = [&](const QJsonObject& C) { const auto D = Document(C); QVERIFY2(FileColorRules::Validate(D).isEmpty(), qPrintable(FileColorRules::Validate(D))); Rules.Load(D); };
        Load({{"type", "pattern"}, {"op", "matches"}, {"value", QJsonArray{"CMakeLists.txt", "*.cmake"}}});
        QVERIFY(Rules.Evaluate("CMakeLists.txt", 0, 0, false).matched_); QVERIFY(Rules.Evaluate("example.CMAKE", 0, 0, false).matched_); QVERIFY(!Rules.Evaluate("notes.txt", 0, 0, false).matched_);
        for (const auto& Op : {"ge", "le", "gt", "lt", "eq", "ne"}) {
            Load({{"type", "size"}, {"op", Op}, {"value", "1"}, {"unit", "MiB"}});
            const bool Inclusive = QStringList{"ge", "le", "eq"}.contains(Op);
            QCOMPARE(Rules.Evaluate("boundary.dat", 0, 1024 * 1024, false).matched_, Inclusive);
            QVERIFY(!Rules.Evaluate("folder.dat", 0x10, 1024 * 1024, false).matched_);
        }
        Load({{"type", "size"}, {"op", "eq"}, {"value", "0"}, {"unit", "B"}}); QVERIFY(Rules.Evaluate("empty.txt", 0, 0, false).matched_); QVERIFY(!Rules.Evaluate("data.txt", 0, 1, false).matched_);
        Load({{"type", "attributes"}, {"op", "any"}, {"value", "6"}});
        QVERIFY(Rules.Evaluate("hidden", 2, 0, false).matched_); QVERIFY(Rules.Evaluate("system", 4, 0, false).matched_); QVERIFY(!Rules.Evaluate("archive", 32, 0, false).matched_);
        Load({{"type", "attributes"}, {"op", "all"}, {"value", "6"}}); QVERIFY(!Rules.Evaluate("hidden", 2, 0, false).matched_); QVERIFY(Rules.Evaluate("both", 6, 0, false).matched_);
        Load({{"type", "modified"}, {"op", "within"}, {"value", "24"}, {"unit", "hours"}}); QVERIFY(Rules.HasRelativeDates());
        FileRuleInput Input{"recent.txt", 0, 0, false}; Input.referenceUtcMs_ = QDateTime::fromString("2026-10-13T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
        QVERIFY(!Rules.Evaluate(Input).matched_); // Unknown is never a fabricated epoch date.
        Input.modifiedUtcMs_ = Input.referenceUtcMs_ - 86400000; QVERIFY(Rules.Evaluate(Input).matched_); QCOMPARE(Rules.Evaluate(Input).validUntilUtcMs_, std::optional<qint64>(Input.referenceUtcMs_ + 1));
        --*Input.modifiedUtcMs_; QVERIFY(!Rules.Evaluate(Input).matched_); QVERIFY(!Rules.Evaluate(Input).validUntilUtcMs_);
        Input.modifiedUtcMs_ = Input.referenceUtcMs_ + 1; QVERIFY(!Rules.Evaluate(Input).matched_);
        Input.modifiedUtcMs_ = Input.referenceUtcMs_; QVERIFY(Rules.Evaluate(Input).matched_);
        Load({{"type", "modified"}, {"op", "before"}, {"value", "2026-10-13T21:00:00+09:00"}}); QVERIFY(!Rules.HasRelativeDates());
        QVERIFY(!Rules.Evaluate(Input).matched_); --*Input.modifiedUtcMs_; QVERIFY(Rules.Evaluate(Input).matched_);
        Input.modifiedUtcMs_.reset(); QVERIFY(!Rules.Evaluate(Input).matched_);
        Load({{"type", "link"}, {"op", "is"}, {"value", "any"}});
        QVERIFY(Rules.Evaluate("launcher.LNK", 0, 0, false).matched_); QVERIFY(Rules.Evaluate("website.url", 0, 0, false).matched_); QVERIFY(!Rules.Evaluate("folder.lnk", 0x10, 0, false).matched_);
        Input = FileRuleInput{"junction", 0x410, 0, false}; QVERIFY(!Rules.Evaluate(Input).matched_);
        Input.reparseTag_ = 0xa0000003u; QVERIFY(Rules.Evaluate(Input).matched_);
        Input.reparseTag_ = 0xa000000cu; QVERIFY(Rules.Evaluate(Input).matched_);
        Input.reparseTag_ = 0x9000001au; QVERIFY(!Rules.Evaluate(Input).matched_); // A cloud tag is not a symbolic link.
        Load({{"type", "link"}, {"op", "is"}, {"value", "none"}}); Input.reparseTag_.reset(); QVERIFY(!Rules.Evaluate(Input).matched_);
        const auto Overflow = Document({{"type", "size"}, {"op", "ge"}, {"value", "18446744073709551615"}, {"unit", "GiB"}}); QVERIFY(!FileColorRules::Validate(Overflow).isEmpty());
        QVERIFY(!FileColorRules::Validate(Document({{"type", "modified"}, {"op", "before"}, {"value", "2026-10-13T12:00:00"}})).isEmpty());
    }
    void fileColorNormalization()
    {
        using namespace hc::settings;
        const auto Document = [](const QString& Value, const QString& Op = "is") {
            auto D = FileColorRules::Empty(); D["version"] = 2;
            const QJsonObject Condition{{"type", "normalization"}, {"op", Op}, {"value", Value}};
            const QJsonObject Group{{"id", "unicode"}, {"name", "정규화"}, {"join", "all"}, {"conditions", QJsonArray{Condition}}};
            D["groups"] = QJsonArray{Group}; return D;
        };
        const auto Nfc = QStringLiteral("한글.txt"); const auto Nfd = Nfc.normalized(QString::NormalizationForm_D);
        const auto Mixed = QStringLiteral("한글-") + QStringLiteral("이름").normalized(QString::NormalizationForm_D) + ".txt";
        const QList<QPair<QString, bool>> Cases{{Nfc, false}, {Nfd, true}, {Mixed, true}, {"ASCII-123.txt", false},
            {QStringLiteral("café.txt"), false}, {QStringLiteral("café.txt").normalized(QString::NormalizationForm_D), true},
            {QStringLiteral("ㅎㅏㄴ.txt"), false}}; // Compatibility jamo are not canonical decomposition.
        for (const auto* Mode : {"decomposed", "composed"}) {
            const auto D = Document(Mode); QVERIFY(FileColorRules::Validate(D).isEmpty()); FileColorRules Rules; Rules.Load(D);
            for (const auto& Item : Cases) for (const bool Folder : {false, true}) {
                const auto Original = Item.first;
                QCOMPARE(FileColorRules::HasDecomposedName(Original), Item.second);
                QCOMPARE(Rules.Evaluate(Original, Folder ? 0x10 : 0, 0, false).matched_, Item.second == (QString(Mode) == "decomposed"));
                QCOMPARE(Item.first, Original);
            }
            QVERIFY(!Rules.Evaluate("..", 0x10, 0, false).matched_);
        }
        QVERIFY(!FileColorRules::Validate(Document("unknown")).isEmpty());
        QVERIFY(!FileColorRules::Validate(Document("decomposed", "not")).isEmpty());
        QTemporaryDir Temp; Repository Store(Temp.path()); auto Options = Store.Read(Section::Options).toObject();
        const auto D = Document("decomposed"); Options["fileColorRules"] = D;
        QVERIFY(Store.Put(Section::Options, Options)); QVERIFY(Store.Commit()); Repository Reload(Temp.path());
        QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject(), D);
    }

    void fileColorMigrationV2()
    {
        using namespace hc::settings;
        auto Old = FileColorRules::Empty(); QJsonArray Groups;
        const QList<QJsonObject> Conditions{{{"type", "pattern"}, {"value", "literal;*.txt"}, {"future", "preserve"}}, {{"type", "sizeMore"}, {"value", "10"}}, {{"type", "sizeLess"}, {"value", "10"}}, {{"type", "sizeEqual"}, {"value", "0"}}, {{"type", "attributes"}, {"value", "6"}}};
        for (int I = 0; I < Conditions.size(); ++I) Groups.append(QJsonObject{{"id", QString::number(I)}, {"name", QString("기존 %1").arg(I)}, {"join", "all"}, {"conditions", QJsonArray{Conditions[I]}}, {"font", QJsonObject{{"bold", false}}}, {"futureGroup", "preserve"}});
        Old["groups"] = Groups; Old["mode"] = "merge"; Old["futureDocument"] = "preserve";
        const auto New = FileColorRules::Upgrade(Old); QCOMPARE(New["version"], QJsonValue(2)); QVERIFY(FileColorRules::Validate(New).isEmpty());
        QCOMPARE(New["futureDocument"], Old["futureDocument"]); QCOMPARE(New["groups"].toArray()[0].toObject()["futureGroup"], QJsonValue("preserve"));
        const auto Pattern = New["groups"].toArray()[0].toObject()["conditions"].toArray()[0].toObject(); QCOMPARE(Pattern["value"].toArray(), QJsonArray{"literal;*.txt"}); QCOMPARE(Pattern["future"], QJsonValue("preserve"));
        FileColorRules Legacy, Upgraded; Legacy.Load(Old); Upgraded.Load(New);
        for (const auto& Name : {"literal;a.txt", "a.txt", "literal;test.bin"}) for (quint64 Size : {0ull, 9ull, 10ull, 11ull}) for (quint32 Attributes : {0u, 2u, 6u, 0x10u}) {
            const auto A = Legacy.Evaluate(Name, Attributes, Size, false), B = Upgraded.Evaluate(Name, Attributes, Size, false);
            QCOMPARE(A.groups_, B.groups_); QCOMPARE(A.font_, B.font_); QCOMPARE(A.owners_, B.owners_);
        }
        QCOMPARE(FileColorRules::Upgrade(New), New);
        const QJsonObject Opaque{{"type", "future-condition"}, {"value", "64"}, {"future", "preserve"}};
        QCOMPARE(FileColorRules::NormalizeCondition(Opaque, 1), Opaque);
        auto Future = New; Future["version"] = 99; QCOMPARE(FileColorRules::Upgrade(Future), Future); QVERIFY(!FileColorRules::Supported(Future));
        QTemporaryDir Temp; Repository Store(Temp.path()); auto Options = Store.Read(Section::Options).toObject(); Options["fileColorRules"] = New;
        QVERIFY(Store.Put(Section::Options, Options)); QVERIFY(Store.Commit()); Repository Reload(Temp.path()); QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject(), New);
    }
    void settingsDraftIsolation()
    {
        QTemporaryDir Temp;
        hc::settings::Repository Store(Temp.path());
        QVERIFY(Store.Commit());
        hc::ui::SettingsDraft Draft(Store);
        auto General = Draft.Read(hc::settings::Section::General).toObject();
        General["p1Fixture"] = true;
        Draft.Stage(hc::settings::Section::General, General);
        QVERIFY(Draft.Dirty());
        QVERIFY(!Store.Read(hc::settings::Section::General).toObject().contains("p1Fixture"));
        Draft.Discard();
        QVERIFY(!Draft.Dirty());
        Draft.Stage(hc::settings::Section::General, General);
        Store.BeginEdit();
        QVERIFY(!Draft.Apply());
        QCOMPARE(Draft.Error(), QStringLiteral("settings-editor-busy"));
        QVERIFY(Store.Editing());
        Store.DiscardEdit();
        QVERIFY(Draft.Apply());
        QVERIFY(!Store.Editing());
        QVERIFY(!Draft.Dirty());
        hc::settings::Repository Reopened(Temp.path());
        QVERIFY(Reopened.Read(hc::settings::Section::General).toObject()["p1Fixture"].toBool());
    }
    void settingsDraftWriteFailureAndConflict()
    {
        QTemporaryDir Temp;
        hc::settings::Repository Store(Temp.path());
        QVERIFY(Store.Commit());
        hc::ui::SettingsDraft Draft(Store);
        auto General = Draft.Read(hc::settings::Section::General).toObject();
        General["p1Fixture"] = true;
        Draft.Stage(hc::settings::Section::General, General);
        QLockFile Lock(Store.FileName() + QStringLiteral(".lock"));
        QVERIFY(Lock.tryLock());
        QVERIFY(!Draft.Apply());
        QVERIFY(Draft.Dirty());
        QVERIFY(!Store.Editing());
        QVERIFY(!Store.Read(hc::settings::Section::General).toObject().contains("p1Fixture"));
        Lock.unlock();
        QVERIFY(Draft.Apply());
        General["p1Fixture"] = false;
        Draft.Stage(hc::settings::Section::General, General);
        hc::settings::Repository Other(Temp.path());
        auto OtherGeneral = Other.Read(hc::settings::Section::General).toObject();
        OtherGeneral["externalFixture"] = true;
        QVERIFY(Other.Put(hc::settings::Section::General, OtherGeneral));
        QVERIFY(!Draft.Apply());
        QVERIFY(Draft.Error().endsWith(QStringLiteral(": changed-on-disk")));
        QVERIFY(Draft.Dirty());
        QVERIFY(Store.Read(hc::settings::Section::General).toObject()["externalFixture"].toBool());
    }
    void wrappingControlsKeepNativeBehavior()
    {
        QWidget Window; Window.resize(330, 400);
        auto* Layout = new fm::ui::WrapLayout(&Window);
        auto* First = new fm::ui::Button(QStringLiteral("첫 번째 작업 시작"), &Window);
        auto* Hidden = new fm::ui::Button(QStringLiteral("숨긴 응답"), &Window);
        auto* Last = new fm::ui::Button(QStringLiteral("마지막 작업 취소"), &Window);
        Layout->addWidget(First); Layout->addWidget(Hidden); Layout->addWidget(Last);
        Hidden->hide(); Window.show(); QCoreApplication::processEvents();
        Layout->setGeometry(Window.rect());
        QVERIFY(!First->geometry().intersects(Last->geometry()));
        QVERIFY(Window.rect().contains(First->geometry())); QVERIFY(Window.rect().contains(Last->geometry()));
        const auto Ltr = First->geometry(); Window.setLayoutDirection(Qt::RightToLeft); Layout->setGeometry(Window.rect());
        QCOMPARE(First->geometry(), QStyle::visualRect(Qt::RightToLeft, Window.rect(), Ltr));
        First->hide(); Last->hide(); QCOMPARE(Layout->heightForWidth(330), 0);
        fm::ui::WrapCheckBox Check(QStringLiteral("긴 파일 이름 <자료>&&를 포함한 해시 검증(&V)"));
        QFont Large = Check.font(); Large.setPointSize(18); Check.setFont(Large);
        Check.resize(160, Check.heightForWidth(160)); Check.show(); QCoreApplication::processEvents();
        QVERIFY(Check.height() > QCheckBox("짧은 문구").sizeHint().height());
        auto* Accessible = QAccessible::queryAccessibleInterface(&Check); QVERIFY(Accessible);
        QCOMPARE(Accessible->role(), QAccessible::CheckBox); QVERIFY(Accessible->text(QAccessible::Name).contains("<자료>&"));
        QTest::mouseClick(&Check, Qt::LeftButton, {}, QPoint(Check.width()-5, Check.height()-5)); QVERIFY(Check.isChecked());
        Check.setFocus(); QTest::keyClick(&Check, Qt::Key_Space); QVERIFY(!Check.isChecked());
        Check.setEnabled(false); QTest::mouseClick(&Check, Qt::LeftButton, {}, Check.rect().center()); QVERIFY(!Check.isChecked());
        Check.setLayoutDirection(Qt::RightToLeft); QVERIFY(Check.heightForWidth(160) > 0);
    }
    void visualContract_data()
    {
        QTest::addColumn<bool>("Dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void visualContract()
    {
        QFETCH(bool, Dark);
        auto& Theme = fm::style::ThemeManager::instance();
        Theme.setDesign(fm::style::Design::Watercolor);
        Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        if (!Theme.isInstalled()) Theme.install(*qApp);
        hc::ui::FileInputDialog Dialog({QStringLiteral("복사"), QStringLiteral("D:/fixture/source/sample.txt"),
            QStringLiteral("목적지 폴더"), QStringLiteral("D:/fixture/target"), true});
        Dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Dialog));
        QCoreApplication::processEvents();
        const auto Directory = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        const auto Scale = QString::number(Dialog.devicePixelRatioF());
        qInfo() << "Rendered device pixel ratio:" << Scale;
        const auto Prefix = QStringLiteral("p1-%1-%2").arg(Dark ? "dark" : "light", Scale);
        const auto Save = [&](const QString& State) {
            if (Directory.isEmpty()) return;
            QDir().mkpath(Directory);
            QVERIFY(Dialog.grab().save(QDir(Directory).filePath(Prefix + '-' + State + ".png")));
        };
        Save(QStringLiteral("normal"));
        QTest::mouseMove(Dialog.AcceptButton(), Dialog.AcceptButton()->rect().center());
        QCoreApplication::processEvents();
        Save(QStringLiteral("hover"));
        QTest::mousePress(Dialog.AcceptButton(), Qt::LeftButton);
        Save(QStringLiteral("pressed"));
        QTest::mouseRelease(Dialog.AcceptButton(), Qt::LeftButton);
        Dialog.show();
        Dialog.CancelButton()->setFocus();
        QCoreApplication::processEvents();
        Save(QStringLiteral("focus"));
        auto* Edit = Dialog.findChild<QLineEdit*>(QStringLiteral("hc.dialog.input.editor"));
        Edit->clear();
        QVERIFY(!Dialog.AcceptButton()->isEnabled());
        Save(QStringLiteral("disabled"));
        Dialog.resize(Dialog.minimumSize());
        QCoreApplication::processEvents();
        for (auto* Button : {Dialog.AcceptButton(), Dialog.CancelButton()}) {
            QVERIFY(Dialog.rect().contains(QRect(Button->mapTo(&Dialog, QPoint()), Button->size())));
            QVERIFY(Button->width() >= Button->minimumSizeHint().width());
        }
        QVERIFY(Edit->isVisible());
        Save(QStringLiteral("minimum"));
    }
};
QTEST_MAIN(DialogFoundationTests)
#include "tstDialogFoundation.moc"
