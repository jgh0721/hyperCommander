#pragma once
#include "environmentSnapshot.hpp"
#include "commonLibs/system/environmentAnalysis.hpp"
#include <QAbstractItemModel>
#include <QSet>

namespace hc::environment {
enum class EnvironmentViewScope { User, System, Effective, Host, Process, Profile, History };
class EnvironmentModel final : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Role { IdRole = Qt::UserRole + 610, GridSortRole, ProblemRole, NameRole, EntryRole, ScopeRole, MaskedRole };
    struct Row {
        QString id_, name_, text_, description_, comparison_;
        std::optional<Value> value_;
        std::optional<int> entry_;
        std::vector<Row> children_;
        bool masked_ = false, problem_ = false;
    };
    explicit EnvironmentModel(QObject* Parent = nullptr);
    void SetSnapshot(const EnvironmentSnapshot& Snapshot, EnvironmentViewScope Scope, const ValueMap& Other = {}, bool Expanded = false);
    void Clear();
    void SetMaskExceptions(QSet<QString> Names) { maskExceptions_ = std::move(Names); }
    void SetDescriptions(const QHash<QString, QString>& Descriptions);
    void SetProblems(const std::vector<PathEntry>& Entries, const std::wstring& Name = L"Path");
    const Row* Item(const QModelIndex& Index) const;
    QModelIndex Find(const QString& Id) const;
    QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override;
    QModelIndex parent(const QModelIndex& Index) const override;
    int rowCount(const QModelIndex& Parent = {}) const override;
    int columnCount(const QModelIndex& = {}) const override { return 4; }
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
    QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override;
    Qt::ItemFlags flags(const QModelIndex& Index) const override;
    static bool ListVariable(const std::wstring& Name);
private:
    std::vector<Row> rows_;
    QHash<QString, QString> descriptions_;
    QSet<QString> maskExceptions_;
    EnvironmentViewScope scope_ = EnvironmentViewScope::User;
};
}
