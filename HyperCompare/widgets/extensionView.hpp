#pragma once
#include <QJsonObject>
#include <QWidget>
namespace hc::compare
{
    class ExtensionView : public QWidget
    {
    public:
        using QWidget::QWidget;
        virtual void SetListFont(const QFont&) {}
    };
    ExtensionView* CreateExtensionView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent);
}
