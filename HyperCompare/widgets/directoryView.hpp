#pragma once
#include "runtime/session.hpp"
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QWidget>
#include <functional>

class QTableView;
class QTreeView;
class QSplitter;
class QComboBox;
class QLabel;
class QPushButton;
namespace hc::compare
{
    class DirectoryTreeModel;
    class DirectoryTreeFilter;
    bool DirectoryEntryMatches(const QJsonObject& Entry, const QString& Query, int Status);
    bool DirectoryEntryMatches(const DirectoryRow& Entry, const QString& Query, int Status);
    QString DirectoryStatusName(const QString& Status);
    class DirectoryModel final : public QAbstractTableModel
    {
    public:
        explicit DirectoryModel(QObject* Parent = nullptr) : QAbstractTableModel(Parent) {}
        void SetEntries(DirectoryEntries Entries);
        int rowCount(const QModelIndex& Parent = {}) const override;
        int columnCount(const QModelIndex& Parent = {}) const override;
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override;
        QJsonObject Entry(int Row) const;
        bool Matches(int Row, const QString& Query, int Status) const { return DirectoryEntryMatches(entries_.at(Row), Query, Status); }
    private:
        DirectoryEntries entries_;
    };
    class DirectoryFilter final : public QSortFilterProxyModel
    {
    public:
        explicit DirectoryFilter(QObject* Parent) : QSortFilterProxyModel(Parent) {}
        void SetQuery(QString Query, int Status);
    protected:
        bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override;
    private:
        QString query_;
        int status_ = 0;
    };
    class DirectoryView final : public QWidget
    {
    public:
        explicit DirectoryView(QWidget* Parent = nullptr);
        void SetComparison(const Request& Input, const Result& Value);
        void BeginComparison(const Request& Input);
        void ApplyProgress(const DirectoryProgress& Progress);
        void EndProgress(bool PreserveRows = true);
        void SetListFont(const QFont& Font);
        void NavigateDifference(int Direction);
        void OpenCurrent();
        std::function<void(Request)> open_;
        std::function<void(QString)> levelChanged_;
    private:
        void updateSelection();
        void updateRoots();
        QJsonObject currentEntry() const;
        void selectTree(const QModelIndex& Index);
        void switchView(bool Tree);
        DirectoryTreeModel* treeModel_ = nullptr;
        DirectoryTreeFilter* treeFilter_ = nullptr;
        QTreeView* trees_[2]{};
        QLabel* treeRoots_[2]{};
        QSplitter* treeBox_ = nullptr;
        QComboBox* viewMode_ = nullptr;
        QSet<QString> expandedPaths_;
        bool treeMode_ = true, syncing_ = false, streaming_ = false, listDeferred_ = false;
        DirectoryModel* model_ = nullptr;
        DirectoryFilter* filter_ = nullptr;
        QTableView* table_ = nullptr;
        QLabel* roots_ = nullptr;
        QLabel* summary_ = nullptr;
        QPushButton* openButton_ = nullptr;
        Request request_;
        Result comparison_;
    };
}
