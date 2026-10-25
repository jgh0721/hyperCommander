#pragma once
#include "extensionView.hpp"
#include "runtime/largeText.hpp"
#include <QFutureWatcher>
#include <functional>
class ScintillaEditBase;
class QLabel;
class QSpinBox;
class QPushButton;
namespace hc::compare
{
    class LargeTextView final : public ExtensionView
    {
    public:
        LargeTextView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent);
        ~LargeTextView() override;
        bool Busy() const { return busy_; }
        bool Dirty() const;
        int ExitCode() const { return input_.textEquality_ == "bytes" ? (Dirty() || saved_[0].rawHash_ != saved_[1].rawHash_ ? 1 : 0) : value_->changes_.empty() ? 0 : 1; }
        void Cancel() { if (canceled_) canceled_->store(true); }
        void SetTextFont(const QFont& Font) override;
        void SetDark(bool Dark) { dark_ = Dark; }
        std::function<void()> stateChanged_;
    private:
        struct Response {
            std::shared_ptr<LargeTextComparison> comparison_;
            std::array<QByteArray, 2> text_;
            std::array<std::uint64_t, 2> begin_{}, end_{};
            QString error_; QJsonArray recovery_;
        };
        void load(bool Mutation = false, std::shared_ptr<LargeTextComparison> Value = {});
        void run(std::function<std::shared_ptr<LargeTextComparison>(const Budget&)> Work, bool Mutation, int Saved = -1);
        void applyPages();
        void merge(int Source);
        void save(int Side);
        void undo(bool Redo);
        void controls();
        Request input_;
        std::shared_ptr<LargeTextComparison> value_;
        std::array<LargeTextDocument, 2> saved_;
        std::vector<std::shared_ptr<LargeTextComparison>> undo_, redo_;
        QFutureWatcher<Response> watcher_;
        std::shared_ptr<std::atomic_bool> canceled_;
        ScintillaEditBase* editors_[2]{};
        QSpinBox* lines_[2]{};
        QSpinBox* change_ = nullptr;
        QLabel* status_ = nullptr;
        std::array<std::uint64_t, 2> begin_{}, end_{};
        std::array<std::optional<std::uint64_t>, 2> pageStart_;
        std::array<bool, 2> pageDirty_{};
        QList<QPushButton*> buttons_;
        bool loading_ = false, busy_ = false, dark_ = false;
    };
}
