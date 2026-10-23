#pragma once
#include "runtime/session.hpp"
#include <QAbstractTableModel>
#include <QDialog>
#include <QFutureWatcher>
#include <QSet>
#include <functional>

class QLineEdit;
class QLabel;
class QCheckBox;
class QPushButton;
class QTableView;
namespace hc::compare
{
    class DirectoryPlanModel final : public QAbstractTableModel
    {
    public:
        explicit DirectoryPlanModel(QObject* Parent) : QAbstractTableModel(Parent) {}
        void SetPlan(QJsonObject Plan);
        QJsonObject Plan() const;
        bool HasPlan() const { return plan_["kind"] == "HyperCompareDirectoryPlan"; }
        QString Output() const { return plan_["output"].toObject()["path"].toString(); }
        bool Matches(const QString& Output, bool Mirror, bool Overwrite) const;
        int Unresolved() const { return unresolved_; }
        int rowCount(const QModelIndex& Parent = {}) const override;
        int columnCount(const QModelIndex& Parent = {}) const override;
        QVariant data(const QModelIndex& Index, int Role) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override;
        Qt::ItemFlags flags(const QModelIndex& Index) const override;
        bool setData(const QModelIndex& Index, const QVariant& Value, int Role = Qt::EditRole) override;
        std::function<void()> changed_;
    private:
        QJsonObject plan_;
        QJsonArray actions_;
        QVector<QString> choices_;
        std::array<QSet<QString>, 3> paths_;
        int unresolved_ = 0;
    };
    class DirectoryMergeDialog final : public QDialog
    {
    public:
        DirectoryMergeDialog(Request Input, Result Comparison, QWidget* Parent = nullptr);
        ~DirectoryMergeDialog() override;
        void Prepare(const QString& Output, bool Mirror = false, bool Overwrite = false);
        void SavePlan(const QString& Path);
        void ApplyPlan(bool AllowDelete, bool Resume = false);
        bool Busy() const { return active_; }
        DirectoryPlanModel* Model() const { return model_; }
        const Result& LastResult() const { return last_; }
    protected:
        void closeEvent(QCloseEvent* Event) override;
        void reject() override;
    private:
        void run(QString Kind, std::function<Result()> Work);
        void updateControls();
        Request input_;
        Result comparison_, last_;
        QString kind_, planPath_;
        std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
        QFutureWatcher<Result> watcher_;
        DirectoryPlanModel* model_ = nullptr;
        QTableView* table_ = nullptr;
        QLineEdit* output_ = nullptr;
        QCheckBox* mirror_ = nullptr;
        QCheckBox* overwrite_ = nullptr;
        QLabel* state_ = nullptr;
        QPushButton* prepare_ = nullptr;
        QPushButton* save_ = nullptr;
        QPushButton* apply_ = nullptr;
        QPushButton* cancel_ = nullptr;
        bool active_ = false, closing_ = false;
    };
}
