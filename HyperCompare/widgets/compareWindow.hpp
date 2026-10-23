#pragma once
#include "runtime/session.hpp"
#include "runtime/mergeWork.hpp"
#include <QMainWindow>
#include <QFutureWatcher>
#include <QFont>
#include <QVector>

class ScintillaEditBase;
class QTreeWidget;
class QLabel;
class QSplitter;
class QAction;
class QCloseEvent;
class QComboBox;
class QTabWidget;

namespace hc::compare
{
    // Application-owned indicator; separate from lexer styles, line marker 8,
    // and Scintilla's selection elements. Reserve other IDs for future search.
    inline constexpr int sourceDifferenceIndicator = 20;
    class MergeController;
    class ScrollSynchronizer;
    class FindBar;
    class OverviewBar;
    class DirectoryView;
    class SemanticView;
    class CompareWindow final : public QMainWindow
    {
    public:
        explicit CompareWindow(Request Input);
        ~CompareWindow() override;
        int ExitCode() const { return exitCode_; }
        void SaveWork(const QString& Path, bool ReplaceExisting = false);
        void LoadWork(const QString& Path);
    protected:
        void closeEvent(QCloseEvent* Event) override;
    private:
        void start(bool Preserve = false);
        bool busy() const { return watcher_.isRunning() || workWatcher_.isRunning(); }
        void transferWork(const QString& Path, bool Loading, bool ReplaceExisting);
        void display();
        void applyPairView();
        const PairComparison* currentPair() const
        { return result_.completion_ == "Complete" ? result_.pairViews_[std::size_t(pairIndex_)].get() : nullptr; }
        void refresh(Result Value);
        void showBlocks();
        void openInputs();
        void openDirectories();
        void applyAppearance();
        void applyInlineHighlights();
        void chooseFont(const QString& Role);
        void save();
        void chooseSide(int Index);
        void chooseBlock(Resolution Choice);
        void updateMerge();
        void navigateBlock(int Direction);
        void selectBlock();
        ScintillaEditBase* addPane(const QString& Title, const QString& Path, const QByteArray& Text, bool ReadOnly);
        Request request_;
        Result result_;
        QFutureWatcher<Result> watcher_;
        struct WorkTransfer { MergeWorkFile file_; QString path_, error_; bool loading_ = false; };
        QFutureWatcher<WorkTransfer> workWatcher_;
        QString workPath_;
        std::optional<storage::DestinationExpectation> workExpected_;
        std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
        QVector<ScintillaEditBase*> editors_;
        ScintillaEditBase* output_ = nullptr;
        MergeController* merge_ = nullptr;
        ScrollSynchronizer* scroll_ = nullptr;
        FindBar* find_ = nullptr;
        OverviewBar* overview_ = nullptr;
        DirectoryView* directory_ = nullptr;
        SemanticView* semantic_ = nullptr;
        QTabWidget* details_ = nullptr;
        QWidget* sources_ = nullptr;
        QWidget* extensionView_ = nullptr;
        QSplitter* panes_ = nullptr;
        QTreeWidget* changes_ = nullptr;
        QLabel* state_ = nullptr;
        QLabel* inlineStatus_ = nullptr;
        QAction* save_ = nullptr;
        QAction* saveWork_ = nullptr;
        QAction* loadWork_ = nullptr;
        QAction* cancel_ = nullptr;
        QAction* undo_ = nullptr;
        QAction* redo_ = nullptr;
        QAction* baseChoice_ = nullptr;
        QAction* syncScroll_ = nullptr;
        QAction* alignRows_ = nullptr;
        QAction* inlineHighlights_ = nullptr;
        QAction* showOverview_ = nullptr;
        QAction* pairSelectorAction_ = nullptr;
        QComboBox* pairSelector_ = nullptr;
        int pairIndex_ = 0;
        QVector<QAction*> blockActions_;
        QVector<QAction*> wholeActions_;
        QVector<QAction*> findActions_;
        QFont uiFont_, listFont_, textFont_;
        bool dark_ = false, dirty_ = false, unresolved_ = false, closing_ = false, saving_ = false;
        bool refreshing_ = false;
        int exitCode_ = 3;
    };
}
