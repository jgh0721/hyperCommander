#include "stdafx.h"
#include "registryHistoryModel.hpp"
#include "commonLibs/system/environmentTypes.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "registryModels.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QFutureWatcher>
#include <QtConcurrent>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryHistoryModel, "hc.registry.historyModel")
namespace hc::registry {
namespace {
QString ValueText(const std::optional<RegistryValue>& Value, bool Sensitive)
{
    if (!Value) return QObject::tr("(없음)");
    if (Sensitive || hc::environment::SensitiveName(Value->name.toStdWString())) return QStringLiteral("••••••••");
    const auto Decoded = RegistryValueCodec::DecodeText(*Value);
    const auto Number = RegistryValueCodec::DecodeNumber(*Value);
    QString Text = Decoded.lossless ? Decoded.text : Number ? QString::number(Number.value) : QString::fromLatin1(Value->bytes.left(48).toHex(' '));
    Text.replace('\n', QStringLiteral(" ↵ "));
    return Text.size() > 160 ? Text.left(160) + QStringLiteral("…") : Text;
}
QString StateText(const QString& State)
{
    if (State == "committed") return QObject::tr("완료");
    if (State == "undone") return QObject::tr("되돌림");
    if (State == "partial") return QObject::tr("일부 완료");
    if (State == "conflict") return QObject::tr("충돌");
    if (State == "prepared") return QObject::tr("준비됨");
    return QObject::tr("복구 확인 필요");
}
}
RegistryHistoryModel::RegistryHistoryModel(QObject* Parent) : QAbstractItemModel(Parent) {}
void RegistryHistoryModel::Reload(const QString& Directory, const QString& CallerSid, const QString& MachineId)
{
    const auto Generation = ++generation_; loading_ = true;
    auto* Watcher = new QFutureWatcher<RegistryResult<QList<RegistryHistoryRecord>>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (Generation != generation_) return;
        loading_ = false; error_ = Result.error; SetRecords(Result.value); emit Reloaded();
    });
    Watcher->setFuture(QtConcurrent::run([Directory, CallerSid, MachineId] {
        RegistryChangeStore Store(Directory); auto Records = Store.History();
        if (CallerSid.isEmpty() || MachineId.isEmpty()) return Records;
        WinRegistryBackend Backend; RegistryTransaction Transaction(Backend); bool Checked = false;
        for (auto Record : Records.value) {
            if (Record.inFlight < 0 || Record.plan.callerSid != CallerSid || Record.plan.machineId != MachineId) continue;
            // Recovery only compares recorded images. It never replays a write
            // or starts elevation on startup. Unreadable/conflicting records stay blocked.
            Store.Recover(Record, Transaction); Checked = true;
        }
        return Checked ? Store.History() : Records;
    }));
}
void RegistryHistoryModel::SetRecords(QList<RegistryHistoryRecord> Records)
{
    beginResetModel(); records_ = std::move(Records);
    std::sort(records_.begin(), records_.end(), [](const auto& A, const auto& B) {
        return A.sequence == B.sequence ? A.plan.createdUtc > B.plan.createdUtc : A.sequence > B.sequence;
    });
    endResetModel();
}
QModelIndex RegistryHistoryModel::index(int Row, int Column, const QModelIndex& Parent) const
{
    if (Row < 0 || Column < 0 || Column >= ColumnCount || Row >= rowCount(Parent)) return {};
    return createIndex(Row, Column, Parent.isValid() ? quintptr(Parent.row() + 1) : quintptr(0));
}
QModelIndex RegistryHistoryModel::parent(const QModelIndex& Index) const
{
    if (!Index.isValid() || Index.model() != this || !Index.internalId()) return {};
    return createIndex(int(Index.internalId() - 1), 0, quintptr(0));
}
int RegistryHistoryModel::rowCount(const QModelIndex& Parent) const
{
    if (!Parent.isValid()) return records_.size();
    if (Parent.model() != this || Parent.column() != 0 || Parent.internalId() || Parent.row() < 0 || Parent.row() >= records_.size()) return 0;
    return records_[Parent.row()].plan.steps.size();
}
int RegistryHistoryModel::columnCount(const QModelIndex&) const { return ColumnCount; }
std::optional<RegistryHistoryRecord> RegistryHistoryModel::Record(const QModelIndex& Index) const
{
    if (!Index.isValid() || Index.model() != this) return {};
    const auto Row = Index.internalId() ? qsizetype(Index.internalId() - 1) : Index.row();
    if (Row < 0 || Row >= records_.size()) return {};
    if (Index.internalId() && (Index.row() < 0 || Index.row() >= records_[Row].plan.steps.size())) return {};
    return records_[Row];
}
std::optional<RegistryStep> RegistryHistoryModel::SelectedStep(const QModelIndex& Index) const
{
    const auto Item = Record(Index); if (!Item || Item->plan.steps.isEmpty()) return {};
    return Item->plan.steps[Index.internalId() ? Index.row() : 0];
}
QVariant RegistryHistoryModel::data(const QModelIndex& Index, int RoleValue) const
{
    const auto Found = Record(Index); if (!Found) return {};
    const auto& Record = *Found;
    const bool Child = Index.internalId() != 0;
    const auto StepIndex = Child ? Index.row() : 0;
    if (RoleValue == RecordIdRole) return Record.plan.id;
    if (RoleValue == RegistryTreeModel::IdRole) return Record.plan.id + (Child ? "/step/" + QString::number(StepIndex) : QString());
    if (RoleValue == StateRole) return Record.state;
    if (RoleValue == BackupRole) return Record.directory;
    if (RoleValue == RegistryTreeModel::KindRole) return int(RegistryTreeModel::Kind::Status);
    if (RoleValue == Qt::ToolTipRole) return StateText(Record.state) + "\n" + Record.directory;
    if (RoleValue == RegistryTreeModel::LocationRole) return Record.plan.steps.isEmpty() ? QString() : Record.plan.steps[StepIndex].location.Path();
    if (RoleValue == RegistryTreeModel::GridSortRole) return Index.column() == Time ? QVariant::fromValue(Child ? quint64(StepIndex) : Record.sequence) : data(Index, Qt::DisplayRole);
    if (RoleValue != Qt::DisplayRole) return {};
    if (Index.column() == Time && Child) return tr("단계 %1").arg(StepIndex + 1);
    if (Index.column() == Time) return QDateTime::fromString(Record.plan.createdUtc, Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
    if (Index.column() == Action && Child) {
        const auto& Step = Record.plan.steps[StepIndex];
        if (Step.kind == StepKind::ObserveValue) return tr("원본 값 확인");
        if (Step.kind == StepKind::PolicyFile) return Step.policyPhase == 0 ? tr("로컬 정책 저장") : Step.policyPhase == 1 ? tr("정책 새로 고침") : tr("적용 값 검증");
        return Step.kind == StepKind::RenameKey ? tr("키 이름 변경") : Step.kind == StepKind::Value
            ? (!Step.after ? tr("값 삭제") : !Step.before ? tr("값 만들기") : tr("값 변경"))
            : Step.existsAfter ? tr("키 만들기") : tr("키 삭제");
    }
    if (Index.column() == Action) {
        const auto Verb = Record.plan.purpose == "undo" ? tr("되돌리기") : Record.plan.purpose == "redo" ? tr("다시 실행") : tr("변경");
        return Verb + " · " + StateText(Record.state);
    }
    if (Record.plan.steps.isEmpty()) return {};
    const auto& Step = Record.plan.steps[StepIndex];
    if (Step.kind == StepKind::PolicyFile && Index.column() == Path) return Step.location.Path() + " › Registry.pol";
    if (Index.column() == Path) return Step.location.Path() + ((Step.kind == StepKind::Value || Step.kind == StepKind::ObserveValue) ? " › " + (Step.name.isEmpty() ? tr("(기본값)") : Step.name) : QString());
    if (Index.column() == Change) {
        const auto First = Step.kind == StepKind::ObserveValue ? tr("원본 유지 · %1").arg(ValueText(Step.before, Step.sensitive)) : Step.kind == StepKind::PolicyFile ? (Step.policyPhase == 0 ? tr("Registry.pol 저장 · 원본/변경 파일 백업") : Step.policyPhase == 1 ? tr("정책 서비스 완료 대기") : tr("정책 파일과 실제 값 일치 확인")) : Step.kind == StepKind::Value ? ValueText(Step.before, Step.sensitive) + QStringLiteral(" → ") + ValueText(Step.after, Step.sensitive)
            : Step.kind == StepKind::RenameKey ? Step.location.key.section('\\', -1) + QStringLiteral(" → ") + Step.name
            : Step.existsAfter ? tr("키 만들기") : tr("키 삭제");
        return !Child && Record.plan.steps.size() > 1 ? First + tr(" · %1단계").arg(Record.plan.steps.size()) : First;
    }
    return {};
}
QVariant RegistryHistoryModel::headerData(int Section, Qt::Orientation Orientation, int RoleValue) const
{
    if (Orientation != Qt::Horizontal || RoleValue != Qt::DisplayRole) return {};
    const QStringList Labels{tr("시각"), tr("동작"), tr("키 › 값"), tr("이전 → 이후")};
    return Labels.value(Section);
}
std::optional<RegistryHistoryRecord> RegistryHistoryModel::UndoCandidate() const
{
    for (const auto& Record : records_)
        if (Record.plan.purpose == "edit" && Record.inFlight < 0 && Record.completed > 0
            && (Record.state == "committed" || Record.state == "partial" || Record.state == "conflict")) return Record;
    return {};
}
std::optional<QPair<RegistryHistoryRecord, RegistryHistoryRecord>> RegistryHistoryModel::RedoCandidate() const
{
    for (const auto& Undo : records_) {
        if (Undo.plan.purpose != "undo" || Undo.state != "committed") continue;
        for (const auto& Original : records_)
            if (Original.plan.id == Undo.plan.parentId && Original.state == "undone" && Original.redoAllowed) return QPair{Original, Undo};
    }
    return {};
}
}