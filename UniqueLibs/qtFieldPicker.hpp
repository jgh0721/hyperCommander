#pragma once

/// 고유 라이브러리
#include "commonLibs/columns/columnContracts.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QComboBox>
#include <QHash>
#include <QMenu>
#include <QSet>
#include <QStandardItemModel>
#include <QTimer>

// One field picker for the column editor, file color conditions and item selection conditions.
// Header-only: the condition row is also compiled into tests that link no product sources.
namespace hc::columns
{
    // Item roles. Qt::UserRole keeps the caller's key (a field id, or a built-in condition type),
    // so QComboBox::findData and the current index work as on a plain combo box.
    constexpr int FieldGroupRole = Qt::UserRole + 20, FieldProviderRole = Qt::UserRole + 21;
    constexpr int FieldBadgeRole = Qt::UserRole + 22, FieldReasonRole = Qt::UserRole + 23;

    // provider_ < 0: not a field (template, original value, unavailable reference): listed loose.
    struct FieldMenuEntry { QString title_, id_, group_; int provider_ = -1; QString badge_; bool enabled_ = true; QString reason_; };

    inline QString FieldProviderTitle(int Provider)
    {
        switch (ProviderKind(Provider)) {
        case ProviderKind::Wdx: return QObject::tr("WDX 플러그인");
        case ProviderKind::DOpus: return QObject::tr("DOpus 플러그인");
        case ProviderKind::Lua: return QObject::tr("계산 필드 (Lua)");
        default: return {};
        }
    }
    // Built-in topics are submenus at the top level; plugins and scripts sit one level below
    // their provider. A short filtered list stays flat with its topic on the left instead.
    // Action data is the entry index. Returns the action of Current, if any.
    inline QAction* FillFieldMenu(QMenu& Menu, const QVector<FieldMenuEntry>& Entries, int Current)
    {
        constexpr int FlatLimit = 12;
        QAction* Chosen = nullptr; QMenu* ChosenMenu = nullptr;
        const auto Add = [&](QMenu& Into, int Index, const QString& Text) {
            const auto& Entry = Entries[Index];
            // Text after a tab is drawn in the shortcut column: a right-aligned badge.
            auto* Action = Into.addAction(Entry.badge_.isEmpty() ? Text : Text + '\t' + Entry.badge_); Action->setData(Index);
            Action->setEnabled(Entry.enabled_);
            if (!Entry.reason_.isEmpty()) Action->setToolTip(Entry.reason_);
            else if (!Entry.id_.isEmpty()) Action->setToolTip(Entry.id_);
            // Only the current entry is checkable: some styles draw an empty box for every other one.
            if (Index == Current) { Action->setCheckable(true); Action->setChecked(true); Chosen = Action; ChosenMenu = &Into; }
        };
        const auto Short = [](const FieldMenuEntry& Entry) {
            const auto Prefix = Entry.group_ + " / ";
            return Entry.title_.startsWith(Prefix) ? Entry.title_.mid(Prefix.size()) : Entry.title_;
        };
        QHash<int, QSet<QString>> ProviderGroups; int Grouped = 0;
        for (const auto& Entry : Entries) if (Entry.provider_ >= 0 && !Entry.group_.isEmpty()) { ++Grouped; ProviderGroups[Entry.provider_].insert(Entry.group_); }
        int First = 0;
        for (; First < Entries.size() && (Entries[First].provider_ < 0 || Entries[First].group_.isEmpty()); ++First) Add(Menu, First, Entries[First].title_);
        if (First && Grouped) Menu.addSeparator();
        QVector<int> Trailing; QHash<int, QMenu*> Providers; QHash<QString, QMenu*> Groups; bool Separated = false;
        for (int Index = First; Index < Entries.size(); ++Index) {
            const auto& Entry = Entries[Index];
            if (Entry.provider_ < 0 || Entry.group_.isEmpty()) { Trailing.append(Index); continue; }
            if (Grouped <= FlatLimit) { Add(Menu, Index, Entry.group_ + QStringLiteral("  ›  ") + Short(Entry)); continue; }
            const bool Plugin = Entry.provider_ != int(ProviderKind::BuiltIn);
            if (Plugin && !Separated) { Menu.addSeparator(); Separated = true; }
            auto*& Group = Groups[QString::number(Entry.provider_) + QChar(0) + Entry.group_];
            if (!Group) {
                if (!Plugin) Group = Menu.addMenu(Entry.group_);
                else if (ProviderGroups[Entry.provider_].size() == 1) Group = Menu.addMenu(FieldProviderTitle(Entry.provider_) + QStringLiteral(" · ") + Entry.group_);
                else {
                    auto*& Provider = Providers[Entry.provider_];
                    if (!Provider) Provider = Menu.addMenu(FieldProviderTitle(Entry.provider_));
                    Group = Provider->addMenu(Entry.group_);
                }
            }
            Add(*Group, Index, Short(Entry));
        }
        if (!Trailing.isEmpty() && !Menu.isEmpty()) Menu.addSeparator();
        for (const int Index : Trailing) Add(Menu, Index, Entries[Index].title_);
        // Bold the submenus that lead to the current field.
        for (auto* Level = ChosenMenu; Level && Level != &Menu; Level = qobject_cast<QMenu*>(Level->parentWidget())) {
            auto Font = Level->menuAction()->font(); Font.setBold(true); Level->menuAction()->setFont(Font);
        }
        return Chosen;
    }
    inline QString FieldBadge(const FieldDefinition& Field)
    {
        if (Field.provider_ == ProviderKind::Wdx || Field.provider_ == ProviderKind::DOpus) return QObject::tr("플러그인");
        return Field.cost_ == EvaluationCost::Snapshot ? QString{} : QObject::tr("느림");
    }

    // A categorized popup over a flat combo model: findData, the current index and arrow-key
    // stepping keep working on the combo itself.
    class FieldPicker : public QComboBox
    {
    public:
        using QComboBox::QComboBox;
        QSize minimumSizeHint() const override
        {
            auto Size = QComboBox::minimumSizeHint(); Size.setWidth(75); return Size;
        }
        void AddEntry(const FieldMenuEntry& Entry)
        {
            addItem(Entry.title_, Entry.id_); const int Row = count() - 1;
            if (!Entry.group_.isEmpty()) setItemData(Row, Entry.group_, FieldGroupRole);
            if (Entry.provider_ >= 0) setItemData(Row, Entry.provider_, FieldProviderRole);
            if (!Entry.badge_.isEmpty()) setItemData(Row, Entry.badge_, FieldBadgeRole);
            if (!Entry.reason_.isEmpty()) { setItemData(Row, Entry.reason_, FieldReasonRole); setItemData(Row, Entry.reason_, Qt::ToolTipRole); }
            if (!Entry.enabled_) if (auto* Items = qobject_cast<QStandardItemModel*>(model())) Items->item(Row)->setEnabled(false);
        }
        void AddField(const FieldDefinition& Field) { AddEntry({Field.title_, Field.id_, Field.group_, int(Field.provider_), FieldBadge(Field)}); }
        QVector<FieldMenuEntry> Entries() const
        {
            QVector<FieldMenuEntry> Result; const auto* Items = qobject_cast<const QStandardItemModel*>(model());
            for (int Row = 0; Row < count(); ++Row) {
                const auto Provider = itemData(Row, FieldProviderRole);
                Result.append({itemText(Row), itemData(Row).toString(), itemData(Row, FieldGroupRole).toString(), Provider.isValid() ? Provider.toInt() : -1,
                    itemData(Row, FieldBadgeRole).toString(), !Items || Items->item(Row)->isEnabled(), itemData(Row, FieldReasonRole).toString()});
            }
            return Result;
        }
        void showPopup() override
        {
            if (!count()) return;
            QMenu Menu(this); Menu.setObjectName(objectName() + ".menu"); Menu.setMinimumWidth(width()); Menu.setToolTipsVisible(true);
            auto* Current = FillFieldMenu(Menu, Entries(), currentIndex());
            // Open on the current field: its topic submenu unfolds with the field checked.
            QAction* Top = Current;
            for (auto* Level = Current ? qobject_cast<QMenu*>(Current->parent()) : nullptr; Level && Level != &Menu; Level = qobject_cast<QMenu*>(Level->parentWidget())) Top = Level->menuAction();
            if (Top) QTimer::singleShot(0, &Menu, [&Menu, Top] { Menu.setActiveAction(Top); });
            if (const auto* Action = Menu.exec(mapToGlobal(QPoint(0, height())))) {
                const int Row = Action->data().toInt(); setCurrentIndex(Row); emit activated(Row);
            }
        }
    };
}
