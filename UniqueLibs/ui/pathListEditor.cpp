#include "stdafx.h"
#include "pathListEditor.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/ReorderList.h>
#include <fmwidgets/PathEntryDelegate.h>
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/ValueInputs.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QStackedWidget>
#include <QItemSelectionModel>
#include <mutex>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcPathListEditor, "hc.ui.pathListEditor")

namespace hc::ui {
struct PathEntryModel::Worker { std::mutex mutex; };
PathEntryModel::PathEntryModel(QObject* Parent) : QAbstractListModel(Parent), worker_(std::make_shared<Worker>())
{
    debounce_.setSingleShot(true); debounce_.setInterval(150);
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (resolver_ && !entries_.isEmpty()) resolveChunk(0, generation_, cancel_);
    });
}
PathEntryModel::~PathEntryModel() { if (cancel_) cancel_->store(true); }
int PathEntryModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(entries_.size()); }
QVariant PathEntryModel::data(const QModelIndex& Index, int Role) const
{
    if (!Index.isValid() || Index.model() != this || Index.row() < 0 || Index.row() >= entries_.size()) return {};
    const auto& Entry = entries_[Index.row()];
    if (Role == Qt::DisplayRole) return Entry.raw.isEmpty() ? tr("(빈 항목)") : Entry.raw;
    if (Role == Qt::EditRole) return Entry.raw;
    if (Role == fm::ui::PathEntryDelegate::ExpandedPathRole) return Entry.resolution.expanded;
    if (Role == fm::ui::PathEntryDelegate::PathStatusRole) return int(Entry.resolution.status);
    if (Role == fm::ui::PathEntryDelegate::PathWarningRole) return Entry.resolution.warning;
    if (Role == Qt::ToolTipRole || Role == Qt::AccessibleDescriptionRole)
        return Entry.raw + '\n' + Entry.resolution.expanded + '\n' + Entry.resolution.warning;
    return {};
}
Qt::ItemFlags PathEntryModel::flags(const QModelIndex& Index) const
{
    return Index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable : Qt::NoItemFlags;
}
bool PathEntryModel::setData(const QModelIndex& Index, const QVariant& Value, int Role)
{
    if (Role != Qt::EditRole || !Index.isValid() || Index.model() != this || Index.row() >= entries_.size()) return false;
    const auto Text = Value.toString(); if (Text.contains(QChar(0))) return false;
    if (entries_[Index.row()].raw == Text) return true;
    entries_[Index.row()].raw = Text; emit dataChanged(Index, Index); resolve(); emit ValueChanged(); return true;
}
bool PathEntryModel::moveRows(const QModelIndex& Source, int Row, int Count, const QModelIndex& Destination, int Target)
{
    if (Source.isValid() || Destination.isValid() || Count != 1 || Row < 0 || Row >= entries_.size()
        || Target < 0 || Target > entries_.size() || Target == Row || Target == Row + 1) return false;
    if (!beginMoveRows({}, Row, Row, {}, Target)) return false;
    auto Entry = entries_.takeAt(Row); entries_.insert(Target > Row ? Target - 1 : Target, std::move(Entry)); endMoveRows();
    resolve(); emit ValueChanged(); return true;
}
bool PathEntryModel::SetValue(const QString& Value)
{
    if (Value.contains(QChar(0)) || Value.count(';') >= 65536) return false;
    beginResetModel(); entries_.clear();
    for (const auto& Raw : Value.split(';', Qt::KeepEmptyParts)) entries_.append({Raw, {}});
    endResetModel(); resolve(); emit ValueChanged(); return true;
}
QString PathEntryModel::Value() const
{
    QStringList Result; for (const auto& Entry : entries_) Result.append(Entry.raw); return Result.join(';');
}
void PathEntryModel::SetResolver(PathResolver Resolver) { resolver_ = std::move(Resolver); resolve(); }
bool PathEntryModel::Insert(int Row, const QString& Raw)
{
    if (Row < 0 || Row > entries_.size() || entries_.size() >= 65536 || Raw.contains(QChar(0))) return false;
    beginInsertRows({}, Row, Row); entries_.insert(Row, {Raw, {}}); endInsertRows(); resolve(); emit ValueChanged(); return true;
}
bool PathEntryModel::Remove(int Row)
{
    if (Row < 0 || Row >= entries_.size()) return false;
    beginRemoveRows({}, Row, Row); entries_.removeAt(Row); endRemoveRows(); resolve(); emit ValueChanged(); return true;
}
void PathEntryModel::resolve()
{
    if (cancel_) cancel_->store(true);
    cancel_ = std::make_shared<std::atomic_bool>(false); ++generation_;
    for (auto& Entry : entries_) Entry.resolution = {};
    if (!entries_.isEmpty()) emit dataChanged(index(0), index(int(entries_.size()) - 1));
    debounce_.start();
}
void PathEntryModel::resolveChunk(int Start, quint64 Generation, const std::shared_ptr<std::atomic_bool>& Cancel)
{
    QStringList Input; const auto End = qMin(Start + 16, int(entries_.size()));
    for (int I = Start; I < End; ++I) Input.append(entries_[I].raw);
    auto* Future = new QFutureWatcher<QList<PathResolution>>(this);
    connect(Future, &QFutureWatcherBase::finished, this, [this, Future, Start, Generation, Cancel] {
        const auto Result = Future->result(); Future->deleteLater();
        if (Cancel->load() || Generation != generation_) return;
        for (int I = 0; I < Result.size(); ++I) entries_[Start + I].resolution = Result[I];
        if (!Result.isEmpty()) emit dataChanged(index(Start), index(Start + int(Result.size()) - 1));
        if (Start + Result.size() < entries_.size()) resolveChunk(Start + int(Result.size()), Generation, Cancel);
    });
    Future->setFuture(QtConcurrent::run([Worker = worker_, Resolve = resolver_, Input, Cancel] {
        QList<PathResolution> Result; std::lock_guard Lock(Worker->mutex);
        for (const auto& Raw : Input) {
            if (Cancel->load()) break;
            try { Result.append(Resolve(Raw)); }
            catch (...) { Result.append({{}, PathResolution::Status::Unresolved, QCoreApplication::translate("PathEntryModel", "경로를 확인하지 못했습니다.")}); }
        }
        return Result;
    }));
}
PathListEditor::PathListEditor(QWidget* Parent) : QWidget(Parent), model_(new PathEntryModel(this))
{
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    pages_ = new QStackedWidget(this); Layout->addWidget(pages_, 1);
    list_ = new fm::ui::ReorderListView(this); setFocusProxy(list_); list_->setObjectName("hc.pathList.rows"); list_->setModel(model_);
    list_->SetMoveModifiers(Qt::ControlModifier); list_->setItemDelegate(new fm::ui::PathEntryDelegate(list_)); pages_->addWidget(list_);
    auto* RawPage = new QWidget(this); auto* RawLayout = new QVBoxLayout(RawPage); RawLayout->setContentsMargins(0, 0, 0, 0);
    raw_ = new fm::ui::TextField(RawPage); raw_->setObjectName("hc.pathList.raw"); raw_->setMonospace(true); raw_->setMaxLength(32 * 1024 * 1024);
    RawLayout->addWidget(raw_); RawLayout->addStretch(); pages_->addWidget(RawPage);
    editor_ = new fm::ui::PathEdit(this); editor_->setObjectName("hc.pathList.entry"); editor_->setHistoryVisible(false); Layout->addWidget(editor_);
    auto* Actions = new QHBoxLayout; Layout->addLayout(Actions);
    auto* Add = new fm::ui::Button(tr("추가"), this); remove_ = new fm::ui::Button(tr("제거"), this);
    auto* Up = new fm::ui::Button(tr("위로"), this); auto* Down = new fm::ui::Button(tr("아래로"), this);
    for (auto* Button : {Add, remove_, Up, Down}) { Button->setCompact(true); Actions->addWidget(Button); }
    auto* Clean = new fm::ui::Button(tr("없는 폴더 정리"), this); Clean->setObjectName("hc.pathList.clean"); Clean->setCompact(true); Actions->addWidget(Clean);
    auto* TextMode = new fm::ui::Button(tr("텍스트로 편집"), this); TextMode->setObjectName("hc.pathList.textMode"); TextMode->setCompact(true); Actions->addWidget(TextMode);
    Actions->addStretch();
    notice_ = new fm::ui::Label(tr("↑/↓ 선택 · Ctrl+↑/↓ 또는 끌기 핸들로 순서 변경. 빈 항목과 원문은 직접 고치기 전까지 보존합니다."), fm::ui::Label::Help, this);
    Layout->addWidget(notice_);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { syncEditor(); });
    connect(editor_, &fm::ui::PathEdit::pathChanged, this, [this](const QString& Text) { model_->setData(list_->currentIndex(), Text); });
    connect(Add, &QPushButton::clicked, this, [this] {
        const int Row = list_->currentIndex().isValid() ? list_->currentIndex().row() + 1 : model_->rowCount();
        if (model_->Insert(Row)) { list_->setCurrentIndex(model_->index(Row)); editor_->lineEdit()->setFocus(); }
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        const int Row = list_->currentIndex().row(); if (model_->Remove(Row) && model_->rowCount()) list_->setCurrentIndex(model_->index(qMin(Row, model_->rowCount() - 1)));
        syncEditor();
    });
    connect(Up, &QPushButton::clicked, this, [this] { list_->MoveRow(list_->currentIndex().row(), list_->currentIndex().row() - 1); });
    connect(Down, &QPushButton::clicked, this, [this] { list_->MoveRow(list_->currentIndex().row(), list_->currentIndex().row() + 1); });
    connect(model_, &PathEntryModel::ValueChanged, this, [this] {
        if (!textMode_) { const QSignalBlocker Block(raw_); raw_->setText(model_->Value()); }
        emit ModifiedChanged(IsModified());
    });
    connect(raw_, &QLineEdit::textChanged, this, [this](const QString& Text) {
        if (!textMode_) return;
        if (!model_->SetValue(Text)) notice_->setText(tr("이 원문은 목록으로 표시할 수 없습니다. 텍스트 편집을 유지합니다."));
        emit ModifiedChanged(IsModified());
    });
    connect(TextMode, &QPushButton::clicked, this, [this, TextMode, Add, Up, Down, Clean] {
        if (textMode_ && !model_->SetValue(raw_->text())) { notice_->setText(tr("이 원문은 목록으로 표시할 수 없습니다.")); return; }
        if (!textMode_) { const QSignalBlocker Block(raw_); raw_->setText(model_->Value()); }
        textMode_ = !textMode_; setFocusProxy(textMode_ ? static_cast<QWidget*>(raw_) : list_); pages_->setCurrentIndex(textMode_ ? 1 : 0); editor_->setVisible(!textMode_);
        for (auto* Button : {Add, remove_, Up, Down, Clean}) Button->setEnabled(!textMode_);
        TextMode->setText(textMode_ ? tr("목록으로 편집") : tr("텍스트로 편집"));
        if (textMode_) raw_->setFocus(); else { if (model_->rowCount()) list_->setCurrentIndex(model_->index(0)); syncEditor(); list_->setFocus(); }
    });
    connect(Clean, &QPushButton::clicked, this, [this] {
        QStringList Kept; int Missing = 0;
        for (int Row = 0; Row < model_->rowCount(); ++Row) {
            const auto Index = model_->index(Row);
            if (Index.data(fm::ui::PathEntryDelegate::PathStatusRole).toInt() == int(PathResolution::Status::Missing)) ++Missing;
            else Kept.append(Index.data(Qt::EditRole).toString());
        }
        if (Missing) model_->SetValue(Kept.join(';'));
        notice_->setText(tr("없음을 확인한 폴더 %1개를 초안에서 제거했습니다. 미확인 항목은 유지합니다.").arg(Missing)); syncEditor();
    });
    syncEditor();
}
bool PathListEditor::SetValue(const QString& Value)
{
    const auto Previous = original_; original_ = Value;
    if (!model_->SetValue(Value)) { original_ = Previous; return false; }
    { const QSignalBlocker Block(raw_); raw_->setText(Value); }
    if (model_->rowCount()) list_->setCurrentIndex(model_->index(0)); syncEditor(); emit ModifiedChanged(IsModified()); return true;
}
QString PathListEditor::Value() const { return textMode_ ? raw_->text() : model_->Value(); }
bool PathListEditor::IsModified() const { return Value() != original_; }
void PathListEditor::SetResolver(PathResolver Resolver) { model_->SetResolver(std::move(Resolver)); }
void PathListEditor::syncEditor()
{
    const auto Index = list_->currentIndex(); const QSignalBlocker Block(editor_);
    editor_->setEnabled(Index.isValid()); remove_->setEnabled(Index.isValid()); editor_->setPath(Index.data(Qt::EditRole).toString());
}
}
