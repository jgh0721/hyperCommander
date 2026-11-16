#include "stdafx.h"
#include "environmentModel.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentModel, "hc.environment.model")
namespace hc::environment {
EnvironmentModel::EnvironmentModel(QObject* Parent) : QAbstractItemModel(Parent) {}
bool EnvironmentModel::ListVariable(const std::wstring& Name)
{
    return SameName(Name, L"Path") || SameName(Name, L"LibPath") || SameName(Name, L"Os2LibPath") || SameName(Name, L"PSModulePath") || SameName(Name, L"PATHEXT");
}
void EnvironmentModel::SetDescriptions(const QHash<QString, QString>& Descriptions)
{
    descriptions_ = Descriptions;
    for (auto& Row : rows_) Row.description_ = descriptions_.value(Row.name_.toCaseFolded());
    if (!rows_.empty()) emit dataChanged(index(0, 2), index(int(rows_.size()) - 1, 2), {Qt::DisplayRole, Qt::ToolTipRole});
}
void EnvironmentModel::SetSnapshot(const EnvironmentSnapshot& Snapshot, EnvironmentViewScope Source, const ValueMap& Other, bool Expanded)
{
    beginResetModel(); rows_.clear(); scope_ = Source;
    std::vector<Value> Values;
    if (Source == EnvironmentViewScope::User) Values = Snapshot.input_.user_;
    else if (Source == EnvironmentViewScope::System) Values = Snapshot.input_.system_;
    else if (Source == EnvironmentViewScope::Effective) {
        for (const auto& [Name, Value] : Snapshot.merged_.values_) Values.push_back({Name, Value.value_, ValueType::String, Value.origins_.back().value_.scope_});
    } else for (const auto& [Name, Text] : Other) Values.push_back({Name, Text, ValueType::String, Scope::Process});
    std::set<std::wstring, NameLess> Sensitive;
    for (const auto& Group : {Snapshot.input_.system_, Snapshot.input_.user_, Values}) for (const auto& Value : Group) if (SensitiveName(Value.name_)) Sensitive.insert(Value.name_);
    // Derived values can reveal a secret even when their own names look harmless.
    for (size_t Pass = 0; Pass <= Values.size() + Snapshot.merged_.values_.size(); ++Pass) {
        const auto Before = Sensitive.size();
        for (const auto& [Name, Effective] : Snapshot.merged_.values_) for (const auto& Origin : Effective.origins_)
            for (const auto& Ref : References(Origin.value_.raw_)) if (Sensitive.contains(Ref)) Sensitive.insert(Name);
        for (const auto& Group : {Snapshot.input_.system_, Snapshot.input_.user_, Values})
            for (const auto& Value : Group) for (const auto& Ref : References(Value.raw_))
                if (SensitiveName(Ref) || Sensitive.contains(Ref)) Sensitive.insert(Value.name_);
        if (Before == Sensitive.size()) break;
    }
    if (Source == EnvironmentViewScope::Process) for (const auto& [Name, Value] : Snapshot.merged_.values_)
        if (!Other.contains(Name)) Values.push_back({Name, {}, ValueType::String, Scope::Process});
    std::stable_sort(Values.begin(), Values.end(), [](const auto& Left, const auto& Right) {
        const bool A = SameName(Left.name_, L"Path"), B = SameName(Right.name_, L"Path"); return A != B ? A : NameLess{}(Left.name_, Right.name_);
    });
    for (const auto& Value : Values) {
        Row Row; Row.value_ = Value; Row.name_ = QString::fromStdWString(Value.name_);
        Row.id_ = QString::number(int(Source)) + '/' + Row.name_.toCaseFolded(); Row.masked_ = Sensitive.contains(Value.name_) && !maskExceptions_.contains(Row.name_.toCaseFolded());
        auto Text = Value.raw_;
        if (Expanded && Value.type_ == ValueType::ExpandString) Text = ExpandAtStage(Text, Source == EnvironmentViewScope::System ? Snapshot.merged_.afterSystemStrings_ : Snapshot.merged_.beforeUserExpansion_);
        Row.text_ = Row.masked_ ? QStringLiteral("••••••••") : QString::fromStdWString(Text);
        Row.description_ = descriptions_.value(Row.name_.toCaseFolded());
        if (Source == EnvironmentViewScope::Process) {
            const auto Current = Snapshot.merged_.values_.find(Value.name_);
            Row.comparison_ = !Other.contains(Value.name_) ? tr("프로세스에 없음") : Current == Snapshot.merged_.values_.end() ? tr("프로세스에만") : Current->second.value_ == Value.raw_ ? tr("같음") : tr("다름");
        }
        if (Source == EnvironmentViewScope::Profile) {
            const auto Current = Snapshot.merged_.values_.find(Value.name_);
            Row.comparison_ = Current == Snapshot.merged_.values_.end() ? tr("추가") : Current->second.value_ == Value.raw_ ? tr("같음") : tr("변경");
        }
        if (ListVariable(Value.name_) && !Row.masked_) {
            const auto Raw = ParseList(Value.raw_), Display = ParseList(Text);
            for (size_t I = 0; I < Raw.entries_.size(); ++I) {
                EnvironmentModel::Row Child; Child.value_ = Value; Child.entry_ = int(I); Child.id_ = Row.id_ + '/' + QString::number(I);
                Child.name_ = QString::number(I + 1); Child.text_ = QString::fromStdWString(I < Display.entries_.size() ? Display.entries_[I] : Raw.entries_[I]);
                Row.children_.push_back(std::move(Child));
            }
        }
        rows_.push_back(std::move(Row));
    }
    endResetModel();
}
void EnvironmentModel::Clear() { beginResetModel(); rows_.clear(); endResetModel(); }
void EnvironmentModel::SetProblems(const std::vector<PathEntry>& Entries, const std::wstring& Name)
{
    for (auto& Row : rows_) {
        if (!Row.value_ || !SameName(Row.value_->name_, Name)) continue;
        Row.problem_ = false;
        for (auto& Child : Row.children_) { Child.problem_ = false; Child.description_.clear(); }
        for (auto& Child : Row.children_) for (const auto& Entry : Entries) if (Child.entry_ && size_t(*Child.entry_) == Entry.index_ && Row.value_->scope_ == Entry.scope_) {
            Child.problem_ = !Entry.issues_.empty(); Row.problem_ |= Child.problem_;
            QStringList Issues; const QStringList Labels{tr("없음"), tr("중복"), tr("가려짐"), tr("정의 안 된 변수"), tr("확장 순서"), tr("따옴표"), tr("빈 항목"), tr("누구나 쓰기"), tr("setx로 합친 흔적")};
            for (auto Issue : Entry.issues_) Issues.append(Labels.value(int(Issue)));
            if (Entry.probe_.status_ == ProbeStatus::Denied) Issues.append(tr("접근 거부"));
            if (Entry.probe_.status_ == ProbeStatus::Timeout) Issues.append(tr("확인 시간 초과"));
            Child.description_ = Issues.join(" · ");
        }
    }
    for (int I = 0; I < int(rows_.size()); ++I) {
        emit dataChanged(index(I, 0), index(I, 3));
        const auto Parent = index(I, 0); if (rowCount(Parent)) emit dataChanged(index(0, 0, Parent), index(rowCount(Parent) - 1, 3, Parent));
    }
}
const EnvironmentModel::Row* EnvironmentModel::Item(const QModelIndex& Index) const
{
    if (!Index.isValid() || Index.model() != this || Index.row() < 0) return nullptr;
    const auto Parent = Index.internalId();
    if (!Parent) return Index.row() < int(rows_.size()) ? &rows_[Index.row()] : nullptr;
    if (Parent > rows_.size()) return nullptr;
    const auto& Children = rows_[Parent - 1].children_; return Index.row() < int(Children.size()) ? &Children[Index.row()] : nullptr;
}
QModelIndex EnvironmentModel::Find(const QString& Id) const
{
    for (int I = 0; I < int(rows_.size()); ++I) {
        if (rows_[I].id_ == Id) return index(I, 0);
        for (int J = 0; J < int(rows_[I].children_.size()); ++J) if (rows_[I].children_[J].id_ == Id) return index(J, 0, index(I, 0));
    }
    return {};
}
QModelIndex EnvironmentModel::index(int Row, int Column, const QModelIndex& Parent) const
{
    if (Row < 0 || Row >= rowCount(Parent) || Column < 0 || Column >= 4) return {};
    return createIndex(Row, Column, Parent.isValid() ? quintptr(Parent.row() + 1) : quintptr(0));
}
QModelIndex EnvironmentModel::parent(const QModelIndex& Index) const { return Item(Index) && Index.internalId() ? createIndex(int(Index.internalId()) - 1, 0, quintptr(0)) : QModelIndex(); }
int EnvironmentModel::rowCount(const QModelIndex& Parent) const
{
    if (!Parent.isValid()) return int(rows_.size());
    const auto* Row = Item(Parent); return Row && Parent.column() == 0 && !Parent.internalId() ? int(Row->children_.size()) : 0;
}
QVariant EnvironmentModel::data(const QModelIndex& Index, int RoleValue) const
{
    const auto* Row = Item(Index); if (!Row) return {};
    if (RoleValue == IdRole) return Row->id_; if (RoleValue == ProblemRole) return Row->problem_; if (RoleValue == MaskedRole) return Row->masked_;
    if (RoleValue == NameRole) return Row->value_ ? QString::fromStdWString(Row->value_->name_) : Row->name_;
    if (RoleValue == EntryRole) return Row->entry_ ? QVariant(*Row->entry_) : QVariant();
    if (RoleValue == ScopeRole) return Row->value_ ? int(Row->value_->scope_) : -1;
    if (RoleValue == GridSortRole) return Index.internalId() ? QVariant(Index.row()) : Index.column() == 0 ? QVariant(Row->name_.compare("Path", Qt::CaseInsensitive) == 0 ? QString() : Row->name_.toCaseFolded()) : data(Index, Qt::DisplayRole);
    if (RoleValue == Qt::ToolTipRole) return Row->masked_ ? tr("값이 가려져 있습니다.") : Row->description_;
    if (RoleValue != Qt::DisplayRole) return {};
    switch (Index.column()) { case 0: return Row->name_; case 1: return Row->text_; case 2: return Row->description_; case 3: return Row->comparison_; default: return {}; }
}
QVariant EnvironmentModel::headerData(int Section, Qt::Orientation Orientation, int RoleValue) const
{
    if (Orientation != Qt::Horizontal || RoleValue != Qt::DisplayRole) return {};
    return QStringList{tr("이름"), tr("값"), tr("설명 · 점검"), tr("비교")}.value(Section);
}
Qt::ItemFlags EnvironmentModel::flags(const QModelIndex& Index) const { return Item(Index) ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags; }
}
