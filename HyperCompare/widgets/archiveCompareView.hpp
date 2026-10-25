#pragma once
#include "extensionView.hpp"
#include <QAbstractTableModel>
#include <QJsonArray>
#include "runtime/nativeArchive.hpp"
#include <QFutureWatcher>
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
        void SetEntries(QJsonArray Entries) { beginResetModel(); entries_ = std::move(Entries); endResetModel(); }
    private:
        QJsonArray entries_;
    };
    class ArchiveCompareView final : public ExtensionView
    {
    public:
        ArchiveCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent = nullptr);
        ArchiveCompareView(Request Input, std::shared_ptr<ArchiveSession> Session, QWidget* Parent = nullptr);
        ~ArchiveCompareView() override;
        void SetListFont(const QFont& Font) override;
        bool Busy() const override { return busy_; }
        bool Dirty() const override;
        void Cancel() override { canceled_->store(true); }
        bool CanUndo(bool Redo) const override { return !busy_ && !(Redo ? redo_ : undo_).empty(); }
        void Undo(bool Redo) override;
        bool CanSave() const override { return session_ && !busy_ && !input_.readOnlyInputs_; }
        void SaveActive() override { Save(input_.mode_ == "merge" ? 1 : activeSide_, input_.mode_ == "merge" ? input_.output_ : QString{}); }
        void CopySelected(int Source);
        void RemoveSelected(int Side);
        void Save(int Side, const QString& Path = {});
        void OpenCurrent();
        void ExportReport(const QString& Path, bool ReplaceExisting);
        const Request& Input() const { return input_; }
        std::function<void()> stateChanged_;
        std::function<void(Request, QString)> open_;
    private:
        struct Response { std::shared_ptr<ArchiveSession> session_; QString error_; int saved_ = -1; std::optional<Request> open_; QString title_; };
        void run(std::function<Response(const Budget&)> Work, bool Mutation = false);
        void refresh();
        QJsonArray selectedChanges(int Side, bool Remove) const;
        QTableView* table_;
        ArchiveModel* model_ = nullptr;
        Request input_;
        std::shared_ptr<ArchiveSession> session_;
        std::array<QByteArray, 2> saved_;
        std::vector<std::shared_ptr<ArchiveSession>> undo_, redo_;
        QFutureWatcher<Response> watcher_;
        std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
        bool busy_ = false;
        bool outputNeverSaved_ = false;
        int activeSide_ = 1;
    };
}
