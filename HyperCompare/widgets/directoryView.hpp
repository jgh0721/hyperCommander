#pragma once
#include "runtime/session.hpp"
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QWidget>
#include <functional>

class QTableView;
class QLabel;
class QPushButton;
namespace hc::compare
{
    class DirectoryModel final : public QAbstractTableModel
    {
    public:
        explicit DirectoryModel(QObject* Parent = nullptr) : QAbstractTableModel(Parent) {}
        void SetEntries(QJsonArray Entries);
        int rowCount(const QModelIndex& Parent = {}) const override;
        int columnCount(const QModelIndex& Parent = {}) const override;
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override;
        QJsonObject Entry(int Row) const;
    private:
        QJsonArray entries_;
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
        void SetListFont(const QFont& Font);
        void NavigateDifference(int Direction);
        void OpenCurrent();
        std::function<void(Request)> open_;
    private:
        void updateSelection();
        DirectoryModel* model_ = nullptr;
        DirectoryFilter* filter_ = nullptr;
        QTableView* table_ = nullptr;
        QLabel* roots_ = nullptr;
        QLabel* summary_ = nullptr;
        QPushButton* openButton_ = nullptr;
        Request request_;
    };
}
