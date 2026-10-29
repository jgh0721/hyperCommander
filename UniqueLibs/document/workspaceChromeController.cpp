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
    WorkspaceChromeController::WorkspaceChromeController(QMenuBar* Menu, QToolBar* Toolbar, QObject* Parent)
        : QObject(Parent), menu_(Menu), toolbar_(Toolbar) {}
    bool WorkspaceChromeController::ApplySettings(const QJsonObject& Settings)
    {
        if (Settings["schemaVersion"].toInt() != 1) return false;
        for (const auto* Key : {"mainMenuMode", "mainToolbarMode"})
            if (Settings.contains(Key) && !QStringList{"recommended", "show", "hide"}.contains(Settings[Key].toString())) return false;
        if (Settings.contains("viewOverrides") && !Settings["viewOverrides"].isObject()) return false;
        const auto Overrides = Settings["viewOverrides"].toObject();
        for (auto At = Overrides.begin(); At != Overrides.end(); ++At) {
            if (!At.value().isObject()) return false;
            for (const auto* Key : {"mainMenuMode", "mainToolbarMode"}) if (At.value().toObject().contains(Key)
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
    void WorkspaceChromeController::SetActive(ViewUiContribution Contribution)
    {
        bool SameActions = activeActions_.size() == Contribution.menuActions_.size();
        for (qsizetype Index = 0; SameActions && Index < activeActions_.size(); ++Index)
            SameActions = activeActions_[Index].data() == Contribution.menuActions_[Index];
        if (active_.viewId_ == Contribution.viewId_ && active_.menuTitle_ == Contribution.menuTitle_
            && SameActions) { active_ = std::move(Contribution); apply(); return; }
        if (contributionMenu_) { menu_->removeAction(contributionMenu_->menuAction()); delete contributionMenu_; contributionMenu_ = nullptr; }
        active_ = std::move(Contribution);
        // Deleted toolbar actions must invalidate identity even if a new view
        // reuses the same allocation addresses during reload.
        activeActions_.clear(); for (auto* Action : active_.menuActions_) activeActions_.append(Action);
        if (!active_.menuActions_.isEmpty()) {
            contributionMenu_ = menu_->addMenu(active_.menuTitle_);
            QString Label;
            for (auto* Action : active_.menuActions_) {
                if (Action->isSeparator()) { contributionMenu_->addSeparator(); Label.clear(); continue; }
                auto* Control = qobject_cast<QWidgetAction*>(Action);
                if (auto* Text = Control ? qobject_cast<QLabel*>(Control->defaultWidget()) : nullptr) { Label = Text->text().trimmed().remove(':'); continue; }
                auto* Widget = Control ? Control->defaultWidget() : nullptr;
                if (Widget && !Widget->accessibleName().isEmpty()) Label = Widget->accessibleName();
                if (auto* Spin = qobject_cast<QSpinBox*>(Widget); Spin && Label.isEmpty()) Label = tr("탭 간격");
                AddMenuControl(contributionMenu_, Action, Label);
                Label.clear();
            }
        }
        apply();
    }
    bool WorkspaceChromeController::visible(bool Menu) const
    {
        const auto Key = Menu ? "mainMenuMode" : "mainToolbarMode";
        const auto Override = settings_["viewOverrides"].toObject()[active_.viewId_].toObject()[Key].toString("inherit");
        if (Override != "inherit") return Override == "show";
        const auto Global = settings_[Key].toString("recommended");
        return Global == "recommended" ? (Menu ? active_.mainMenuRecommended_ : active_.mainToolbarRecommended_) : Global == "show";
    }
    bool WorkspaceChromeController::MenuVisible() const { return visible(true); }
    bool WorkspaceChromeController::ToolbarVisible() const { return visible(false); }
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
        toolbar_->setVisible(!suppressed_ && ToolbarVisible());
    }
}
