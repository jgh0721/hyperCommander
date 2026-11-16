#pragma once
#include "environmentStorage.hpp"
#include <QObject>
#include <QVariant>
#include <QPointer>
#include <functional>
class QMainWindow;
class CmpPanel;
class CEnvironmentView;
class CEnvironmentDetailView;
class CommandUiBridge;
namespace hc::environment {
class EnvironmentCommands final : public QObject
{
    Q_OBJECT
public:
    using PanelAccessor = std::function<CmpPanel*()>;
    EnvironmentCommands(QMainWindow* Main, CommandUiBridge* Bridge, PanelAccessor Active);
    static bool Handles(const QString& Command);
    bool Execute(const QString& Command, const QVariant& Argument = {});
    CEnvironmentView* Open(const QVariant& Argument = {});
    CEnvironmentView* ActiveView() const;
    bool Active() const;
    bool Busy() const { return busy_; }
    bool DetailOpen() const;
    void ToggleDetail();
    bool CloseDetail();
    void RefreshState();
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    CEnvironmentDetailView* openDetail(CEnvironmentView* View);
    CmpPanel* detailPanel() const;
    void change(CEnvironmentView* View, QList<EnvironmentChange> Changes, CEnvironmentDetailView* Detail = nullptr);
    void apply(CEnvironmentView* View, EnvironmentPreview Preview, CEnvironmentDetailView* Detail = nullptr, std::optional<Value> Saved = {});
    void history(CEnvironmentView* View);
    void profile(CEnvironmentView* View, const QString& Operation);
    void undoRedo(CEnvironmentView* View, bool Redo);
    void follow(CEnvironmentView* View);
    void transfer(CEnvironmentView* View, bool Import);
    void process(CEnvironmentView* View, bool StartHelper = false);
    QString directory() const;
    QPointer<QMainWindow> main_;
    CommandUiBridge* bridge_;
    PanelAccessor active_;
    bool busy_ = false;
};
}
