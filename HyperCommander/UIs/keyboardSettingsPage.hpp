#pragma once
#include "commandSelector.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/SettingsWidgets.h>
#include <fmwidgets/KeySequenceDelegate.h>

namespace hc::ui {
class KeyboardSettingsPage final : public QWidget {
    enum Column { ScopeColumn, CommandColumn, DescriptionColumn, CurrentKeyColumn, DefaultKeyColumn, ColumnCount };
public:
    explicit KeyboardSettingsPage(QWidget* Parent = nullptr) : QWidget(Parent) {
        setObjectName("hc.settings.keyboard"); auto* Root = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("키보드 · 기능 키 막대")); Header->setSubtitle(tr("명령과 키를 찾고 바꿉니다. 사용 중인 키는 충돌 명령을 확인한 후 재지정합니다.")); Root->addWidget(Header);
        auto* Tabs = new QTabWidget(this); Root->addWidget(Tabs, 1);
        auto* Page = new QWidget; auto* Rows = new QVBoxLayout(Page); Tabs->addTab(Page, tr("단축키"));
        auto* Filter = new QHBoxLayout; Rows->addLayout(Filter);
        search_ = new fm::ui::SearchField(Page); search_->setObjectName("hc.keys.search"); search_->setPlaceholderText(tr("명령 또는 키 찾기")); Filter->addWidget(search_, 1);
        scope_ = new QComboBox(Page); scope_->setObjectName("hc.keys.scope"); scope_->addItems({tr("전체"), tr("파일 작업"), tr("패널·탭"), tr("탐색"), tr("창·설정")}); Filter->addWidget(scope_);
        auto* PresetLabel = new QLabel(tr("기본 키:"), Page); Filter->addWidget(PresetLabel);
        preset_ = new QComboBox(Page); preset_->setObjectName("hc.keys.preset"); preset_->addItems({tr("기존 기본값"), tr("파일 작업 기본값")}); Filter->addWidget(preset_); PresetLabel->setBuddy(preset_);
        preset_->setAccessibleName(tr("기본 단축키 구성"));
        preset_->setToolTip(tr("기본값 열에 표시할 단축키 구성을 선택합니다. 내장 단축키 사용이 켜져 있으면 현재 키에도 반영하며, 사용자 지정 키가 우선합니다.\n초기화 버튼을 누르면 사용자 지정을 지우고 선택한 기본값으로 되돌립니다.\n변경은 적용/확인으로 저장하고 취소하면 복원합니다."));
        preset_->setItemData(0, tr("기존 단축키 구성 · F5 새로 고침"), Qt::ToolTipRole);
        preset_->setItemData(1, tr("파일 작업 구성 · F5 새로 고침 · F6 이동 · F7 새 폴더 · F8 삭제 · Ctrl+R 이름 변경"), Qt::ToolTipRole);
        table_ = new QTableWidget(Page); table_->setObjectName("hc.keys.commands"); table_->setColumnCount(ColumnCount); table_->setHorizontalHeaderLabels({tr("범위"), tr("명령"), tr("설명"), tr("현재 키"), tr("기본값")});
        table_->setItemDelegateForColumn(CurrentKeyColumn, new fm::ui::KeySequenceDelegate(false, table_)); table_->setItemDelegateForColumn(DefaultKeyColumn, new fm::ui::KeySequenceDelegate(true, table_));
        table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide();
        // Every column can be resized. The defaults keep the command, its description and the current key
        // readable; the default-key column takes the remaining width. Multi-key rows no longer widen the table.
        auto* Columns = table_->horizontalHeader(); Columns->setSectionResizeMode(QHeaderView::Interactive); Columns->setStretchLastSection(true); Columns->setMinimumSectionSize(48);
        table_->setColumnWidth(ScopeColumn, 90); table_->setColumnWidth(CommandColumn, 200); table_->setColumnWidth(DescriptionColumn, 260); table_->setColumnWidth(CurrentKeyColumn, 170);
        table_->setWordWrap(false); table_->setTextElideMode(Qt::ElideRight); Rows->addWidget(table_, 1);
        auto* Edit = new fm::ui::Card(Page); auto* EditRows = new QVBoxLayout(Edit); Rows->addWidget(Edit);
        selector_ = new CommandSelector(Edit); selector_->setObjectName("hc.keys.command"); EditRows->addWidget(selector_);
        auto* Capture = new QHBoxLayout; EditRows->addLayout(Capture);
        key_ = new fm::ui::KeyCaptureEdit(Edit); key_->setObjectName("hc.keys.capture"); Capture->addWidget(key_, 1);
        auto* Listen = new QPushButton(tr("키 누르기(&K)"), Edit); Listen->setObjectName("hc.keys.listen"); Capture->addWidget(Listen);
        assign_ = new QPushButton(tr("지정(&A)"), Edit); assign_->setObjectName("hc.keys.assign"); Capture->addWidget(assign_);
        auto* Clear = new QPushButton(tr("이 명령의 키 해제"), Edit); Clear->setObjectName("hc.keys.clear"); Capture->addWidget(Clear);
        conflict_ = new QLabel(Edit); conflict_->setObjectName("hc.keys.conflict"); conflict_->setTextFormat(Qt::PlainText); conflict_->setWordWrap(true); EditRows->addWidget(conflict_);
        reassign_ = new QPushButton(tr("충돌 명령에서 가져와 재지정(&O)"), Edit); reassign_->setObjectName("hc.keys.reassign"); EditRows->addWidget(reassign_); reassign_->hide();
        auto* Reset = new QPushButton(tr("선택한 기본값으로 초기화(&R)"), Page); Rows->addWidget(Reset);
        auto* Functions = new QWidget; auto* FunctionForm = new QFormLayout(Functions); Tabs->addTab(Functions, tr("기능 키 막대"));
        FunctionForm->addRow(new QLabel(tr("표시할 명령을 선택합니다. 각 명령의 키는 단축키 탭에서 변경합니다."), Functions));
        for (int I = 0; I < 9; ++I) {
            auto* C = new CommandSelector(Functions); C->setObjectName("hc.keys.function" + QString::number(I)); functions_.append(C);
            auto* Caption = new QLineEdit(Functions); Caption->setObjectName("hc.keys.functionCaption" + QString::number(I));
            Caption->setPlaceholderText(tr("표시 문구 · 비우면 기본값")); Caption->setMaxLength(128);
            Caption->setAccessibleName(tr("기능 %1 표시 문구").arg(I + 1)); captions_.append(Caption);
            auto* Row = new QHBoxLayout; Row->addWidget(C, 2); Row->addWidget(Caption, 1);
            FunctionForm->addRow(tr("기능 %1").arg(I + 1), Row);
        }
        auto* DefaultFunctions = new QPushButton(tr("기능 막대를 기본값으로"), Functions); FunctionForm->addRow(DefaultFunctions);
        connect(DefaultFunctions, &QPushButton::clicked, this, [this] { const auto Ids = FunctionCommands({}); for (int I = 0; I < functions_.size(); ++I) { functions_[I]->SetCommand(Ids[I]); captions_[I]->clear(); } });
        auto* Note = new QLabel(tr("변경 내용은 적용/확인으로 저장하고 취소하면 복원합니다. 키 표시와 기능바·메뉴는 같은 명령 ID를 사용합니다. 대화상자 자체의 고정 키는 여기서 변경하지 않습니다."), this); Note->setWordWrap(true); Root->addWidget(Note);
        connect(search_, &QLineEdit::textChanged, this, [this] { filter(); }); connect(scope_, &QComboBox::currentIndexChanged, this, [this] { filter(); });
        connect(table_, &QTableWidget::currentCellChanged, this, [this](int Row) { if (Row < 0) return; selector_->SetCommand(table_->item(Row, CommandColumn)->data(Qt::UserRole).toString()); key_->setPending({}); updateConflict(); });
        connect(selector_, &QComboBox::currentTextChanged, this, [this] { updateConflict(); });
        connect(Listen, &QPushButton::clicked, key_, &fm::ui::KeyCaptureEdit::startCapture);
        connect(key_, &fm::ui::KeyCaptureEdit::captured, this, [this] { updateConflict(); });
        connect(key_, &fm::ui::KeyCaptureEdit::cleared, this, [this] { updateConflict(); });
        connect(assign_, &QPushButton::clicked, this, [this] { assign(false); }); connect(reassign_, &QPushButton::clicked, this, [this] { assign(true); });
        connect(Clear, &QPushButton::clicked, this, [this] { auto* M = TyStCommandMgr::GetInstance(); const auto Id = selector_->Command(); for (const auto& K : CommandKeys(M->Retrieve(), Id)) M->DelShortcut(K[0]); Refresh(); });
        connect(preset_, &QComboBox::currentIndexChanged, this, [this](int I) { if (loading_) return; auto G = settings::AppSettings().Read(settings::Section::General).toObject(); G["shortcutPreset"] = I ? "prototype" : "legacy"; settings::AppSettings().Put(settings::Section::General, G); Refresh(); });
        connect(Reset, &QPushButton::clicked, this, [this] { if (hc::ui::MessageDialog::question(this, tr("단축키 초기화"), tr("사용자 키를 지우고 선택한 기본값으로 되돌리시겠습니까?")) != QMessageBox::Yes) return; settings::AppSettings().Put(settings::Section::Shortcuts, QJsonObject{}); Refresh(); });
        Load();
    }
    void Load() { loading_ = true; const auto G = settings::AppSettings().Read(settings::Section::General).toObject(); preset_->setCurrentIndex(G["shortcutPreset"] == "prototype" ? 1 : 0); const auto Ids = FunctionCommands(G); for (int I = 0; I < functions_.size(); ++I) { functions_[I]->SetCommand(Ids[I]); captions_[I]->setText((I < G["functionCaptions"].toArray().size() ? G["functionCaptions"].toArray()[I].toString() : QString{})); } loading_ = false; Refresh(); }
    bool SaveFunctions(QJsonObject& G) { QJsonArray Items; for (auto* C : functions_) { if (FindCommand(C->Command()).Name.isEmpty()) { conflict_->setText(tr("기능 막대에 등록되지 않은 명령이 있습니다.")); return false; } Items.append(C->Command()); } G["functionCommands"] = Items; QJsonArray Captions; for (auto* Caption : captions_) Captions.append(Caption->text()); G["functionCaptions"] = Captions; return true; }
    void Refresh() {
        auto* M = TyStCommandMgr::GetInstance(); M->Refresh();
        auto DefaultGeneral = settings::AppSettings().Read(settings::Section::General).toObject(); DefaultGeneral["useBuiltInShortcuts"] = true;
        const auto Values = M->Retrieve(), Defaults = DefaultShortcuts(DefaultGeneral);
        const auto Id = selector_->Command(); table_->setRowCount(0);
        for (int I = 0; I < selector_->count(); ++I) { const auto Command = selector_->itemData(I).toString(); if (FindCommand(Command).Name.isEmpty()) continue;
            const auto Label = selector_->itemText(I).section(" · ", 0, 0); const int Row = table_->rowCount(); table_->insertRow(Row);
            auto* Item = new QTableWidgetItem(Command); Item->setData(Qt::UserRole, Command); Item->setToolTip(Command); table_->setItem(Row, CommandColumn, Item);
            auto* Description = new QTableWidgetItem(Label); Description->setToolTip(Label); table_->setItem(Row, DescriptionColumn, Description);
            const auto Join = [&](const auto& Keys) { QStringList Labels; for (const auto& K : CommandKeys(Keys, Command)) Labels.append(K.toString()); return Labels.join(", "); };
            table_->setItem(Row, CurrentKeyColumn, new QTableWidgetItem(Join(Values))); table_->setItem(Row, DefaultKeyColumn, new QTableWidgetItem(Join(Defaults))); const int Scope = category(Command); table_->setItem(Row, ScopeColumn, new QTableWidgetItem(scope_->itemText(Scope))); table_->item(Row, ScopeColumn)->setData(Qt::UserRole, Scope);
        }
        selector_->SetCommand(Id); filter(); updateConflict();
    }
private:
    static int category(const QString& Id) { if (Id.startsWith("hc.file.") || Id.startsWith("hc.archive.") || Id.startsWith("hc.viewer.") || Id.startsWith("hc.jobs.")) return 1; if (Id.startsWith("hc.panel.") || Id.startsWith("hc.tab.") || Id.startsWith("hc.view.") || Id.startsWith("hc.selection.")) return 2; if (Id.startsWith("hc.navigate.") || Id.startsWith("hc.search.")) return 3; return 4; }
    void filter() { const auto Text = search_->text().trimmed(); for (int R = 0; R < table_->rowCount(); ++R) { QString Match; for (int C = 0; C < ColumnCount; ++C) Match += " " + table_->item(R, C)->text(); table_->setRowHidden(R, (scope_->currentIndex() && table_->item(R, ScopeColumn)->data(Qt::UserRole) != scope_->currentIndex()) || !Match.contains(Text, Qt::CaseInsensitive)); } }
    void updateConflict() { const auto Key = key_->pending(); const auto Existing = Key.isEmpty() ? QString{} : CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(Key[0])); const auto Id = selector_->Command(); const bool Valid = !Key.isEmpty() && Key.count() == 1 && !FindCommand(Id).Name.isEmpty(); const bool Conflict = Valid && !Existing.isEmpty() && Existing != Id; assign_->setEnabled(Valid && !Conflict); reassign_->setVisible(Conflict); conflict_->setText(Conflict ? tr("%1은(는) '%2'에서 사용 중입니다. 재지정하면 기존 명령에서 이 키가 해제됩니다.").arg(Key.toString(), FindCommand(Existing).Desc) : Valid ? tr("적용 전 초안으로 키를 지정할 수 있습니다.") : tr("명령을 선택하고 키 누르기를 실행하세요. Esc로 입력을 취소합니다.")); }
    void assign(bool Replace) { const auto K = key_->pending(); if (K.isEmpty() || K.count() != 1) return; auto* M = TyStCommandMgr::GetInstance(); const auto Existing = CommandId(M->Retrieve().value(K[0])); if (!Replace && !Existing.isEmpty() && Existing != selector_->Command()) return; M->SetShortcutWithCMD(K[0], selector_->Command()); Refresh(); }
    QLineEdit* search_; QComboBox *scope_, *preset_; QTableWidget* table_; CommandSelector* selector_; fm::ui::KeyCaptureEdit* key_; QLabel* conflict_; QPushButton *assign_, *reassign_; QList<CommandSelector*> functions_; QList<QLineEdit*> captions_; bool loading_ = false;
};
}
