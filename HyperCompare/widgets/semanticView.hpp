#pragma once
#include <QAbstractTableModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QTableView;
class QSortFilterProxyModel;
namespace hc::compare
{
    class SemanticModel final : public QAbstractTableModel
    {
    public:
        using QAbstractTableModel::QAbstractTableModel;
        int rowCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : int(rows_.size()); }
        int columnCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : 4; }
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Direction, int Role = Qt::DisplayRole) const override;
        void SetPair(const QJsonObject& Pair, const QJsonArray& Documents);
        QJsonObject Function(int Row, int Side) const;
    private:
        QJsonArray rows_, functions_[2];
    };
    class SemanticView final : public QWidget
    {
    public:
        explicit SemanticView(QWidget* Parent = nullptr);
        void SetResult(const QJsonObject& Analysis, bool HasBase);
        void SetPair(int Index);
        void SetListFont(const QFont& Font);
        void RevealCurrent();
        std::function<void(int, const QJsonObject&, int, const QJsonObject&)> reveal_;
        std::function<void(int)> pairChanged_;
    private:
        void updatePair();
        SemanticModel* model_;
        QSortFilterProxyModel* filter_;
        QTableView* table_;
        QComboBox* pairs_;
        QLabel* coverage_;
        QJsonObject analysis_;
    };
}
