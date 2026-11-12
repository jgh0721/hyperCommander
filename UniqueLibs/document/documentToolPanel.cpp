#include "stdafx.h"
#include "documentToolPanel.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace hc::document
{
    DocumentToolHost::DocumentToolHost(const QString& EmptyText, bool Picker, QWidget* Parent)
        : QWidget(Parent)
    {
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(0);
        if (Picker) {
            auto* Row = new QWidget(this); Row->setObjectName("documentToolPickerRow");
            auto* RowLayout = new QHBoxLayout(Row); RowLayout->setContentsMargins(8, 4, 8, 4); RowLayout->setSpacing(6);
            auto* Label = new QLabel(tr("출처"), Row); RowLayout->addWidget(Label);
            picker_ = new QComboBox(Row); picker_->setObjectName("documentToolSource"); picker_->setAccessibleName(tr("출력 출처"));
            picker_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
            RowLayout->addWidget(picker_); RowLayout->addStretch(1);
            Layout->addWidget(Row);
            connect(picker_, qOverload<int>(&QComboBox::activated), this, [this](int Index) { showEntry(picker_->itemData(Index).toInt()); });
        }
        stack_ = new QStackedWidget(this); Layout->addWidget(stack_, 1);
        empty_ = new QLabel(EmptyText, stack_); empty_->setObjectName("documentToolEmpty");
        empty_->setAlignment(Qt::AlignCenter); empty_->setWordWrap(true); empty_->setMargin(12); empty_->setEnabled(false);
        stack_->addWidget(empty_);
    }
    DocumentToolHost::~DocumentToolHost()
    {
        // 패널은 문서 화면 것이다(화면이 지운다). 화면은 지워지는 동안에도 패널을 만질 수 있으므로(EPUBCheck 출력 등)
        // 여기서 함께 지우지 않고 돌려준다 — 끊고, 숨기고, 부모를 뗀다.
        for (const auto& Each : entries_) {
            if (!Each.panel_) continue;
            disconnect(Each.panel_, nullptr, this, nullptr);
            stack_->removeWidget(Each.panel_); Each.panel_->hide(); Each.panel_->setParent(nullptr);
        }
    }
    void DocumentToolHost::Add(const QString& Owner, const QString& OwnerName, QWidget* Panel, const QString& Label)
    {
        if (!Panel) return;
        for (const auto& Each : entries_) if (Each.panel_ == Panel) return;
        entries_.append(Entry{Owner, OwnerName, Panel, Label});
        stack_->addWidget(Panel); Panel->show();
        connect(Panel, &QObject::destroyed, this, &DocumentToolHost::removePanel);
        refreshPicker();
        if (Owner == active_ || CurrentPanel() == nullptr) SetActiveOwner(active_);
    }
    void DocumentToolHost::SetActiveOwner(const QString& Owner)
    {
        active_ = Owner;
        for (int At = 0; At < entries_.size(); ++At)
            if (entries_[At].owner_ == Owner && entries_[At].panel_) { showEntry(At); return; }
        // 고르개가 있으면 다른 문서에서 고른 출처를 그대로 둔다.
        if (picker_ && CurrentPanel()) return;
        if (picker_) for (int At = 0; At < entries_.size(); ++At) if (entries_[At].panel_) { showEntry(At); return; }
        if (stack_->currentWidget() != empty_) { stack_->setCurrentWidget(empty_); emit CurrentChanged(); }
    }
    bool DocumentToolHost::HasOwner(const QString& Owner) const
    {
        for (const auto& Each : entries_) if (Each.owner_ == Owner && Each.panel_) return true;
        return false;
    }
    QWidget* DocumentToolHost::CurrentPanel() const
    {
        auto* Current = stack_->currentWidget();
        return Current == empty_ ? nullptr : Current;
    }
    void DocumentToolHost::RenameOwner(const QString& Owner, const QString& OwnerName)
    {
        bool Changed = false;
        for (auto& Each : entries_) if (Each.owner_ == Owner && Each.ownerName_ != OwnerName) { Each.ownerName_ = OwnerName; Changed = true; }
        if (Changed) refreshPicker();
    }
    void DocumentToolHost::ShowPanel(QWidget* Panel)
    {
        for (int At = 0; At < entries_.size(); ++At) if (entries_[At].panel_ == Panel) { showEntry(At); return; }
    }
    void DocumentToolHost::removePanel(QObject* Panel)
    {
        // 문서 화면이 패널을 지웠다(문서를 닫음). 스택에서는 Qt가 이미 뺐다.
        const bool WasCurrent = stack_->currentWidget() == Panel || stack_->currentWidget() == nullptr;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [Panel](const Entry& Each) { return !Each.panel_ || Each.panel_ == Panel; }), entries_.end());
        refreshPicker();
        if (WasCurrent || CurrentPanel() == nullptr) SetActiveOwner(active_);
    }
    void DocumentToolHost::refreshPicker()
    {
        if (!picker_) return;
        const QSignalBlocker Block(picker_);
        picker_->clear();
        for (int At = 0; At < entries_.size(); ++At) {
            if (!entries_[At].panel_) continue;
            const auto& Each = entries_[At];
            picker_->addItem(Each.label_.isEmpty() ? Each.ownerName_ : QStringLiteral("%1 — %2").arg(Each.ownerName_, Each.label_), At);
            if (stack_->currentWidget() == Each.panel_) picker_->setCurrentIndex(picker_->count() - 1);
        }
        picker_->setEnabled(picker_->count() > 0);
    }
    void DocumentToolHost::showEntry(int Index)
    {
        if (Index < 0 || Index >= entries_.size() || !entries_[Index].panel_) return;
        const bool Changed = stack_->currentWidget() != entries_[Index].panel_;
        stack_->setCurrentWidget(entries_[Index].panel_);
        if (picker_) {
            const QSignalBlocker Block(picker_);
            const int Item = picker_->findData(Index); if (Item >= 0) picker_->setCurrentIndex(Item);
        }
        if (Changed) emit CurrentChanged();
    }
}
