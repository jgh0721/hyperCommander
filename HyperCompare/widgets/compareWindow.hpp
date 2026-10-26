#pragma once
#include "runtime/session.hpp"
#include "runtime/mergeWork.hpp"
#include "runtime/inputEditing.hpp"
#include <QTimer>
#include <QMainWindow>
#include <QFutureWatcher>
#include <QFont>
#include <QVector>
#include <functional>
#include <QPointer>

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
    class ChunkGutter;
    class ScrollSynchronizer;
    class MultiPaneAlignment;
    class FindBar;
    class OverviewBar;
    class DirectoryView;
    class DirectoryMergeDialog;
    class SaveRecoveryDialog;
    class SemanticView;
    class ExtensionView;
    class CompareWindow final : public QMainWindow
    {
    public:
        explicit CompareWindow(Request Input);
        ~CompareWindow() override;
        int ExitCode() const { return exitCode_; }
        void BindRequest(std::function<void(int)> Completed);
        void CancelRemoteRequest();
        void SaveWork(const QString& Path, bool ReplaceExisting = false);
        void LoadWork(const QString& Path);
        void RecoverWork(const QString& Path);
        void ImportDecisions(const QString& Path);
        void ExportDecisions(const QString& Path, bool ReplaceExisting = false);
        void ExportReport(const QString& Path, bool ReplaceExisting = false);
    protected:
        void closeEvent(QCloseEvent* Event) override;
    private:
        void start(bool Preserve = false, bool Reintegrate = false);
        void setupInputEditing();
        void bindInputEditors();
        void invalidateInputComparison();
        void compareInputBuffers();
        void updateInputState();
        void saveInput(int Index);
        void applyInputChunk(std::uint64_t Id, std::uint64_t Revision, Resolution Choice);
        bool applySelectedChunk(ScintillaEditBase* Source, ScintillaEditBase* Target,
            std::intptr_t SourceBegin, std::intptr_t SourceEnd, std::intptr_t TargetBegin, std::intptr_t TargetEnd,
            std::uint64_t Id, std::uint64_t Revision);
        void inputUndo(bool Redo);
        bool busy() const;
        void transferWork(const QString& Path, bool Loading, bool ReplaceExisting, bool Decisions = false, bool RecoverText = false);
        void display();
        void applyPairView();
        const PairComparison* currentPair() const
        { return result_.completion_ == "Complete" && (!sourceEditing_ || (!sourcePending_ && sourceValid_)) ? result_.pairViews_[std::size_t(pairIndex_)].get() : nullptr; }
        void refresh(Result Value);
        void showBlocks();
        void openInputs();
        void openDirectories(bool Three = false);
        void applyAppearance();
        void updateFormats();
        void rebuildMultiAlignment();
        void updateAlignmentView();
        void rebuildGutters();
        void updateGutters();
        void chooseGutter(std::uint64_t Id, std::uint64_t Revision, Resolution Choice);
        void applyInlineHighlights();
        void showSettings();
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
        struct BufferComparison { Result result_; std::uint64_t generation_ = 0; };
        QFutureWatcher<BufferComparison> bufferWatcher_;
        QFutureWatcher<InputSaveResult> inputSaveWatcher_;
        QTimer bufferTimer_, directoryTimer_;
        bool directoryStreaming_ = false;
        std::shared_ptr<std::atomic_bool> bufferCanceled_;
        std::uint64_t bufferGeneration_ = 0;
        bool sourceEditing_ = false, sourcePending_ = false, sourceValid_ = true;
        int activeInput_ = 0, savingInput_ = -1;
        QString inputError_, inputSaveError_;
        QVector<QAction*> inputSaveActions_;
        struct WorkTransfer { MergeWorkFile file_; QString path_, error_; bool loading_ = false, decisions_ = false; };
        QFutureWatcher<WorkTransfer> workWatcher_;
        struct ReportTransfer { ReportFile file_; QString path_, error_; };
        QFutureWatcher<ReportTransfer> reportWatcher_;
        QAction* exportReport_ = nullptr;
        QString workPath_;
        std::optional<storage::DestinationExpectation> workExpected_;
        std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
        QVector<ScintillaEditBase*> editors_;
        ScintillaEditBase* output_ = nullptr;
        MergeController* merge_ = nullptr;
        ScrollSynchronizer* scroll_ = nullptr;
        MultiPaneAlignment* multi_ = nullptr;
        FindBar* find_ = nullptr;
        OverviewBar* overview_ = nullptr;
        DirectoryView* directory_ = nullptr;
        QPointer<DirectoryMergeDialog> directoryMerge_;
        QPointer<SaveRecoveryDialog> recoveryDialog_;
        QAction* recoveryAction_ = nullptr;
        SemanticView* semantic_ = nullptr;
        QTabWidget* details_ = nullptr;
        QWidget* sources_ = nullptr;
        ExtensionView* extensionView_ = nullptr;
        QSplitter* panes_ = nullptr;
        QTreeWidget* changes_ = nullptr;
        QLabel* state_ = nullptr;
        QLabel* inlineStatus_ = nullptr;
        QLabel* alignmentStatus_ = nullptr;
        QAction* save_ = nullptr;
        QAction* openFiles_ = nullptr;
        QAction* openFolders_ = nullptr;
        std::function<void(int)> completed_;
        QAction* saveWork_ = nullptr;
        QAction* loadWork_ = nullptr;
        QAction* recoverWork_ = nullptr;
        QAction* reintegrate_ = nullptr;
        QAction* importDecisions_ = nullptr;
        QAction* exportDecisions_ = nullptr;
        QAction* cancel_ = nullptr;
        QAction* undo_ = nullptr;
        QAction* redo_ = nullptr;
        QAction* baseChoice_ = nullptr;
        QAction* syncScroll_ = nullptr;
        QAction* alignRows_ = nullptr;
        QAction* alignAll_ = nullptr;
        QAction* inlineHighlights_ = nullptr;
        QAction* showOverview_ = nullptr;
        QAction* pairSelectorAction_ = nullptr;
        QComboBox* pairSelector_ = nullptr;
        int pairIndex_ = 0;
        QVector<QAction*> blockActions_;
        QVector<QAction*> wholeActions_;
        QVector<QAction*> findActions_;
        QVector<QAction*> encodingActions_;
        QVector<QLabel*> formatLabels_;
        QVector<ChunkGutter*> gutters_;
        QFont uiFont_, listFont_, textFont_;
        bool dark_ = false, dirty_ = false, unresolved_ = false, closing_ = false, saving_ = false;
        bool refreshing_ = false;
        int exitCode_ = 3;
    };
}
