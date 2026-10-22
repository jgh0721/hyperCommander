#pragma once
#include "runtime/session.hpp"
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

namespace hc::compare
{
    class MergeController;
    class CompareWindow final : public QMainWindow
    {
    public:
        explicit CompareWindow(Request Input);
        ~CompareWindow() override;
        int ExitCode() const { return exitCode_; }
    protected:
        void closeEvent(QCloseEvent* Event) override;
    private:
        void start();
        void display();
        void openInputs();
        void applyAppearance();
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
        std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
        QVector<ScintillaEditBase*> editors_;
        ScintillaEditBase* output_ = nullptr;
        MergeController* merge_ = nullptr;
        QSplitter* panes_ = nullptr;
        QTreeWidget* changes_ = nullptr;
        QLabel* state_ = nullptr;
        QAction* save_ = nullptr;
        QAction* cancel_ = nullptr;
        QAction* undo_ = nullptr;
        QAction* redo_ = nullptr;
        QAction* baseChoice_ = nullptr;
        QVector<QAction*> blockActions_;
        QVector<QAction*> wholeActions_;
        QFont uiFont_, listFont_, textFont_;
        bool dark_ = false, dirty_ = false, unresolved_ = false, closing_ = false, saving_ = false;
        int exitCode_ = 3;
    };
}
