#pragma once
#include <QElapsedTimer>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <QtGlobal>
#include <functional>
#include <optional>
#include <utility>

class QAbstractButton;
class QTimer;

namespace hc::document
{
    /// One file read or write shown over the left of the status bar (plan 261104 §7).
    struct DocumentProgress
    {
        enum class Kind { None, Read, Save, Reinterpret, EpubSave };
        Kind kind_ = Kind::None;
        QString token_;                     // tab id + operation generation; another value is another operation
        QString label_;                     // text inside the bar
        std::optional<double> fraction_;    // 0..1, nullopt = indeterminate
        std::optional<qint64> count_;       // percent slot while indeterminate ("37개")
        bool cancellable_ = true;
        bool operator==(const DocumentProgress&) const = default;
    };

    struct DocumentProgressResult
    {
        bool success_ = true;
        QString doneLabel_;         // "열기 완료" · "저장 완료" · "완료 · 37개 이미지"; empty = by kind
        QString failureSummary_;    // "저장 실패 · 대상 폴더에 쓸 수 없음"
        QString failureDetail_;     // DetailRequested payload; empty = the summary
        bool operator==(const DocumentProgressResult&) const = default;
    };

    /// Child of the status bar, raised above its items, placed from the status bar's geometry.
    /// Only the bar and the × take the mouse (two hit buttons stacked above the overlay as its
    /// siblings); the rest passes through to the items below.
    ///
    ///   Idle ─start→ Pending ─300 ms→ Visible ─done→ Done(0.7 s) → Fading(0.3 s) → Idle
    ///                  │                       ├─fail→ Failed ─click · × · next token · Clear→ Idle
    ///                  └─done within 300 ms→ Idle └─×→ Cancelling('취소 중…') ─finish→ Idle
    ///
    /// Visible lasts at least 400 ms before Done. A failure before the bar appeared is still
    /// shown: the user would otherwise never learn that a save failed.
    class DocumentProgressOverlay final : public QWidget
    {
        Q_OBJECT
    public:
        enum class State { Idle, Pending, Visible, Done, Fading, Failed, Cancelling };
        Q_ENUM(State)
        /// Monotonic milliseconds.
        using Clock = std::function<qint64()>;

        static constexpr int ShowDelayMs = 300, MinVisibleMs = 400, DoneHoldMs = 700, FadeMs = 300;
        static constexpr int SweepPeriodMs = 1100, FrameMs = 16;
        static constexpr int LeftInset = 4, VerticalInset = 3, PreferredWidth = 300, MinimumWidth = 220;
        static constexpr double MaximumWidthRatio = 0.4;

        explicit DocumentProgressOverlay(QWidget* StatusBar);
        ~DocumentProgressOverlay() override;

        /// Starts or updates the running operation. Another token drops what is shown (Done ·
        /// Fading · Failed) at once; a running bar that was already visible hands its place to the
        /// new operation without hiding. Empty tokens and tokens that already finished are ignored.
        void Update(const DocumentProgress& Progress);
        /// Ignored unless Token is the running operation.
        void Finish(const QString& Token, const DocumentProgressResult& Result);
        /// Shows '취소 중…' until Finish or Clear. Before the bar appeared it keeps the show delay.
        void SetCancelling(const QString& Token);
        /// Tab switch: shows the tab's running operation at once, without the show delay.
        /// An empty token clears.
        void ShowCurrent(const DocumentProgress& Progress);
        /// Drops everything at once, a visible failure included.
        void Clear();
        /// Applies the timers against the clock. The internal timer calls it; tests call it after
        /// moving their clock.
        void Advance();

        State CurrentState() const { return state_; }
        QString CurrentToken() const { return current_.token_; }
        /// Text drawn inside the bar (also the accessible name).
        QString Text() const;
        /// Text drawn in the percent slot: "42 %", "37개", "⚠" or empty.
        QString PercentText() const;
        /// Accessible value; nullopt while indeterminate.
        std::optional<int> Percent() const;
        /// The internal timer runs only while waiting for a deadline or animating.
        bool TimerActive() const;

        void SetClock(Clock Now);
        /// nullopt follows Windows "애니메이션 효과" (SPI_GETCLIENTAREAANIMATION).
        void SetReduceMotion(std::optional<bool> Reduce);
        bool ReduceMotion() const;

        /// Overlay width for a status bar width: 300 DIP capped at 40 % of the bar, but never
        /// below 220 DIP — the minimum wins over the 40 % cap so the label stays readable. The
        /// only hard limit is the bar itself (4 DIP inset on both sides).
        static int OverlayWidth(int StatusBarWidth);
        /// Overlay-local geometry of the bar and the ×.
        QRect BarRect() const;
        QRect CloseRect() const;
        /// The hit buttons over the bar (enabled only while Failed) and over the ×.
        QAbstractButton* DetailButton() const;
        QAbstractButton* CloseButton() const;

    signals:
        void CancelRequested(QString Token);
        void DetailRequested(QString Detail);
        /// Every accessibility announcement: shown, completed, failed (never per percent).
        void Announced(QString Message);
        void StateChanged(hc::document::DocumentProgressOverlay::State Current);

    protected:
        bool eventFilter(QObject* Object, QEvent* Event) override;
        void changeEvent(QEvent* Event) override;
        void paintEvent(QPaintEvent* Event) override;

    private:
        struct Parts { QRectF bar_, percent_, close_; };
        Parts parts() const;
        qint64 now() const;
        void step(qint64 Now);
        void sync();
        void schedule();
        void place();
        void restack();
        void syncButtons(bool Shown);
        bool displayed() const;
        bool running() const;
        bool animating() const;
        bool closeShown() const;
        bool assign(const DocumentProgress& Progress);
        void start(const DocumentProgress& Progress, bool Immediately, qint64 Now);
        void enterVisible(qint64 Now);
        void enterDone(const DocumentProgressResult& Result, qint64 Now);
        void enterFailed(const DocumentProgressResult& Result);
        void enterIdle();
        void rememberFinished(const QString& Token);
        void closeClicked();
        void detailClicked();
        std::optional<double> shownFraction() const;
        qreal opacityAt(qint64 Now) const;
        QFont labelFont() const;

        State state_ = State::Idle, reported_ = State::Idle;
        DocumentProgress current_;
        DocumentProgressResult result_;
        bool held_ = false;            // success arrived while the minimum visible time runs
        bool cancelShown_ = false;     // Cancelling that has passed the show delay
        QStringList finished_;         // recent tokens that finished here; late ticks for them are ignored
        qint64 startedAt_ = 0, shownAt_ = 0, doneAt_ = 0, fadeAt_ = 0;
        Clock clock_;
        QElapsedTimer elapsed_;
        QTimer* timer_ = nullptr;
        QPointer<QAbstractButton> detailButton_, closeButton_;
        std::optional<bool> reduceMotion_;
        bool systemReduceMotion_ = false;
        std::optional<std::pair<QString, bool>> announcement_;  // message, assertive
    };
}
