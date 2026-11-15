#include "stdafx.h"
#include "registryNavigation.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonArray>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryNavigation, "hc.registry.navigation")

namespace hc::registry {
RegistryNavigation::RegistryNavigation(RegistryService* Service, RegistryKeyTreeModel* Keys, RegistryItemsModel* Items, QObject* Parent)
    : QObject(Parent), service_(Service), keys_(Keys), items_(Items)
{
    connect(Service, &RegistryService::Changed, this, [this] { QTimer::singleShot(0, this, &RegistryNavigation::advance); });
}
bool RegistryNavigation::Navigate(const QString& Path, bool AddHistory)
{
    const auto Text = Path.trimmed();
    const bool Computer = Text.isEmpty() || Text == tr("컴퓨터") || Text.compare("Computer", Qt::CaseInsensitive) == 0;
    auto Target = Computer ? std::optional<RegistryLocation>() : RegistryLocation::Parse(Text, view_);
    if (!Computer && !Target) { emit Failed(Path, ERROR_INVALID_NAME); return false; }
    destination_ = Target; pending_ = true; addHistory_ = AddHistory; pendingHistoryIndex_ = -1;
    QTimer::singleShot(0, this, &RegistryNavigation::advance); return true;
}
void RegistryNavigation::advance()
{
    if (!pending_ || !service_ || !keys_ || !items_) return;
    if (!destination_) { commit(); return; }
    QList<RegistryLocation> Chain; auto Cursor = destination_;
    while (Cursor) { Chain.prepend(*Cursor); Cursor = Cursor->Parent(); }
    for (const auto& Key : Chain) {
        auto ItemIndex = items_->Find(Key.Id());
        if (!ItemIndex.isValid()) {
            const auto Parent = Key.Parent();
            if (!Parent) { pending_ = false; emit Failed(destination_->Path(), ERROR_PATH_NOT_FOUND); return; }
            const auto* Cached = service_->Cached(*Parent);
            if (Cached && Cached->loaded && !Cached->pending) {
                pending_ = false; emit Failed(destination_->Path(), Cached->snapshot.error ? Cached->snapshot.error : ERROR_FILE_NOT_FOUND); return;
            }
            return;
        }
        if (Key.Id() == destination_->Id()) { commit(); return; }
        // A link is a leaf, never an implicit shortcut through another hive.
        const auto* Cached = service_->Cached(Key);
        if (Cached && Cached->loaded && Cached->snapshot.value.info.link) {
            pending_ = false; emit Failed(destination_->Path(), ERROR_CANT_ACCESS_FILE); return;
        }
        if (items_->canFetchMore(ItemIndex)) items_->fetchMore(ItemIndex);
        const auto TreeIndex = keys_->Find(Key.Id()); if (keys_->canFetchMore(TreeIndex)) keys_->fetchMore(TreeIndex);
        Cached = service_->Cached(Key);
        if (!Cached || Cached->pending) return;
    }
}
void RegistryNavigation::commit()
{
    current_ = destination_; pending_ = false;
    if (pendingHistoryIndex_ >= 0) historyIndex_ = pendingHistoryIndex_;
    pendingHistoryIndex_ = -1;
    if (addHistory_ && history_.value(historyIndex_) != Path()) {
        history_ = history_.mid(0, historyIndex_ + 1); history_.append(Path());
        if (history_.size() > 100) history_.removeFirst(); historyIndex_ = int(history_.size()) - 1;
    }
    if (current_) {
        // A view switch recreates model nodes while the service cache survives.
        // Request alone is a no-op for cached destinations: hydrate both models.
        const auto Item = items_->Find(current_->Id());
        if (items_->canFetchMore(Item)) items_->fetchMore(Item);
        const auto Key = keys_->Find(current_->Id());
        if (keys_->canFetchMore(Key)) keys_->fetchMore(Key);
        service_->Request(*current_);
    }
    emit Navigated();
}
void RegistryNavigation::Back()
{
    if (CanBack() && Navigate(history_[historyIndex_ - 1], false)) pendingHistoryIndex_ = historyIndex_ - 1;
}
void RegistryNavigation::Forward()
{
    if (CanForward() && Navigate(history_[historyIndex_ + 1], false)) pendingHistoryIndex_ = historyIndex_ + 1;
}
void RegistryNavigation::Up() { if (current_) Navigate(current_->Parent() ? current_->Parent()->Path() : QString()); }
void RegistryNavigation::SetView(View SelectedView)
{
    if (view_ == SelectedView) return;
    const auto SavedPath = Path(); pending_ = false; view_ = SelectedView; current_.reset();
    keys_->SetView(view_); items_->SetView(view_); Navigate(SavedPath, false);
}
QJsonObject RegistryNavigation::CaptureState() const
{
    return {{"path", Path()}, {"view", int(view_)}, {"history", QJsonArray::fromStringList(history_)}, {"historyIndex", historyIndex_}};
}
void RegistryNavigation::RestoreState(const QJsonObject& State)
{
    const int SavedView = State["view"].toInt(); view_ = SavedView == int(View::Wow32) ? View::Wow32 : View::Native64;
    keys_->SetView(view_); items_->SetView(view_);
    history_.clear();
    for (const auto& Item : State["history"].toArray()) {
        if (history_.size() == 100) break;
        const auto Path = Item.toString(); if (Path.isEmpty() || RegistryLocation::Parse(Path, view_)) history_.append(Path);
    }
    if (history_.isEmpty()) history_.append(QString());
    historyIndex_ = std::clamp(State["historyIndex"].toInt(), 0, int(history_.size()) - 1);
    Navigate(State["path"].toString(), false);
}
} // namespace hc::registry
