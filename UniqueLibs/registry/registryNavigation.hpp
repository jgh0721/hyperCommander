#pragma once

/// 고유 라이브러리
#include "registryModels.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>

namespace hc::registry {
// Resolves addresses through the same lazy cache as both models, one ancestor at
// a time. A newer navigation replaces the pending destination without waiting.
class RegistryNavigation final : public QObject
{
    Q_OBJECT
public:
    RegistryNavigation(RegistryService* Service, RegistryKeyTreeModel* Keys, RegistryItemsModel* Items, QObject* Parent = nullptr);
    bool Navigate(const QString& Path, bool AddHistory = true);
    void Back();
    void Forward();
    void Up();
    void SetView(View SelectedView);
    std::optional<RegistryLocation> Current() const { return current_; }
    View SelectedView() const { return view_; }
    QString Path() const { return current_ ? current_->Path() : QString(); }
    bool CanBack() const { return historyIndex_ > 0; }
    bool CanForward() const { return historyIndex_ + 1 < history_.size(); }
    bool Pending() const { return pending_; }
    QJsonObject CaptureState() const;
    void RestoreState(const QJsonObject& State);
signals:
    void Navigated();
    void Failed(const QString& Path, quint32 Error);
private:
    void advance();
    void commit();
    QPointer<RegistryService> service_;
    QPointer<RegistryKeyTreeModel> keys_;
    QPointer<RegistryItemsModel> items_;
    std::optional<RegistryLocation> current_, destination_;
    View view_ = View::Native64;
    QStringList history_{QString()};
    int historyIndex_ = 0;
    int pendingHistoryIndex_ = -1;
    bool pending_ = false, addHistory_ = true;
};
} // namespace hc::registry
