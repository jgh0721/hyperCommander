#pragma once
#include "extensionView.hpp"
#include "runtime/largeText.hpp"
#include "runtime/compareLanguage.hpp"
#include <QFutureWatcher>
#include <functional>
#include <QFont>
class ScintillaEditBase;
class QLabel;
class QSpinBox;
class QPushButton;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QTimer;
class QHBoxLayout;
namespace hc::compare
{
    // Every document of the session takes part in the fallback, so a RESULT
    // without a known suffix styles like its inputs.
    CompareLanguageResolver LargeTextLanguages(const LargeTextComparison& Value, const QString& Override);
    void ApplyLargeTextEditorAppearance(ScintillaEditBase* Editor, const QFont& Font, bool Dark, int LexerId);
    class ChunkGutter;
    class LargeTextSyntax;
    class LargeTextView : public ExtensionView
    {
    public:
        LargeTextView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent);
        ~LargeTextView() override;
        bool Busy() const { return busy_; }
        bool Dirty() const;
        virtual int ExitCode() const;
        void Cancel() { if (canceled_) canceled_->store(true); }
        void SetTextFont(const QFont& Font) override;
        virtual void SetDark(bool Dark) { dark_ = Dark; }
        // "auto" or a CompareSyntaxChoices() id; restyles the displayed pages.
        virtual void SetSyntax(const QString& Override);
        const CompareLanguage& Language(int Side) const;
        const QString& Syntax() const { return input_.syntax_; }
        void SaveWork(const QString& Path, bool ReplaceExisting = false);
        void LoadWork(const QString& Path);
        void RecoverWork(const QString& Path);
        void ExportReport(const QString& Path, bool ReplaceExisting = false);
        void SaveActive();
        void FindCommand(int Direction);
        void NavigateDifference(int Direction);
        void Undo(bool Redo);
        bool CanUndo(bool Redo) const;
        bool CanExport() const { return !Busy(); }
        bool CanSave() const { return CanExport() && (!merging() || (!LargeTextUnresolved(*value_) && !value_->session_->reviewRequired_)); }
        std::function<void()> stateChanged_;
    protected:
        bool eventFilter(QObject* Object, QEvent* Event) override;
    protected:
        struct Task {
            std::shared_ptr<LargeTextComparison> comparison_;
            std::optional<LargeTextMatch> match_;
            int side_ = 0;
            QString note_;
            QString syntaxOverride_;
            Task(std::shared_ptr<LargeTextComparison> Value) : comparison_(std::move(Value)) {}
        };
        struct Response {
            std::shared_ptr<LargeTextComparison> comparison_;
            std::array<QByteArray, 2> text_;
            std::array<QByteArray, 2> styles_;
            std::array<std::shared_ptr<LargeTextSyntax>, 2> syntax_;
            std::array<std::uint64_t, 2> begin_{}, end_{};
            QString error_; QJsonArray recovery_;
            std::optional<LargeTextMatch> match_;
            int matchSide_ = 0;
            QString note_;
            // A loaded work file's language, applied after the page is shown.
            QString syntaxOverride_;
        };
        void load(bool Mutation = false, std::shared_ptr<LargeTextComparison> Value = {});
        void run(std::function<Task(const Budget&)> Work, bool Mutation, int Saved = -1);
        void applyPages();
        bool deferPages(std::function<void()> Action);
        void merge(int Source);
        void save(int Side, bool ChoosePath = false);
        void undo(bool Redo);
        void controls();
        void switchPair(int Index);
        void resolve(Resolution Choice);
        void completeReview();
        virtual void find(bool Backwards);
        void updateGutter();
        void chooseChunk(std::uint64_t Id, std::uint64_t Revision, Resolution Choice);
        bool merging() const { return value_->session_ && value_->session_->result_ >= 0; }
        Request input_;
        std::shared_ptr<LargeTextComparison> value_;
        std::array<LargeTextDocument, 2> saved_;
        std::vector<LargeTextDocument> savedAll_;
        bool resultNeverSaved_ = false;
        std::vector<std::shared_ptr<LargeTextComparison>> undo_, redo_;
        QFutureWatcher<Response> watcher_;
        std::shared_ptr<std::atomic_bool> canceled_;
        ScintillaEditBase* editors_[2]{};
        QHBoxLayout* panes_ = nullptr;
        QWidget* paneWidgets_[2]{};
        bool displayReadOnly_ = false;
        bool deferredContinuation_ = false;
        QSpinBox* lines_[2]{};
        QSpinBox* change_ = nullptr;
        QSpinBox* conflict_ = nullptr;
        QComboBox* pairs_ = nullptr;
        QLabel* titles_[2]{};
        ChunkGutter* gutter_ = nullptr;
        QLineEdit* query_ = nullptr;
        QComboBox* searchScope_ = nullptr;
        QCheckBox* searchCase_ = nullptr;
        QCheckBox* searchRegex_ = nullptr;
        bool lastRegex_ = false, lastCase_ = false;
        QTimer* applyTimer_ = nullptr;
        std::function<void()> pendingAction_;
        struct EditorPosition { std::intptr_t anchor_ = 0, current_ = 0, first_ = 0; };
        std::optional<std::array<EditorPosition, 2>> pendingPosition_;
        std::array<std::shared_ptr<LargeTextSyntax>, 2> syntax_;
        std::array<int, 2> lexerIds_{};
        CompareLanguageResolver languages_;
        QString lastQuery_;
        LargeTextMatch lastMatch_;
        int lastSearchDocument_ = -1;
        std::uint64_t generation_ = 0;
        int activeSide_ = 0;
        QFont textFont_{"D2Coding ligature", 14};
        QLabel* status_ = nullptr;
        std::array<std::uint64_t, 2> begin_{}, end_{};
        std::array<std::optional<std::uint64_t>, 2> pageStart_;
        std::array<bool, 2> pageDirty_{};
        QList<QPushButton*> buttons_;
        bool loading_ = false, busy_ = false, dark_ = false;
    };
}
