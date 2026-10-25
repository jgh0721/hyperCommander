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
        virtual void SetTextFont(const QFont&) {}
        virtual bool Busy() const { return false; }
        virtual bool Dirty() const { return false; }
        virtual void Cancel() {}
        virtual bool CanUndo(bool) const { return false; }
        virtual void Undo(bool) {}
        virtual bool CanSave() const { return false; }
        virtual void SaveActive() {}
    };
    ExtensionView* CreateExtensionView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent);
}
