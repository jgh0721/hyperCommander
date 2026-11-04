#include "stdafx.h"
#include "UniqueLibs/document/documentProgressOverlay.hpp"
#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QLabel>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qplatformaccessibility.h>
#include <qpa/qplatformintegration.h>

using hc::document::DocumentProgress;
using hc::document::DocumentProgressOverlay;
using hc::document::DocumentProgressResult;
using State = DocumentProgressOverlay::State;

namespace
{
    // A status-bar-sized host: one status item under the overlay and a test clock.
    struct Host
    {
        explicit Host(int Width = 1000)
        {
            bar_.resize(Width, 30);
            item_ = new QLabel("줄 1, 열 1", &bar_);
            item_->setGeometry(6, 1, 400, 29);
            overlay_ = new DocumentProgressOverlay(&bar_);
            overlay_->SetClock([this] { return now_; });
            overlay_->SetReduceMotion(false);
        }
        void At(qint64 Ms) { now_ = Ms; overlay_->Advance(); }
        bool Shown() const { return !overlay_->isHidden(); }
        QPoint Center(const QRect& Local) const { return overlay_->pos() + Local.center(); }
        QWidget* Detail() const { return overlay_->DetailButton(); }
        QWidget* Close() const { return overlay_->CloseButton(); }
        QWidget* Item() const { return item_; }
        QWidget bar_;
        QLabel* item_ = nullptr;
        DocumentProgressOverlay* overlay_ = nullptr;
        qint64 now_ = 0;
    };

    DocumentProgress Op(const QString& Token, std::optional<double> Fraction = 0.0,
                        std::optional<qint64> Count = std::nullopt)
    {
        DocumentProgress Progress;
        Progress.kind_ = DocumentProgress::Kind::Read;
        Progress.token_ = Token;
        Progress.label_ = QString("읽는 중 · %1.txt").arg(Token);
        Progress.fraction_ = Fraction;
        Progress.count_ = Count;
        return Progress;
    }

    DocumentProgressResult Success(const QString& Label = "열기 완료")
    {
        DocumentProgressResult Result;
        Result.doneLabel_ = Label;
        return Result;
    }

    DocumentProgressResult Failure()
    {
        DocumentProgressResult Result;
        Result.success_ = false;
        Result.failureSummary_ = "저장 실패 · 대상 폴더에 쓸 수 없음";
        Result.failureDetail_ = "Access denied (5)";
        return Result;
    }

    QList<State> States(const QSignalSpy& Spy)
    {
        QList<State> Result;
        for (const auto& Arguments : Spy) Result << Arguments.at(0).value<State>();
        return Result;
    }

    QList<QPair<const QObject*, QString>>& Announcements()
    {
        static QList<QPair<const QObject*, QString>> List;
        return List;
    }

    void recordAnnouncement(QAccessibleEvent* Event)
    {
        if (Event->type() == QAccessible::Announcement)
            Announcements().append({ Event->object(), static_cast<QAccessibleAnnouncementEvent*>(Event)->message() });
    }
}

class DocumentProgressTests final : public QObject
{
    Q_OBJECT
private slots:
    void shortOperationIsNeverShown()
    {
        Host H;
        QSignalSpy Announced(H.overlay_, &DocumentProgressOverlay::Announced);
        QSignalSpy Changes(H.overlay_, &DocumentProgressOverlay::StateChanged);
        H.overlay_->Update(Op("a", 0.5));
        QCOMPARE(H.overlay_->CurrentState(), State::Pending);
        QVERIFY(!H.Shown());
        H.At(299);
        QCOMPARE(H.overlay_->CurrentState(), State::Pending);
        QVERIFY(!H.Shown());
        H.overlay_->Finish("a", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        H.At(5000);
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());
        QCOMPARE(Announced.count(), 0);
        QCOMPARE(States(Changes), (QList<State>{ State::Pending, State::Idle }));
    }

    void showsAfterDelay()
    {
        Host H;
        QSignalSpy Announced(H.overlay_, &DocumentProgressOverlay::Announced);
        H.overlay_->Update(Op("a", 0.25));
        H.At(300);
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        QVERIFY(H.Shown());
        QCOMPARE(H.overlay_->Text(), QString("읽는 중 · a.txt"));
        QCOMPARE(Announced.count(), 1);
        QCOMPARE(Announced.at(0).at(0).toString(), QString("읽는 중 · a.txt"));
    }

    void keepsMinimumVisibleBeforeDone()
    {
        Host H;
        H.overlay_->Update(Op("a", 0.5));
        H.At(300);
        H.now_ = 500;
        H.overlay_->Finish("a", Success());
        // Finished 200 ms after appearing: the bar stays, full, until 400 ms have passed.
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        QCOMPARE(H.overlay_->Percent().value_or(-1), 100);
        QVERIFY(!H.overlay_->CloseButton()->isVisibleTo(&H.bar_));
        H.At(699);
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        H.At(700);
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        QCOMPARE(H.overlay_->Text(), QString("열기 완료"));
        QCOMPARE(H.overlay_->PercentText(), QString("100 %"));

        // Finished after the minimum: Done at once.
        Host Late;
        Late.overlay_->Update(Op("b", 0.5));
        Late.At(300);
        Late.now_ = 700;
        Late.overlay_->Finish("b", Success("저장 완료"));
        QCOMPARE(Late.overlay_->CurrentState(), State::Done);
        QCOMPARE(Late.overlay_->Text(), QString("저장 완료"));
    }

    void doneThenFadeThenIdle()
    {
        Host H;
        QSignalSpy Announced(H.overlay_, &DocumentProgressOverlay::Announced);
        QSignalSpy Changes(H.overlay_, &DocumentProgressOverlay::StateChanged);
        H.overlay_->Update(Op("a", 0.1));
        H.At(300);
        H.now_ = 800;
        H.overlay_->Finish("a", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.At(1499);
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.At(1500);
        QCOMPARE(H.overlay_->CurrentState(), State::Fading);
        QVERIFY(H.Shown());
        H.At(1799);
        QCOMPARE(H.overlay_->CurrentState(), State::Fading);
        H.At(1800);
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());
        QVERIFY(!H.overlay_->DetailButton()->isVisibleTo(&H.bar_));
        QCOMPARE(Announced.count(), 2);
        QCOMPARE(Announced.at(1).at(0).toString(), QString("열기 완료"));
        const QList<State> Expected{ State::Pending, State::Visible, State::Done, State::Fading, State::Idle };
        QCOMPARE(States(Changes), Expected);

        // A clock that jumps over the Done and the fade deadlines lands on Idle in one step.
        H.now_ = 2000;
        H.overlay_->Update(Op("b", 0.1));
        H.At(2300);
        H.now_ = 2800;
        H.overlay_->Finish("b", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.At(10000);
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());
    }

    void failurePersistsUntilDismissed()
    {
        Host H;
        QSignalSpy Detail(H.overlay_, &DocumentProgressOverlay::DetailRequested);
        auto FailVisible = [&](const QString& Token) {
            H.overlay_->Update(Op(Token, 0.3));
            H.At(H.now_ + 300);
            H.now_ += 50;
            H.overlay_->Finish(Token, Failure());
        };

        FailVisible("a");
        // No minimum visible time for a failure.
        QCOMPARE(H.overlay_->CurrentState(), State::Failed);
        QCOMPARE(H.overlay_->Text(), QString("저장 실패 · 대상 폴더에 쓸 수 없음 — 자세히"));
        QCOMPARE(H.overlay_->PercentText(), QString::fromUtf8("\xE2\x9A\xA0"));
        QVERIFY(!H.overlay_->Percent());
        H.At(H.now_ + 100000);
        QCOMPARE(H.overlay_->CurrentState(), State::Failed);
        QVERIFY(H.Shown());
        QVERIFY(!H.overlay_->TimerActive());
        QVERIFY(H.overlay_->CloseButton()->isVisibleTo(&H.bar_));
        H.overlay_->CloseButton()->click();
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());
        QCOMPARE(Detail.count(), 0);

        FailVisible("b");
        QVERIFY(H.overlay_->DetailButton()->isEnabled());
        H.overlay_->DetailButton()->click();
        QCOMPARE(Detail.count(), 1);
        QCOMPARE(Detail.at(0).at(0).toString(), QString("Access denied (5)"));
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);

        FailVisible("c");
        H.overlay_->Clear();
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());

        FailVisible("d");
        H.overlay_->Update(Op("e"));
        QCOMPARE(H.overlay_->CurrentState(), State::Pending);
        QCOMPARE(H.overlay_->CurrentToken(), QString("e"));
        QVERIFY(!H.Shown());
    }

    void failureBeforeShowIsShown()
    {
        Host H;
        QSignalSpy Announced(H.overlay_, &DocumentProgressOverlay::Announced);
        H.overlay_->Update(Op("a"));
        H.now_ = 50;
        H.overlay_->Finish("a", Failure());
        QCOMPARE(H.overlay_->CurrentState(), State::Failed);
        QVERIFY(H.Shown());
        QCOMPARE(Announced.count(), 1);
        QCOMPARE(Announced.at(0).at(0).toString(), QString("저장 실패 · 대상 폴더에 쓸 수 없음 — 자세히"));
    }

    void staleTokensAreIgnored()
    {
        Host H;
        H.overlay_->Update(Op("a", 0.1));
        H.overlay_->Finish("b", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Pending);
        QCOMPARE(H.overlay_->CurrentToken(), QString("a"));
        H.At(300);
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);

        // A new token replaces a visible operation at once, without hiding first.
        H.now_ = 400;
        H.overlay_->Update(Op("b", 0.1));
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        QCOMPARE(H.overlay_->CurrentToken(), QString("b"));
        QCOMPARE(H.overlay_->Text(), QString("읽는 중 · b.txt"));
        H.overlay_->Finish("a", Failure());
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        H.overlay_->SetCancelling("a");
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);

        H.now_ = 900;
        H.overlay_->Finish("b", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        // A late 50 ms tick of the finished operation neither restarts nor changes it.
        H.overlay_->Update(Op("b", 0.9));
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.overlay_->Finish("b", Failure());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.At(2000);
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        H.overlay_->Update(Op("b", 0.95));
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        // Empty tokens are not operations.
        H.overlay_->Update(Op(QString()));
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
    }

    void cancelFlow()
    {
        Host H;
        QSignalSpy Cancel(H.overlay_, &DocumentProgressOverlay::CancelRequested);
        H.overlay_->Update(Op("a", 0.4));
        H.At(300);
        QVERIFY(H.overlay_->CloseButton()->isVisibleTo(&H.bar_));
        QCOMPARE(H.overlay_->CloseButton()->toolTip(), QString("작업 취소 (Ctrl+.)"));
        H.overlay_->CloseButton()->click();
        QCOMPARE(Cancel.count(), 1);
        QCOMPARE(Cancel.at(0).at(0).toString(), QString("a"));
        QCOMPARE(H.overlay_->CurrentState(), State::Cancelling);
        QCOMPARE(H.overlay_->Text(), QString("취소 중…"));
        QVERIFY(H.Shown());
        QVERIFY(!H.overlay_->CloseButton()->isVisibleTo(&H.bar_));
        H.overlay_->Update(Op("a", 0.6));
        QCOMPARE(H.overlay_->CurrentState(), State::Cancelling);
        QCOMPARE(H.overlay_->Text(), QString("취소 중…"));
        QCOMPARE(H.overlay_->Percent().value_or(-1), 60);
        // The cancelled operation ends: no Done, no Failed.
        H.overlay_->Finish("a", Failure());
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());

        // Not cancellable: no ×.
        auto Fixed = Op("b", 0.4);
        Fixed.cancellable_ = false;
        H.now_ = 1000;
        H.overlay_->Update(Fixed);
        H.At(1300);
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        QVERIFY(!H.overlay_->CloseButton()->isVisibleTo(&H.bar_));
        H.overlay_->Clear();

        // Ctrl+. before the bar appeared keeps the show delay.
        H.now_ = 2000;
        H.overlay_->Update(Op("c", 0.1));
        H.overlay_->SetCancelling("wrong");
        QCOMPARE(H.overlay_->CurrentState(), State::Pending);
        H.overlay_->SetCancelling("c");
        QCOMPARE(H.overlay_->CurrentState(), State::Cancelling);
        QVERIFY(!H.Shown());
        H.At(2299);
        QVERIFY(!H.Shown());
        H.At(2300);
        QVERIFY(H.Shown());
        QCOMPARE(H.overlay_->Text(), QString("취소 중…"));
        H.overlay_->Finish("c", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QCOMPARE(Cancel.count(), 1);
    }

    void showCurrentSkipsDelay()
    {
        Host H;
        QSignalSpy Announced(H.overlay_, &DocumentProgressOverlay::Announced);
        H.overlay_->ShowCurrent(Op("a", 0.3));
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        QVERIFY(H.Shown());
        QCOMPARE(Announced.count(), 1);
        H.overlay_->Clear();
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!H.Shown());

        H.now_ = 10;
        H.overlay_->Update(Op("b", 0.1));
        QCOMPARE(H.overlay_->CurrentState(), State::Pending);
        H.now_ = 20;
        H.overlay_->ShowCurrent(Op("b", 0.4));
        QCOMPARE(H.overlay_->CurrentState(), State::Visible);
        QCOMPARE(H.overlay_->Percent().value_or(-1), 40);
        H.overlay_->ShowCurrent(Op(QString()));
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);

        // A tab whose operation already finished here shows nothing of another tab.
        H.now_ = 100;
        H.overlay_->Update(Op("c"));
        H.overlay_->Finish("c", Success());
        H.overlay_->ShowCurrent(Op("d"));
        QCOMPARE(H.overlay_->CurrentToken(), QString("d"));
        H.overlay_->ShowCurrent(Op("c"));
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
    }

    void percentAndCountText()
    {
        Host H;
        H.overlay_->ShowCurrent(Op("a", 0.42));
        QCOMPARE(H.overlay_->PercentText(), QString("42 %"));
        QCOMPARE(H.overlay_->Percent().value_or(-1), 42);
        H.overlay_->Update(Op("a", 0.29));
        QCOMPARE(H.overlay_->PercentText(), QString("29 %"));
        H.overlay_->Update(Op("a", std::nullopt, 37));
        QCOMPARE(H.overlay_->PercentText(), QString("37개"));
        QVERIFY(!H.overlay_->Percent());
        H.overlay_->Update(Op("a", std::nullopt));
        QCOMPARE(H.overlay_->PercentText(), QString());
        H.overlay_->Update(Op("a", 1.7));
        QCOMPARE(H.overlay_->PercentText(), QString("100 %"));
        H.now_ = 400;
        H.overlay_->Finish("a", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        QCOMPARE(H.overlay_->PercentText(), QString("100 %"));

        // Indeterminate until the end: Done shows no percent.
        H.now_ = 2000;
        H.overlay_->ShowCurrent(Op("b", std::nullopt, 37));
        H.now_ = 2400;
        H.overlay_->Finish("b", Success("완료 · 37개 이미지"));
        QCOMPARE(H.overlay_->Text(), QString("완료 · 37개 이미지"));
        QCOMPARE(H.overlay_->PercentText(), QString());
        QVERIFY(!H.overlay_->Percent());
    }

    void reduceMotionSkipsFade()
    {
        Host H;
        QSignalSpy Changes(H.overlay_, &DocumentProgressOverlay::StateChanged);
        H.overlay_->SetReduceMotion(true);
        QVERIFY(H.overlay_->ReduceMotion());
        H.overlay_->ShowCurrent(Op("a", std::nullopt));
        QVERIFY(!H.overlay_->TimerActive());  // still stripes instead of the sweep
        H.now_ = 500;
        H.overlay_->Finish("a", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.At(1199);
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        H.At(1200);
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        QVERIFY(!States(Changes).contains(State::Fading));
    }

    void timerRunsOnlyWhenNeeded()
    {
        Host H;
        QVERIFY(!H.overlay_->TimerActive());
        H.overlay_->Update(Op("a", 0.5));
        QVERIFY(H.overlay_->TimerActive());     // waiting for the show delay
        H.At(300);
        QVERIFY(!H.overlay_->TimerActive());    // determinate: repainted by updates only
        H.overlay_->Update(Op("a", std::nullopt));
        QVERIFY(H.overlay_->TimerActive());     // sweep
        H.overlay_->Finish("a", Failure());
        QVERIFY(!H.overlay_->TimerActive());
        H.overlay_->Clear();
        QVERIFY(!H.overlay_->TimerActive());
    }

    void geometryClamps()
    {
        // 300 DIP, at most 40 % of the bar, at least 220 DIP; the minimum wins over the 40 % cap
        // and only the bar itself (4 DIP inset on both sides) limits it further.
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(1000), 300);
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(750), 300);
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(625), 250);
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(550), 220);
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(400), 220);
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(200), 192);
        QCOMPARE(DocumentProgressOverlay::OverlayWidth(0), 0);

        Host H;
        H.bar_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&H.bar_));
        H.overlay_->ShowCurrent(Op("a", 0.5));
        QCOMPARE(H.overlay_->geometry(), QRect(4, 3, 300, 24));
        // [bar] 8 [percent 34] 8 [× 18], 6 DIP padding; bar 18 DIP high, centred.
        QCOMPARE(H.overlay_->BarRect(), QRect(6, 3, 220, 18));
        QCOMPARE(H.overlay_->CloseRect(), QRect(276, 3, 18, 18));
        QCOMPARE(H.overlay_->CloseButton()->geometry(), H.overlay_->CloseRect().translated(4, 3));
        H.bar_.resize(625, 30);
        QTRY_COMPARE(H.overlay_->width(), 250);
        QCOMPARE(H.overlay_->BarRect().width(), 170);
        QCOMPARE(H.overlay_->DetailButton()->geometry(), H.overlay_->BarRect().translated(4, 3));
        H.bar_.resize(400, 30);
        QTRY_COMPARE(H.overlay_->width(), 220);
        // A wrapped status bar: the overlay stays on the first row.
        H.bar_.resize(1000, 60);
        QTRY_COMPARE(H.overlay_->width(), 300);
        QVERIFY(H.overlay_->height() < 60 - 6);
    }

    void mouseReachesOnlyBarAndClose()
    {
        Host H;
        H.overlay_->ShowCurrent(Op("a", 0.5));
        QVERIFY(H.overlay_->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(H.bar_.childAt(H.Center(H.overlay_->BarRect())), H.Detail());
        QCOMPARE(H.bar_.childAt(H.Center(H.overlay_->CloseRect())), H.Close());
        // The percent slot passes the mouse to the status item below.
        const QPoint Slot = H.overlay_->pos() + QPoint(H.overlay_->BarRect().right() + 20, H.overlay_->height() / 2);
        QCOMPARE(H.bar_.childAt(Slot), H.Item());
        // While running the bar swallows presses without acting on them.
        QVERIFY(!H.overlay_->DetailButton()->isEnabled());
        H.overlay_->Clear();
        QCOMPARE(H.bar_.childAt(H.Center(H.overlay_->BarRect())), H.Item());
    }

    void staysAboveNewStatusItems()
    {
        Host H;
        H.overlay_->ShowCurrent(Op("a", 0.5));
        auto* Late = new QLabel("late", &H.bar_);
        Late->setGeometry(0, 0, 600, 30);
        Late->show();
        const auto& Children = H.bar_.children();
        QVERIFY(Children.indexOf(H.overlay_) > Children.indexOf(Late));
        QCOMPARE(H.bar_.childAt(H.Center(H.overlay_->BarRect())), H.Detail());
        QCOMPARE(H.bar_.childAt(H.Center(H.overlay_->CloseRect())), H.Close());
    }

    void accessibleInterface()
    {
        Host H;
        auto* Interface = QAccessible::queryAccessibleInterface(H.overlay_);
        QVERIFY(Interface);
        QCOMPARE(Interface->role(), QAccessible::ProgressBar);
        H.overlay_->ShowCurrent(Op("a", 0.42));
        QCOMPARE(Interface->text(QAccessible::Name), QString("읽는 중 · a.txt"));
        QCOMPARE(Interface->text(QAccessible::Value), QString("42 %"));
        QVERIFY(Interface->valueInterface());
        QCOMPARE(Interface->valueInterface()->currentValue().toInt(), 42);
        QCOMPARE(Interface->valueInterface()->minimumValue().toInt(), 0);
        QCOMPARE(Interface->valueInterface()->maximumValue().toInt(), 100);
        H.overlay_->Update(Op("a", std::nullopt));
        QVERIFY(!Interface->valueInterface()->currentValue().isValid());
    }

    void accessibilityEventCount()
    {
        // Screen readers hear the start (when shown), the completion and the failure once each.
        Host H;
        auto* Integration = QGuiApplicationPrivate::platformIntegration();
        if (!Integration || !Integration->accessibility()) QSKIP("No platform accessibility");
        Announcements().clear();
        const auto Previous = QAccessible::installUpdateHandler(&recordAnnouncement);
        Integration->accessibility()->setActive(true);
        const auto Restore = qScopeGuard([&] {
            Integration->accessibility()->setActive(false);
            QAccessible::installUpdateHandler(Previous);
        });
        QSignalSpy Announced(H.overlay_, &DocumentProgressOverlay::Announced);
        H.now_ = 1000;
        H.overlay_->Update(Op("b", 0.0));
        H.At(1300);
        for (int Step = 1; Step < 10; ++Step) H.overlay_->Update(Op("b", Step / 10.0));
        H.now_ = 1800;
        H.overlay_->Finish("b", Success());
        H.At(2500);
        H.At(2800);
        QCOMPARE(H.overlay_->CurrentState(), State::Idle);
        H.overlay_->Update(Op("c", 0.0));
        H.now_ = 2850;
        H.overlay_->Finish("c", Failure());
        QCOMPARE(H.overlay_->CurrentState(), State::Failed);

        QStringList Messages;
        for (const auto& [Object, Message] : Announcements())
            if (Object == H.overlay_) Messages << Message;
        const QStringList Expected{ "읽는 중 · b.txt", "열기 완료", "저장 실패 · 대상 폴더에 쓸 수 없음 — 자세히" };
        QCOMPARE(Messages, Expected);
        QCOMPARE(Announced.count(), 3);
    }

    void paintsEveryState()
    {
        Host H;
        auto Grab = [&] { QVERIFY(!H.overlay_->grab().isNull()); };
        auto Pixel = [&](QPoint Point) {
            const auto Image = H.overlay_->grab().toImage();
            return Image.pixelColor((QPointF(Point) * Image.devicePixelRatio()).toPoint());
        };
        // Half filled: the left of the bar carries the accent, the right only the input background.
        auto Blank = Op("a", 0.5);
        Blank.label_.clear();
        H.overlay_->ShowCurrent(Blank);
        QVERIFY(Pixel({ 60, 12 }) != Pixel({ 180, 12 }));
        H.overlay_->Update(Op("a", 0.5));
        Grab();
        H.overlay_->Update(Op("a", std::nullopt, 37));
        H.At(550);
        Grab();
        H.overlay_->SetReduceMotion(true);
        Grab();
        H.overlay_->SetReduceMotion(false);
        H.overlay_->SetCancelling("a");
        Grab();
        H.overlay_->Finish("a", Success());
        H.now_ = 1000;
        H.overlay_->ShowCurrent(Op("b", 0.7));
        H.now_ = 1500;
        H.overlay_->Finish("b", Success());
        QCOMPARE(H.overlay_->CurrentState(), State::Done);
        Grab();
        H.At(2350);
        QCOMPARE(H.overlay_->CurrentState(), State::Fading);
        Grab();
        H.overlay_->Update(Op("c", 0.2));
        H.overlay_->Finish("c", Failure());
        QCOMPARE(H.overlay_->CurrentState(), State::Failed);
        Grab();
    }
};

int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv); App.setAttribute(Qt::AA_Use96Dpi, true);
    DocumentProgressTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstDocumentProgress.moc"
