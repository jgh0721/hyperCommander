#pragma once
#include "extensionView.hpp"
#include <QAbstractTableModel>
#include <QJsonArray>
class QTableView;
namespace hc::compare
{
    class ArchiveModel final : public QAbstractTableModel
    {
    public:
        ArchiveModel(QJsonArray Entries, QObject* Parent) : QAbstractTableModel(Parent), entries_(std::move(Entries)) {}
        int rowCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : int(entries_.size()); }
        int columnCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : 4; }
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
    private:
        QJsonArray entries_;
    };
    class ArchiveCompareView final : public ExtensionView
    {
    public:
        ArchiveCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent = nullptr);
        void SetListFont(const QFont& Font) override;
    private:
        QTableView* table_;
    };
}
