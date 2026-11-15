#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QSortFilterProxyModel>

namespace hc::registry {
class RegistryFilterProxy final : public QSortFilterProxyModel
{
public:
    explicit RegistryFilterProxy(QObject* Parent = nullptr);
    void SetText(const QString& Text);
    QString Text() const { return text_; }
    void SetKnownOnly(bool KnownOnly);
    bool KnownOnly() const { return knownOnly_; }
protected:
    bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override;
private:
    QString text_;
    bool knownOnly_ = false;
};
} // namespace hc::registry
