#include "stdafx.h"

/// 모듈

/// UIs
#include "text/TextMinimapPeek.h"
#include "text/TextMinimapWidget.h"
#include "text/TextOverviewRuler.h"

/// 기타 헤더파일
#include "text/FindReplaceWidget.h"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "text/TextMinimap.h"
#include "text/TextMinimapSettings.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractScrollArea>
#include <QApplication>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace
{
    QString lines(int Count, const QString& Word = QStringLiteral("line"))
    {
        QStringList Result;
        Result.reserve(Count);
        for (int Index = 0; Index < Count; ++Index)
            Result.append(QStringLiteral("%1 %2 with some text").arg(Word).arg(Index));
        return Result.join(QLatin1Char('\n'));
    }

    struct Editor
    {
        std::unique_ptr<QTextView> view_;
        ScintillaQtDirectBackend* backend_ = nullptr;
        TextMinimap* minimap_ = nullptr;
        TextMinimapWidget* column_ = nullptr;

        qintptr Send(unsigned Message, quintptr W = 0, qintptr L = 0) const { return backend_->Send(Message, W, L); }
        QWidget* Viewport() const { return backend_->scrollArea()->viewport(); }
        int CaretLine() const { return int(Send(SCI_LINEFROMPOSITION, quintptr(Send(SCI_GETCURRENTPOS)))); }
        int FirstRow() const { return int(Send(SCI_GETFIRSTVISIBLELINE)); }
    };

    Editor open(const QString& Text, QSize Size = {900, 600}, const QString& Name = QStringLiteral("sample.txt"))
    {
        Editor Result;
        Result.view_ = std::make_unique<QTextView>();
        Result.view_->LoadContent(Text, Name, QStringLiteral("UTF-8"));
        Result.view_->resize(Size);
        Result.view_->show();
        if (!QTest::qWaitForWindowExposed(Result.view_.get()))
            qWarning("window not exposed");
        Result.backend_ = static_cast<ScintillaQtDirectBackend*>(Result.view_->Backend());
        Result.minimap_ = Result.view_->Minimap();
        Result.column_ = Result.minimap_ ? Result.minimap_->Column() : nullptr;
        QCoreApplication::processEvents();
        return Result;
    }

    void storeDefaults(void (*Change)(TextMinimapSettings&) = nullptr)
    {
        TextMinimapSettings Settings;
        Settings.peekDelayMs_ = 50;
        if (Change)
            Change(Settings);
        TextMinimapSettings::Store(Settings);
    }
}

class TextMinimapUiTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(config_.isValid());
        qApp->setProperty("documentViewsConfigDir", config_.path());
    }

    void init() { storeDefaults(); }

    void columnBesideViewport()
    {
        auto E = open(lines(2000));
        QVERIFY(E.minimap_ && E.column_);
        QTRY_VERIFY(E.column_->isVisible());
        QCOMPARE(E.column_->width(), 104);
        QCOMPARE(E.column_->geometry().left(), E.Viewport()->geometry().right() + 1);
        QCOMPARE(E.column_->height(), E.Viewport()->height());
        QVERIFY(E.column_->Mapping().document_);
        QVERIFY(E.minimap_->Ruler()->isVisible());
        QCOMPARE(E.column_->focusPolicy(), Qt::NoFocus);
        storeDefaults([](TextMinimapSettings& S) { S.side_ = TextMinimapSettings::Side::Left; });
        QTRY_COMPARE(E.column_->geometry().right() + 1, E.Viewport()->geometry().left());
    }

    void clickKeepsCaretSelectionUndo()
    {
        auto E = open(lines(2000));
        QTRY_VERIFY(E.column_->isVisible());
        E.Send(SCI_APPENDTEXT, 1, qintptr("x"));
        E.Send(SCI_SETSEL, 100, 200);
        const auto Caret = E.Send(SCI_GETCURRENTPOS), Anchor = E.Send(SCI_GETANCHOR);
        const auto CanUndo = E.Send(SCI_CANUNDO), Modified = E.Send(SCI_GETMODIFY);
        const int First = E.FirstRow();
        QTest::mouseClick(E.column_, Qt::LeftButton, {}, QPoint(50, E.column_->height() - 20));
        QTRY_VERIFY(E.FirstRow() > First);
        QCOMPARE(E.Send(SCI_GETCURRENTPOS), Caret);
        QCOMPARE(E.Send(SCI_GETANCHOR), Anchor);
        QCOMPARE(E.Send(SCI_CANUNDO), CanUndo);
        QCOMPARE(E.Send(SCI_GETMODIFY), Modified);
    }

    void doubleClickUsesFirstPress()
    {
        auto E = open(lines(4000));
        QTRY_VERIFY(E.column_->isVisible());
        const int Y = E.column_->height() - 30;
        const auto Map = E.column_->Mapping();
        QVERIFY(Map.documentGeometry_.scrolls_);
        const int Expected = E.minimap_->Source().LineOfRow(int(Map.RowAt(Y)));
        // Windows order: press (centres the row, which scrolls the thumbnail), release, double click, release.
        const QPoint At(50, Y);
        QTest::mousePress(E.column_, Qt::LeftButton, {}, At);
        QTest::mouseRelease(E.column_, Qt::LeftButton, {}, At);
        QVERIFY(qAbs(E.column_->Mapping().RowAt(Y) - Map.RowAt(Y)) > 5); // the row under the pointer moved
        QMouseEvent Double(QEvent::MouseButtonDblClick, At, E.column_->mapToGlobal(At), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(E.column_, &Double);
        QTest::mouseRelease(E.column_, Qt::LeftButton, {}, At);
        QTRY_COMPARE(E.CaretLine(), Expected);
        // A double click without the first press (synthetic) uses the row under the pointer.
        const int Under = E.minimap_->Source().LineOfRow(int(E.column_->Mapping().RowAt(40)));
        QTest::qWait(2 * QApplication::doubleClickInterval() + 50);
        QTest::mouseDClick(E.column_, Qt::LeftButton, {}, QPoint(50, 40));
        QTRY_COMPARE(E.CaretLine(), Under);
    }

    void dragAndWheelScroll()
    {
        auto E = open(lines(4000));
        QTRY_VERIFY(E.column_->isVisible());
        const auto Map = E.column_->Mapping();
        const QPoint Grip(50, int(Map.SliderTop() + 3));
        const int First = E.FirstRow();
        QTest::mousePress(E.column_, Qt::LeftButton, {}, Grip);
        QTest::mouseMove(E.column_, Grip + QPoint(0, 120));
        QTest::mouseRelease(E.column_, Qt::LeftButton, {}, Grip + QPoint(0, 120));
        QTRY_VERIFY(E.FirstRow() > First);
        const int AfterDrag = E.FirstRow();
        const auto Zoom = E.Send(SCI_GETZOOM);
        const QPoint At(50, 50);
        QWheelEvent Wheel(At, E.column_->mapToGlobal(At), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(E.column_, &Wheel);
        QTRY_VERIFY(E.FirstRow() > AfterDrag);
        const int AfterWheel = E.FirstRow();
        QWheelEvent ZoomWheel(At, E.column_->mapToGlobal(At), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(E.column_, &ZoomWheel);
        QCoreApplication::processEvents();
        QCOMPARE(E.Send(SCI_GETZOOM), Zoom);
        QCOMPARE(E.FirstRow(), AfterWheel);
    }

    void toggleCommandHidesColumn()
    {
        auto E = open(lines(2000));
        auto Other = open(lines(100));
        QTRY_VERIFY(E.column_->isVisible());
        const int Width = E.Viewport()->width();
        auto* Toggle = E.view_->findChild<QAction*>(QStringLiteral("text.minimap"));
        QVERIFY(Toggle);
        Toggle->trigger();
        QTRY_VERIFY(!E.column_->isVisible());
        QCOMPARE(E.Viewport()->width(), Width + 104);
        QVERIFY(!E.minimap_->Ruler()->isVisible());
        // Shared setting: the other editor follows at once.
        QVERIFY(!Other.column_->isVisible());
        QVERIFY(!E.view_->IsMinimapEnabled());
        Toggle->trigger();
        QTRY_VERIFY(E.column_->isVisible());
        QCOMPARE(E.Viewport()->width(), Width);
    }

    void narrowShowsOverviewOnly()
    {
        auto E = open(lines(2000), {440, 500});
        QTRY_VERIFY(!E.column_->isVisible());
        QVERIFY(E.minimap_->Mode().narrow_);
        QVERIFY(E.minimap_->Ruler()->isVisible());
        E.view_->resize(900, 500);
        QTRY_VERIFY(E.column_->isVisible());
        QVERIFY(!E.minimap_->Mode().narrow_);
    }

    void largeDocumentUsesOutline()
    {
        auto E = open(lines(25000));
        QTRY_VERIFY(E.column_->isVisible());
        QVERIFY(!E.column_->Mapping().document_);
        QVERIFY(E.minimap_->Mode().large_);
        QTRY_VERIFY(E.minimap_->Outline().Complete());
        QVERIFY(E.minimap_->Outline().BinCount() > 0);
    }

    void historyMarkersInBodyOnlyMode()
    {
        auto E = open(lines(200));
        E.view_->setChangeHistoryMode(ScintillaEditorSettings::ChangeHistoryIndicators);
        const qintptr Position = E.Send(SCI_POSITIONFROMLINE, 5);
        E.Send(SCI_INSERTTEXT, quintptr(Position), qintptr("edit "));
        QVERIFY(E.Send(SCI_MARKERGET, 5) & SC_MASK_HISTORY);
        QCOMPARE(E.Send(SCI_GETMARGINWIDTHN, 2), 0);
        QCOMPARE(E.Send(SCI_MARKERSYMBOLDEFINED, SC_MARKNUM_HISTORY_MODIFIED), qintptr(SC_MARK_EMPTY));
        // No background tint on the edited line: the empty right side looks like any other line's.
        const QImage Shot = E.Viewport()->grab().toImage();
        const int X = Shot.width() - 6;
        const int EditedY = int(E.Send(SCI_POINTYFROMPOSITION, 0, Position) + E.Send(SCI_TEXTHEIGHT, 5) / 2);
        const int OtherY = int(E.Send(SCI_POINTYFROMPOSITION, 0, E.Send(SCI_POSITIONFROMLINE, 9)) + E.Send(SCI_TEXTHEIGHT, 9) / 2);
        const qreal Dpr = Shot.devicePixelRatio();
        QCOMPARE(Shot.pixelColor(X, int(EditedY * Dpr)), Shot.pixelColor(X, int(OtherY * Dpr)));
        QTRY_VERIFY(std::any_of(E.minimap_->Changes().cbegin(), E.minimap_->Changes().cend(),
                                [](const auto& Mark) { return Mark.line_ == 5 && Mark.change_ == TextMinimapChange::Modified; }));
        // Margin mode restores the bar symbol and the margin.
        E.view_->setChangeHistoryMode(ScintillaEditorSettings::ChangeHistoryMarkers);
        QCOMPARE(E.Send(SCI_MARKERSYMBOLDEFINED, SC_MARKNUM_HISTORY_MODIFIED), qintptr(SC_MARK_BAR));
        QVERIFY(E.Send(SCI_GETMARGINWIDTHN, 2) > 0);
        // Off: no history layer.
        E.view_->setChangeHistoryMode(ScintillaEditorSettings::ChangeHistoryOff);
        E.view_->ReloadPreferences();
        QTRY_VERIFY(E.minimap_->Changes().isEmpty());
        E.view_->setChangeHistoryMode(ScintillaEditorSettings::ChangeHistoryMarkers);
    }

    void historyKeptAcrossDocumentSwitch()
    {
        auto E = open(QStringLiteral("seed"));
        const auto A = E.Send(SCI_CREATEDOCUMENT, 0, SC_DOCUMENTOPTION_DEFAULT);
        const auto B = E.Send(SCI_CREATEDOCUMENT, 0, SC_DOCUMENTOPTION_DEFAULT);
        E.view_->AttachDocument(A, QStringLiteral("None"));
        E.backend_->SetTextUtf8(lines(50).toUtf8());
        E.Send(SCI_INSERTTEXT, quintptr(E.Send(SCI_POSITIONFROMLINE, 3)), qintptr("changed "));
        const auto HasLine3 = [&E] {
            return std::any_of(E.minimap_->Changes().cbegin(), E.minimap_->Changes().cend(), [](const auto& Mark) { return Mark.line_ == 3; });
        };
        QTRY_VERIFY(HasLine3());
        E.view_->AttachDocument(B, QStringLiteral("None"));
        E.backend_->SetTextUtf8(lines(30, QStringLiteral("other")).toUtf8());
        QTRY_VERIFY(!HasLine3());
        E.view_->AttachDocument(A, QStringLiteral("None"));
        QTRY_VERIFY(HasLine3());
        E.Send(SCI_RELEASEDOCUMENT, 0, A);
        E.Send(SCI_RELEASEDOCUMENT, 0, B);
    }

    void searchCountAndLayer()
    {
        QString Text = lines(500);
        Text.replace(QStringLiteral("line 42 "), QStringLiteral("needle 42 ")).replace(QStringLiteral("line 420 "), QStringLiteral("needle needle "));
        auto E = open(Text);
        E.view_->showFindBar(false);
        auto* Find = E.view_->findChild<FindReplaceWidget*>();
        QVERIFY(Find);
        Find->SetSearchText(QStringLiteral("needle"));
        QTRY_COMPARE_WITH_TIMEOUT(E.minimap_->Search().Count(), 3, 3000);
        QCOMPARE(E.minimap_->Search().Hits().first().line_, 42);
        QCOMPARE(E.minimap_->Search().Hits().last().line_, 420);
        // An edit updates the collected matches.
        E.Send(SCI_APPENDTEXT, 7, qintptr("\nneedle"));
        QTRY_COMPARE_WITH_TIMEOUT(E.minimap_->Search().Count(), 4, 3000);
        E.view_->hideFindBar();
        QVERIFY(!E.minimap_->Search().Active());
    }

    void problemLinesShift()
    {
        auto E = open(lines(100));
        E.view_->SetProblemLines({{10, true}, {20, false}});
        QCOMPARE(E.minimap_->Problems().size(), 2);
        E.Send(SCI_INSERTTEXT, 0, qintptr("a\nb\nc\n"));
        QCOMPARE(E.minimap_->Problems()[0].line_, 13);
        QCOMPARE(E.minimap_->Problems()[1].line_, 23);
        // A new document clears them.
        E.view_->LoadContent(lines(10), QStringLiteral("other.txt"), QStringLiteral("UTF-8"));
        QVERIFY(E.minimap_->Problems().isEmpty());
    }

    void overviewMarkClick()
    {
        auto E = open(lines(2000));
        QTRY_VERIFY(E.minimap_->Ruler()->isVisible());
        E.view_->SetProblemLines({{1500, true}});
        auto* Ruler = E.minimap_->Ruler();
        auto* Bar = qobject_cast<QScrollBar*>(Ruler->parentWidget());
        QVERIFY(Bar);
        const int Y = int(Ruler->YOfRow(E.minimap_->Source().RowOfLine(1500)));
        QTest::mouseClick(Bar, Qt::LeftButton, {}, Ruler->mapToParent(QPoint(Ruler->width() / 2, Y)));
        const int OnScreen = int(E.Send(SCI_LINESONSCREEN));
        QTRY_VERIFY(qAbs(E.FirstRow() + OnScreen / 2 - 1500) <= 2);
    }

    void peekShowsOnHover()
    {
        auto E = open(lines(2000));
        QTRY_VERIFY(E.column_->isVisible());
        const QPoint At(50, 100);
        QTest::mouseMove(E.column_, At);
        QMouseEvent Move(QEvent::MouseMove, At, E.column_->mapToGlobal(At), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(E.column_, &Move);
        auto* Peek = E.minimap_->Peek();
        QTRY_VERIFY(Peek->isVisible());
        const int Expected = E.minimap_->Source().LineOfRow(int(E.column_->Mapping().RowAt(At.y())));
        QCOMPARE(Peek->CenterLine(), Expected);
        QEvent Leave(QEvent::Leave);
        QCoreApplication::sendEvent(E.column_, &Leave);
        QTRY_VERIFY(!Peek->isVisible());
    }

private:
    QTemporaryDir config_;
};

int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    App.setAttribute(Qt::AA_Use96Dpi, true);
    TextMinimapUiTests Tests;
    return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstTextMinimapUi.moc"
