#pragma once
#include "extensionView.hpp"
#include "runtime/largeText.hpp"
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
namespace hc::compare
{
    class ChunkGutter;
    class LargeTextView final : public ExtensionView
    {
    public:
        LargeTextView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent);
        ~LargeTextView() override;
        bool Busy() const { return busy_; }
        bool Dirty() const;
        int ExitCode() const;
        void Cancel() { if (canceled_) canceled_->store(true); }
        void SetTextFont(const QFont& Font) override;
        void SetDark(bool Dark) { dark_ = Dark; }
        void SaveWork(const QString& Path, bool ReplaceExisting = false);
        void LoadWork(const QString& Path);
        void RecoverWork(const QString& Path);
        void ExportReport(const QString& Path, bool ReplaceExisting = false);
        void SaveActive();
        void Undo(bool Redo);
        bool CanUndo(bool Redo) const;
        bool CanExport() const { return !Busy() && !pageDirty_[0] && !pageDirty_[1]; }
        bool CanSave() const { return CanExport() && (!merging() || (!LargeTextUnresolved(*value_) && !value_->session_->reviewRequired_)); }
        std::function<void()> stateChanged_;
    protected:
        bool eventFilter(QObject* Object, QEvent* Event) override;
    private:
        struct Task {
            std::shared_ptr<LargeTextComparison> comparison_;
            std::optional<LargeTextMatch> match_;
            int side_ = 0;
            QString note_;
            Task(std::shared_ptr<LargeTextComparison> Value) : comparison_(std::move(Value)) {}
        };
        struct Response {
            std::shared_ptr<LargeTextComparison> comparison_;
            std::array<QByteArray, 2> text_;
            std::array<std::uint64_t, 2> begin_{}, end_{};
            QString error_; QJsonArray recovery_;
            std::optional<LargeTextMatch> match_;
            int matchSide_ = 0;
            QString note_;
        };
        void load(bool Mutation = false, std::shared_ptr<LargeTextComparison> Value = {});
        void run(std::function<Task(const Budget&)> Work, bool Mutation, int Saved = -1);
        void applyPages();
        void merge(int Source);
        void save(int Side, bool ChoosePath = false);
        void undo(bool Redo);
        void controls();
        void switchPair(int Index);
        void resolve(Resolution Choice);
        void completeReview();
        void find(bool Backwards);
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
        QSpinBox* lines_[2]{};
        QSpinBox* change_ = nullptr;
        QSpinBox* conflict_ = nullptr;
        QComboBox* pairs_ = nullptr;
        QLabel* titles_[2]{};
        ChunkGutter* gutter_ = nullptr;
        QLineEdit* query_ = nullptr;
        QComboBox* searchScope_ = nullptr;
        QCheckBox* searchCase_ = nullptr;
        QString lastQuery_;
        LargeTextMatch lastMatch_;
        int lastSearchDocument_ = -1;
        std::uint64_t generation_ = 0;
        int activeSide_ = 0;
        QFont textFont_{"Consolas", 10};
        QLabel* status_ = nullptr;
        std::array<std::uint64_t, 2> begin_{}, end_{};
        std::array<std::optional<std::uint64_t>, 2> pageStart_;
        std::array<bool, 2> pageDirty_{};
        QList<QPushButton*> buttons_;
        bool loading_ = false, busy_ = false, dark_ = false;
    };
}
