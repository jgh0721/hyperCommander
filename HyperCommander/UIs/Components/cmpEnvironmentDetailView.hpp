#pragma once
#include "cmpView.hpp"
#include "commonLibs/system/environmentTypes.hpp"
#include <QPointer>
class CEnvironmentView;
class QStackedWidget;
namespace fm::ui { class Label; class SegmentedControl; }
namespace hc::environment { class EnvironmentValueEditor; }
class CEnvironmentDetailView final : public CViewT
{
    Q_OBJECT
public:
    explicit CEnvironmentDetailView(QWidget* Parent = nullptr);
    void SetSource(CEnvironmentView* Source);
    CEnvironmentView* Source() const;
    void Update();
    bool RequestClose();
    void CloseView() override;
    void EnsureKeyboardFocusOnView() const override;
    void ShowData(bool Focus = true);
    void ShowDescription();
    void SetBusy(bool Busy);
    void SaveFinished(const hc::environment::Value& Value, const QString& Error = {});
    hc::environment::EnvironmentValueEditor* Editor() const { return editor_; }
signals:
    void SaveRequested(const hc::environment::Value& Original, const hc::environment::Value& Draft);
    void FollowRequested();
private:
    void updateExpansion();
    QPointer<CEnvironmentView> source_;
    fm::ui::Label* title_;
    fm::ui::Label* notice_;
    fm::ui::Label* description_;
    fm::ui::Label* stages_;
    fm::ui::Label* origin_;
    fm::ui::SegmentedControl* tabs_;
    QStackedWidget* pages_;
    hc::environment::EnvironmentValueEditor* editor_;
    QString shownId_;
    bool busy_ = false;
};
