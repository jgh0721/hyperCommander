#include "workspaceChromeController.hpp"
#include <QMenuBar>
#include "UniqueLibs/qtMessageDialog.hpp"
#include <QMenu>
#include <QToolBar>
#include <QWidgetAction>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QInputDialog>
#include <QSignalBlocker>
#include "core/ToolbarIcons.h"
#include <fmstyle/ThemeManager.h>

namespace
{
    void SetMenuIcon(QAction* Action, const QString& Name)
    {
        const auto Refresh = [Action, Name] {
            Action->setIcon(ToolbarIcons::iconByName(Name));
            Action->setIconVisibleInMenu(true);
        };
        Refresh();
        QObject::connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, Action, Refresh);
    }
    void AddMenuControl(QMenu* Menu, QAction* Source, const QString& Label)
    {
        auto* WidgetAction = qobject_cast<QWidgetAction*>(Source);
        auto* Widget = WidgetAction ? WidgetAction->defaultWidget() : nullptr;
        if (auto* Combo = qobject_cast<QComboBox*>(Widget)) {
            auto* Choices = Menu->addMenu(Label.isEmpty() ? Combo->accessibleName() : Label);
            SetMenuIcon(Choices->menuAction(), Combo->objectName() == "wordWrapMode" ? "word-wrap" : "view-source");
            QObject::connect(Choices, &QMenu::aboutToShow, Choices, [Choices, Combo] {
                Choices->clear();
                for (int Index = 0; Index < Combo->count(); ++Index) {
                    auto* Choice = Choices->addAction(Combo->itemText(Index)); Choice->setCheckable(true); Choice->setChecked(Index == Combo->currentIndex());
                    QObject::connect(Choice, &QAction::triggered, Choices, [Combo, Index] {
                        Combo->setCurrentIndex(Index); emit Combo->activated(Index); emit Combo->textActivated(Combo->currentText());
                    });
                }
            });
            return;
        }
        if (auto* Spin = qobject_cast<QSpinBox*>(Widget)) {
            auto* Item = Menu->addAction(Label + QStringLiteral("…"));
            SetMenuIcon(Item, "view-source");
            QObject::connect(Item, &QAction::triggered, Menu, [Spin, Label] {
                bool Accepted = false;
                const int Value = hc::ui::InputDialog::getInt(Spin, Label, Label, Spin->value(), Spin->minimum(), Spin->maximum(), Spin->singleStep(), &Accepted);
                if (Accepted) Spin->setValue(Value);
            }); return;
        }
        if (auto* Check = qobject_cast<QCheckBox*>(Widget)) {
            auto* Item = Menu->addAction(Check->toolTip().isEmpty() ? Check->text() : Check->toolTip());
            SetMenuIcon(Item, "view-source");
            Item->setCheckable(true); Item->setChecked(Check->isChecked());
            QObject::connect(Item, &QAction::triggered, Check, &QCheckBox::setChecked);
            QObject::connect(Check, &QCheckBox::toggled, Item, &QAction::setChecked); return;
        }
        if (Widget) return;
        // A toolbar QWidgetAction has only one default widget. Sharing it with
        // a popup steals that widget and leaves blank menu rows/toolbar controls.
        auto* Item = Menu->addAction(Source->icon(), Source->text().isEmpty() ? Source->toolTip() : Source->text());
        const auto Sync = [Item, Source] {
            const QSignalBlocker Block(Item); Item->setIcon(Source->icon());
            Item->setIconVisibleInMenu(true);
            Item->setText(Source->text().isEmpty() ? Source->toolTip() : Source->text());
            Item->setEnabled(Source->isEnabled()); Item->setVisible(Source->isVisible());
            Item->setCheckable(Source->isCheckable()); Item->setChecked(Source->isChecked());
        };
        Sync(); QObject::connect(Source, &QAction::changed, Item, Sync);
        QObject::connect(Item, &QAction::triggered, Source, &QAction::trigger);
    }
}

namespace hc::document
{
    namespace
    {
        const char* PartKey(WorkspaceChromeController::Part Which)
        {
            switch (Which) {
            case WorkspaceChromeController::Part::Menu: return "mainMenuMode";
            case WorkspaceChromeController::Part::Toolbar: return "mainToolbarMode";
            case WorkspaceChromeController::Part::Status: return "statusBarMode";
            }
            return "mainMenuMode";
        }
    }
    WorkspaceChromeController::WorkspaceChromeController(QMenuBar* Menu, QToolBar* Toolbar, QObject* Parent)
        : QObject(Parent), menu_(Menu), toolbar_(Toolbar) {}
    bool WorkspaceChromeController::ApplySettings(const QJsonObject& Settings)
    {
        if (Settings["schemaVersion"].toInt() != 1) return false;
        for (const auto* Key : {"mainMenuMode", "mainToolbarMode", "statusBarMode"})
            if (Settings.contains(Key) && !QStringList{"recommended", "show", "hide"}.contains(Settings[Key].toString())) return false;
        if (Settings.contains("toolbarLayout") && !QStringList{"single", "split"}.contains(Settings["toolbarLayout"].toString())) return false;
        if (Settings.contains("applyToView") && !Settings["applyToView"].isBool()) return false;
        // statusHidden: {view kind: [status field id, …]} from the status bar menu.
        if (Settings.contains("statusHidden")) {
            if (!Settings["statusHidden"].isObject()) return false;
            const auto Hidden = Settings["statusHidden"].toObject();
            for (auto At = Hidden.begin(); At != Hidden.end(); ++At) {
                if (!At.value().isArray()) return false;
                for (const auto& Id : At.value().toArray()) if (!Id.isString()) return false;
            }
        }
        if (Settings.contains("viewOverrides") && !Settings["viewOverrides"].isObject()) return false;
        const auto Overrides = Settings["viewOverrides"].toObject();
        for (auto At = Overrides.begin(); At != Overrides.end(); ++At) {
            if (!At.value().isObject()) return false;
            for (const auto* Key : {"mainMenuMode", "mainToolbarMode", "statusBarMode"}) if (At.value().toObject().contains(Key)
                && !QStringList{"inherit", "show", "hide"}.contains(At.value().toObject()[Key].toString())) return false;
        }
        for (auto At = Settings.begin(); At != Settings.end(); ++At) settings_[At.key()] = At.value();
        apply(); return true;
    }
    void WorkspaceChromeController::SetGlobal(bool Menu, const QString& Mode)
    {
        if (!QStringList{"recommended", "show", "hide"}.contains(Mode)) return;
        settings_[Menu ? "mainMenuMode" : "mainToolbarMode"] = Mode; apply();
    }
    void WorkspaceChromeController::SetViewOverride(const QString& ViewId, bool Menu, const QString& Mode)
    {
        if (ViewId.isEmpty() || !QStringList{"inherit", "show", "hide"}.contains(Mode)) return;
        auto Views = settings_["viewOverrides"].toObject(); auto View = Views[ViewId].toObject();
        View[Menu ? "mainMenuMode" : "mainToolbarMode"] = Mode; Views[ViewId] = View; settings_["viewOverrides"] = Views; apply();
    }
    void WorkspaceChromeController::SetPart(Part Which, bool Visible, bool ActiveViewOnly)
    {
        const auto* Key = PartKey(Which);
        if (ActiveViewOnly && !active_.viewId_.isEmpty()) {
            auto Views = settings_["viewOverrides"].toObject(); auto View = Views[active_.viewId_].toObject();
            View[Key] = Visible ? "show" : "hide"; Views[active_.viewId_] = View; settings_["viewOverrides"] = Views;
        } else {
            settings_[Key] = Visible ? "show" : "hide";
            // A global choice wins over older per-view choices for the same part.
            auto Views = settings_["viewOverrides"].toObject();
            for (auto At = Views.begin(); At != Views.end(); ++At) { auto View = At.value().toObject(); View.remove(Key); At.value() = View; }
            settings_["viewOverrides"] = Views;
        }
        apply();
    }
    void WorkspaceChromeController::ResetParts()
    {
        for (const auto* Key : {"mainMenuMode", "mainToolbarMode", "statusBarMode"}) settings_[Key] = "recommended";
        settings_["viewOverrides"] = QJsonObject{};
        apply();
    }
    void WorkspaceChromeController::SetActiveViewOnly(bool Enabled) { settings_["applyToView"] = Enabled; }
    void WorkspaceChromeController::clearContributed()
    {
        for (auto& Action : contributedEdit_) if (Action && editMenu_) editMenu_->removeAction(Action);
        for (auto& Action : contributedView_) if (Action && viewMenu_) viewMenu_->removeAction(Action);
        contributedEdit_.clear(); contributedView_.clear();
        if (attachedMenu_) menu_->removeAction(attachedMenu_->menuAction());
        attachedMenu_.clear();
        if (contributionMenu_) { menu_->removeAction(contributionMenu_->menuAction()); delete contributionMenu_; contributionMenu_ = nullptr; }
    }
    void WorkspaceChromeController::SetActive(ViewUiContribution Contribution)
    {
        bool SameActions = activeActions_.size() == Contribution.menuActions_.size();
        for (qsizetype Index = 0; SameActions && Index < activeActions_.size(); ++Index)
            SameActions = activeActions_[Index].data() == Contribution.menuActions_[Index];
        const bool SameContribution = active_.viewId_ == Contribution.viewId_ && active_.menuTitle_ == Contribution.menuTitle_
            && SameActions && active_.menu_ == Contribution.menu_ && active_.editActions_ == Contribution.editActions_
            && active_.viewActions_ == Contribution.viewActions_;
        if (SameContribution) { active_ = std::move(Contribution); apply(); return; }
        clearContributed();
        active_ = std::move(Contribution);
        // Deleted toolbar actions must invalidate identity even if a new view
        // reuses the same allocation addresses during reload.
        activeActions_.clear(); for (auto* Action : active_.menuActions_) activeActions_.append(Action);
        const auto Insert = [this](QMenu* Menu) {
            if (modeAnchor_) menu_->insertMenu(modeAnchor_, Menu); else menu_->addMenu(Menu);
        };
        if (active_.menu_) {
            active_.menu_->setTitle(active_.menuTitle_);
            attachedMenu_ = active_.menu_; Insert(active_.menu_);
        } else if (!active_.menuActions_.isEmpty()) {
            contributionMenu_ = new QMenu(active_.menuTitle_, menu_);
            QString Label;
            for (auto* Action : active_.menuActions_) {
                if (Action->isSeparator()) { contributionMenu_->addSeparator(); Label.clear(); continue; }
                auto* Control = qobject_cast<QWidgetAction*>(Action);
                if (auto* Text = Control ? qobject_cast<QLabel*>(Control->defaultWidget()) : nullptr) { Label = Text->text().trimmed().remove(':'); continue; }
                auto* Widget = Control ? Control->defaultWidget() : nullptr;
                if (Widget && !Widget->accessibleName().isEmpty()) Label = Widget->accessibleName();
                // An unnamed value control has no meaningful menu row (it used to become a blank submenu or "탭 간격").
                if (Widget && Label.isEmpty()) continue;
                AddMenuControl(contributionMenu_, Action, Label);
                Label.clear();
            }
            Insert(contributionMenu_);
        }
        if (editMenu_) for (auto* Action : active_.editActions_) { editMenu_->insertAction(editAnchor_, Action); contributedEdit_.append(Action); }
        if (viewMenu_) for (auto* Action : active_.viewActions_) { viewMenu_->insertAction(viewAnchor_, Action); contributedView_.append(Action); }
        apply();
    }
    bool WorkspaceChromeController::visible(Part Which) const
    {
        const auto* Key = PartKey(Which);
        const auto Override = settings_["viewOverrides"].toObject()[active_.viewId_].toObject()[Key].toString(hc::defaults::edit::WorkspaceValue("viewOverrideMode").toString());
        if (Override != "inherit") return Override == "show";
        const auto Global = settings_[Key].toString(hc::defaults::edit::WorkspaceValue(Key).toString());
        if (Global != "recommended") return Global == "show";
        return Which == Part::Menu ? active_.mainMenuRecommended_ : Which == Part::Toolbar ? active_.mainToolbarRecommended_ : true;
    }
    bool WorkspaceChromeController::MenuVisible() const { return visible(Part::Menu); }
    bool WorkspaceChromeController::ToolbarVisible() const { return visible(Part::Toolbar); }
    bool WorkspaceChromeController::StatusVisible() const { return visible(Part::Status); }
    void WorkspaceChromeController::SetSuppressed(bool Suppressed)
    {
        suppressed_ = Suppressed;
        apply();
    }
    void WorkspaceChromeController::apply()
    {
        // Fullscreen owns the visible controls, even when appearance settings
        // or the active document change while its overlay is visible.
        menu_->setVisible(!suppressed_ && MenuVisible());
        const bool Toolbar = !suppressed_ && ToolbarVisible();
        if (toolbarRow_) toolbarRow_(Toolbar); else toolbar_->setVisible(Toolbar);
        // Image fullscreen manages the status row itself while it suppresses the chrome.
        if (status_ && !suppressed_) status_->setVisible(StatusVisible());
    }
}
