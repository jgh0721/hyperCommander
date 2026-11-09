#pragma once
#include "extensionView.hpp"
#include "runtime/binarySession.hpp"
#include <QFutureWatcher>
#include <functional>
class QListWidget;
class QMenu;
class QComboBox;
class QLabel;
class QCheckBox;
class QLineEdit;
class QSplitter;
class QScrollBar;
class QToolBar;
namespace hc::compare
{
    struct HexCell { qint64 position_ = 0; std::array<qint64, 2> offsets_{-1, -1}; std::array<int, 2> bytes_{-1, -1}; std::array<int, 2> marks_{}; };
    struct HexWork
    {
        std::shared_ptr<BinaryComparison> comparison_;
        std::vector<HexCell> cells_;
        std::vector<HexRun> changes_;
        qint64 changeFirst_ = 0;
        QString error_, message_;
        QByteArray clipboard_;
        bool clipboardReady_ = false, cut_ = false;
        qint64 top_ = 0, found_ = -1;
        bool select_ = false, saved_ = false;
        int history_ = 0; // 1 edit, 2 undo, 3 redo; applied only after a successful worker result.
        int savedSide_ = -1;
    };
    class HexPane;
    class HexOverviewBar;
    class HexCompareView final : public ExtensionView
    {
        Q_OBJECT
    public:
        HexCompareView(const Request& Input, std::shared_ptr<BinaryComparison> Comparison, QWidget* Parent = nullptr);
        ~HexCompareView() override;
        void SetTextFont(const QFont& Font) override;
        bool Busy() const override;
        bool Dirty() const override;
        void Cancel() override;
        bool CanUndo(bool Redo) const override;
        void Undo(bool Redo) override;
        bool CanSave() const override;
        void SaveActive() override;
        bool HasOverview() const override { return true; }
        void SetOverviewVisible(bool Visible) override;
        std::function<void()> stateChanged_;
        std::function<void(Request)> openComparison_;
        void NavigateDifference(int Direction);
        void ShowFind();
        void FindCommand(int Direction) { if (!Direction) ShowFind(); else findNext(Direction < 0, false); }
        void PrintReport();
        std::shared_ptr<BinaryComparison> Comparison() const { return comparison_; }
        void ExportReport(const QString& Path = {});
    private:
        Request request_;
        std::shared_ptr<BinaryComparison> comparison_;
        std::array<BinaryDocument, 2> saved_;
        std::vector<std::array<BinaryDocument, 2>> undo_, redo_;
        QFutureWatcher<HexWork> watcher_;
        std::shared_ptr<std::atomic_bool> canceled_;
        std::array<HexPane*, 2> panes_{};
        std::array<QLabel*, 2> paths_{};
        HexOverviewBar* overview_{};
        QListWidget* differenceList_{};
        QSplitter* splitter_{};
        QScrollBar* scroll_{};
        QComboBox *alignment_{}, *width_{}, *filter_{}, *layout_{};
        QCheckBox *readOnly_{}, *insert_{};
        QLineEdit *find_{}, *replace_{}, *address_{};
        QComboBox* searchMode_{};
        QLabel* status_{};
        QToolBar* toolbar_{};
        std::vector<HexCell> cells_;
        std::vector<HexRun> changes_;
        qint64 changeFirst_ = 0;
        qint64 top_ = 0, cursor_ = 0, anchor_ = 0, pendingTop_ = -1;
        int active_ = 0;
        qint64 unlockedMiB_ = -1;
        bool rebuilding_ = false, locked_ = false, working_ = false, showAddresses_ = true;
        QString sessionPath_;
        int bytesPerRow() const;
        void work(std::function<HexWork(const Budget&)> Operation);
        void showPage(qint64 Top);
        void refresh();
        void select(qint64 Position, int Side, bool Extend);
        void editBytes(const QByteArray& Bytes, bool Delete = false);
        void copyAcross(int Source);
        void copyClipboard(bool Cut);
        void pasteClipboard();
        void findNext(bool Backward, bool Replace, bool All = false);
        QByteArray searchBytes(const QString& Text) const;
        void save(bool As);
        void open(int Side, bool Clipboard = false);
        void recompare();
        void session(bool Load);
        void reload();
        void format(int Side);
        void copyFileAndNext(int Source);
        void gotoAddress();
        void bindAction(QMenu* Menu, const QString& Text, const QString& Key, std::function<void()> Slot);
        friend class HexPane;
        friend class HexOverviewBar;
    };
}
