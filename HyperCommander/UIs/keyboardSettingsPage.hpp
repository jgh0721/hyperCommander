#pragma once
#include "UniqueLibs/productDefaults.hpp"
#include "commandSelector.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/SettingsWidgets.h>
#include <fmwidgets/KeySequenceDelegate.h>

namespace hc::ui {
class CombinedKeyDelegate final : public QStyledItemDelegate {
public:
    static constexpr int DefaultRole = Qt::UserRole + 2;
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        QStyleOptionViewItem Background(Option); initStyleOption(&Background, Index); Background.text.clear();
        const auto* Style = Option.widget ? Option.widget->style() : QApplication::style();
        Style->drawControl(QStyle::CE_ItemViewItem, &Background, Painter, Option.widget);
        const auto& Colors = fm::style::ThemeManager::instance().colors();
        int X = Option.rect.left() + 5;
        Painter->save(); Painter->setClipRect(Option.rect);
        for (const bool Dim : {false, true}) {
            for (const auto& Key : Index.data(Dim ? DefaultRole : Qt::DisplayRole).toString().split(", ", Qt::SkipEmptyParts)) {
                const auto Size = fm::ui::keyCapSize(Key);
                fm::ui::paintKeyCap(Painter, QRect(X, Option.rect.center().y() - Size.height() / 2, Size.width(), Size.height()), Key, Colors, Dim);
                X += Size.width() + 4;
            }
            X += 8;
        }
        Painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        auto Size = QStyledItemDelegate::sizeHint(Option, Index); Size.setHeight(qMax(26, Size.height())); return Size;
    }
};
class GlobalShortcutDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QRect CheckRect(const QStyleOptionViewItem& Option, const QModelIndex& Index) const {
        const auto* Style = Option.widget ? Option.widget->style() : QApplication::style();
        const int Width = Style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, Option.widget);
        const int Height = Style->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, Option.widget);
        const int TextHeight = Index.data().toString().isEmpty() ? 0 : Option.fontMetrics.height() + 3;
        return QRect(Option.rect.center().x() - Width / 2, Option.rect.center().y() - (Height + TextHeight) / 2, Width, Height);
    }
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        QStyleOptionViewItem Background(Option); initStyleOption(&Background, Index);
        Background.text.clear(); Background.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        const auto* Style = Option.widget ? Option.widget->style() : QApplication::style();
        Style->drawControl(QStyle::CE_ItemViewItem, &Background, Painter, Option.widget);
        if (!Index.data(Qt::CheckStateRole).isValid()) return;
        QStyleOptionButton Check; Check.rect = CheckRect(Option, Index);
        Check.state = (Index.flags().testFlag(Qt::ItemIsEnabled) ? QStyle::State_Enabled : QStyle::State_None)
            | (Index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? QStyle::State_On : QStyle::State_Off);
        Style->drawPrimitive(QStyle::PE_IndicatorCheckBox, &Check, Painter, Option.widget);
        const QRect Text(Option.rect.left(), Check.rect.bottom() + 4, Option.rect.width(), Option.fontMetrics.height());
        Style->drawItemText(Painter, Text, Qt::AlignCenter, Option.palette, true, Index.data().toString(),
            Option.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);
    }
    QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        auto Size = QStyledItemDelegate::sizeHint(Option, Index);
        Size.setHeight(qMax(Size.height(), CheckRect(Option, Index).height() + 8
            + (Index.data().toString().isEmpty() ? 0 : Option.fontMetrics.height() + 3)));
        return Size;
    }
    bool editorEvent(QEvent* Event, QAbstractItemModel* Model, const QStyleOptionViewItem& Option, const QModelIndex& Index) override {
        if (!(Index.flags() & Qt::ItemIsEnabled) || !(Index.flags() & Qt::ItemIsUserCheckable)) return false;
        if (Event->type() == QEvent::MouseButtonRelease) {
            const auto* Mouse = static_cast<QMouseEvent*>(Event);
            if (Mouse->button() != Qt::LeftButton || !CheckRect(Option, Index).contains(Mouse->position().toPoint())) return false;
        } else if (Event->type() == QEvent::KeyPress) {
            const auto* Key = static_cast<QKeyEvent*>(Event);
            if (Key->key() != Qt::Key_Space && Key->key() != Qt::Key_Select) return false;
        } else return false;
        return Model->setData(Index, Index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    }
};
class KeyboardSettingsPage final : public QWidget {
    enum Column { ScopeColumn, CommandColumn, DescriptionColumn, CurrentKeyColumn, GlobalColumn, ColumnCount };
    // 범위 콤보의 순서. 앞의 일곱 개는 기존 순서를 유지한다(설정·시험이 색인을 쓴다).
    enum Category { AllCategory, FileCategory, PanelCategory, NavigateCategory, WindowCategory, CommandLineCategory, AudioCategory, ProcessCategory, ServiceCategory };
    static constexpr int InputScopeRole = Qt::UserRole + 1;
public:
    explicit KeyboardSettingsPage(QWidget* Parent = nullptr) : QWidget(Parent) {
        setObjectName("hc.settings.keyboard"); auto* Root = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("키보드 · 기능 키 막대")); Header->setSubtitle(tr("명령과 키를 찾고 바꿉니다. 사용 중인 키는 충돌 명령을 확인한 후 재지정합니다.")); Root->addWidget(Header);
        auto* Tabs = new QTabWidget(this); Root->addWidget(Tabs, 1);
        auto* Page = new QWidget; auto* Rows = new QVBoxLayout(Page); Tabs->addTab(Page, tr("단축키"));
        auto* Filter = new QHBoxLayout; Rows->addLayout(Filter);
        search_ = new fm::ui::SearchField(Page); search_->setObjectName("hc.keys.search"); search_->setPlaceholderText(tr("명령 또는 키 찾기")); Filter->addWidget(search_, 1);
        scope_ = new QComboBox(Page); scope_->setObjectName("hc.keys.scope"); scope_->addItems({tr("전체"), tr("파일 작업"), tr("패널·탭"), tr("탐색"), tr("창·설정"), tr("명령행 입력"), tr("오디오"), tr("프로세스 보기"), tr("서비스 보기")}); Filter->addWidget(scope_);
        scope_->setItemData(ProcessCategory, tr("프로세스 보기에 포커스가 있을 때만 쓰는 키. 같은 키의 전역 명령보다 먼저 처리합니다."), Qt::ToolTipRole);
        scope_->setItemData(ServiceCategory, tr("서비스 보기에 포커스가 있을 때만 쓰는 키. 같은 키의 전역 명령보다 먼저 처리합니다."), Qt::ToolTipRole);
        auto* PresetLabel = new QLabel(tr("기본 키:"), Page); Filter->addWidget(PresetLabel);
        preset_ = new QComboBox(Page); preset_->setObjectName("hc.keys.preset"); preset_->addItems({tr("기존 기본값"), tr("파일 작업 기본값"), tr("제품 기본값")}); Filter->addWidget(preset_); PresetLabel->setBuddy(preset_);
        preset_->setAccessibleName(tr("기본 단축키 구성"));
        preset_->setToolTip(tr("흐린 글씨로 표시할 기본 단축키 구성을 선택합니다. 내장 단축키 사용이 켜져 있으면 현재 키에도 반영하며, 사용자 지정 키가 우선합니다.\n초기화 버튼을 누르면 사용자 지정을 지우고 선택한 기본값으로 되돌립니다.\n변경은 적용/확인으로 저장하고 취소하면 복원합니다."));
        preset_->setItemData(0, tr("기존 단축키 구성 · F5 새로 고침"), Qt::ToolTipRole);
        preset_->setItemData(1, tr("파일 작업 구성 · F5 새로 고침 · F6 이동 · F7 새 폴더 · F8 삭제 · Ctrl+R 이름 변경"), Qt::ToolTipRole);
        preset_->setItemData(2, tr("제품 기본값 · Ctrl+C 복사 · F2/Ctrl+R 이름 변경 · Ctrl+M 이동 · Ctrl+Alt+R 다중 이름 변경 · Ctrl+K 새 폴더 · Ctrl+A 압축 · Ctrl+X 압축 해제"), Qt::ToolTipRole);
        table_ = new QTableWidget(Page); table_->setObjectName("hc.keys.commands"); table_->setColumnCount(ColumnCount); table_->setHorizontalHeaderLabels({tr("범위"), tr("명령"), tr("설명"), tr("현재 키 · 기본값"), tr("글로벌")});
        table_->setItemDelegateForColumn(CurrentKeyColumn, new CombinedKeyDelegate(table_));
        table_->setItemDelegateForColumn(GlobalColumn, new GlobalShortcutDelegate(table_));
        table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide();
        // Current and default keys share the expanding column.
        // The global checkbox keeps a compact width.
        auto* Columns = table_->horizontalHeader(); Columns->setSectionResizeMode(QHeaderView::Interactive); Columns->setStretchLastSection(false); Columns->setSectionResizeMode(CurrentKeyColumn, QHeaderView::Stretch); Columns->setMinimumSectionSize(48);
        table_->setColumnWidth(ScopeColumn, 90); table_->setColumnWidth(CommandColumn, 200); table_->setColumnWidth(DescriptionColumn, 260); table_->setColumnWidth(CurrentKeyColumn, 320); table_->setColumnWidth(GlobalColumn, 85);
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
        auto* ResetRow = new QHBoxLayout; Rows->addLayout(ResetRow);
        auto* Reset = new QPushButton(tr("선택한 기본값으로 초기화(&R)"), Page); ResetRow->addWidget(Reset);
        resetScope_ = new QPushButton(tr("이 범위만 기본값으로"), Page); resetScope_->setObjectName("hc.keys.resetScope"); ResetRow->addWidget(resetScope_); ResetRow->addStretch(1);
        resetScope_->setToolTip(tr("프로세스 보기 또는 서비스 보기 범위의 사용자 키만 지웁니다. 다른 범위의 키는 그대로 둡니다."));
        auto* Functions = new QWidget; auto* FunctionForm = new QFormLayout(Functions); Tabs->addTab(Functions, tr("기능 키 막대"));
        functionKind_ = new QComboBox(Functions); functionKind_->setObjectName("hc.keys.functionScope");
        functionKind_->addItems({tr("파일 패널"), tr("프로세스 보기"), tr("서비스 보기")});
        functionKind_->setToolTip(tr("활성 패널의 보기 종류에 따라 기능 키 막대가 바뀝니다."));
        FunctionForm->addRow(tr("보기"), functionKind_);
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
        connect(DefaultFunctions, &QPushButton::clicked, this, [this] {
            const auto Ids = FunctionCommandsFor({}, FunctionBarKind(functionKind_->currentIndex()));
            for (int I = 0; I < functions_.size(); ++I) { functions_[I]->SetCommand(Ids.value(I)); captions_[I]->clear(); }
        });
        connect(functionKind_, &QComboBox::currentIndexChanged, this, [this](int Index) { storeFunctions(shownKind_); shownKind_ = Index; showFunctions(Index); });
        auto* Note = new QLabel(tr("변경 내용은 적용/확인으로 저장하고 취소하면 복원합니다. 키 표시와 기능바·메뉴는 같은 명령 ID를 사용합니다. 대화상자 자체의 고정 키는 여기서 변경하지 않습니다."), this); Note->setWordWrap(true); Root->addWidget(Note);
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* Item) {
            if (Item->column() != GlobalColumn) return;
            auto* M = TyStCommandMgr::GetInstance();
            const auto Id = table_->item(Item->row(), CommandColumn)->data(Qt::UserRole).toString();
            M->SetGlobalShortcutEnabled(Id, Item->checkState() == Qt::Checked);
            const QSignalBlocker Block(table_);
            Item->setText(M->GlobalShortcutError(Id).isEmpty() ? QString{} : tr("실패"));
            Item->setToolTip(M->GlobalShortcutError(Id).isEmpty() ? tr("다른 앱 사용 중에도 현재 키로 실행합니다.") : M->GlobalShortcutError(Id));
            table_->resizeRowToContents(Item->row());
        });
        connect(search_, &QLineEdit::textChanged, this, [this] { filter(); }); connect(scope_, &QComboBox::currentIndexChanged, this, [this] { filter(); });
        connect(table_, &QTableWidget::currentCellChanged, this, [this](int Row) {
            if (Row < 0) return;
            editScope_ = InputScope(table_->item(Row, ScopeColumn)->data(InputScopeRole).toInt());
            const QSignalBlocker Block(selector_);
            selector_->SetCommand(table_->item(Row, CommandColumn)->data(Qt::UserRole).toString()); key_->setPending({}); updateConflict();
        });
        connect(selector_, &QComboBox::currentTextChanged, this, [this] { editScope_ = HomeScope(selector_->Command()); updateConflict(); });
        connect(Listen, &QPushButton::clicked, key_, &fm::ui::KeyCaptureEdit::startCapture);
        connect(key_, &fm::ui::KeyCaptureEdit::captured, this, [this] { updateConflict(); });
        connect(key_, &fm::ui::KeyCaptureEdit::cleared, this, [this] { updateConflict(); });
        connect(assign_, &QPushButton::clicked, this, [this] { assign(false); }); connect(reassign_, &QPushButton::clicked, this, [this] { assign(true); });
        connect(Clear, &QPushButton::clicked, this, [this] {
            auto* M = TyStCommandMgr::GetInstance(); const auto Id = selector_->Command();
            if (viewScope()) { for (const auto& K : CommandKeys(M->RetrieveForScope(editScope_), Id)) M->DelShortcutInScope(K[0], Id, editScope_); }
            else for (const auto& K : CommandKeys(M->RetrieveForCommand(Id), Id)) M->DelShortcut(K[0], Id);
            Refresh();
        });
        connect(preset_, &QComboBox::currentIndexChanged, this, [this](int I) { if (loading_) return; auto G = settings::AppSettings().Read(settings::Section::General).toObject(); G["shortcutPreset"] = hc::defaults::ShortcutPresetIds().value(I, "legacy"); settings::AppSettings().Put(settings::Section::General, G); Refresh(); });
        connect(Reset, &QPushButton::clicked, this, [this] { if (hc::ui::MessageDialog::question(this, tr("단축키 초기화"), tr("사용자 키를 지우고 선택한 기본값으로 되돌리시겠습니까?")) != QMessageBox::Yes) return; settings::AppSettings().Put(settings::Section::Shortcuts, QJsonObject{}); Refresh(); });
        connect(resetScope_, &QPushButton::clicked, this, [this] {
            const auto Scope = scope_->currentIndex() == ProcessCategory ? InputScope::ProcessView : InputScope::ServiceView;
            if (hc::ui::MessageDialog::question(this, tr("단축키 초기화"), tr("%1 범위의 사용자 키를 지우고 기본값으로 되돌리시겠습니까?").arg(scope_->currentText())) != QMessageBox::Yes) return;
            TyStCommandMgr::GetInstance()->ResetScope(Scope); Refresh();
        });
        Load();
    }
    void Load() {
        loading_ = true; const auto G = settings::AppSettings().Read(settings::Section::General).toObject();
        preset_->setCurrentIndex(qMax(0, hc::defaults::ShortcutPresetIds().indexOf(G["shortcutPreset"].toString(hc::defaults::DefaultValue("general/shortcutPreset").toString()))));
        for (int Kind = 0; Kind < 3; ++Kind) {
            const auto Value = FunctionBarKind(Kind);
            drafts_[Kind].commands_ = FunctionCommandsFor(G, Value);
            const auto Captions = Kind == 0 ? G["functionCaptions"].toArray() : G["functionBars"].toObject()[FunctionBarKey(Value)].toObject()["captions"].toArray();
            drafts_[Kind].captions_.clear();
            for (int I = 0; I < 9; ++I) drafts_[Kind].captions_.append(I < Captions.size() ? Captions[I].toString() : QString{});
        }
        { const QSignalBlocker Block(functionKind_); functionKind_->setCurrentIndex(0); } shownKind_ = 0; showFunctions(0);
        loading_ = false; Refresh();
    }
    bool SaveFunctions(QJsonObject& G) {
        storeFunctions(shownKind_);
        for (int Kind = 0; Kind < 3; ++Kind)
            for (const auto& Id : drafts_[Kind].commands_)
                if (FindCommand(Id).Name.isEmpty()) { conflict_->setText(tr("기능 막대에 등록되지 않은 명령이 있습니다.")); return false; }
        G["functionCommands"] = QJsonArray::fromStringList(drafts_[0].commands_); G["functionCaptions"] = QJsonArray::fromStringList(drafts_[0].captions_);
        auto Bars = G["functionBars"].toObject();
        for (int Kind = 1; Kind < 3; ++Kind) {
            QJsonObject Bar{{"commands", QJsonArray::fromStringList(drafts_[Kind].commands_)}, {"captions", QJsonArray::fromStringList(drafts_[Kind].captions_)}};
            Bars[FunctionBarKey(FunctionBarKind(Kind))] = Bar;
        }
        G["functionBars"] = Bars; return true;
    }
    void Refresh() {
        const QSignalBlocker BlockTable(table_);
        auto* M = TyStCommandMgr::GetInstance(); M->Refresh();
        auto DefaultGeneral = settings::AppSettings().Read(settings::Section::General).toObject(); DefaultGeneral["useBuiltInShortcuts"] = true;
        const auto Values = M->Retrieve(), Defaults = DefaultShortcuts(DefaultGeneral);
        const auto Id = selector_->Command(); const auto KeepScope = editScope_; table_->setRowCount(0);
        for (int I = 0; I < selector_->count(); ++I) { const auto Command = selector_->itemData(I).toString(); if (FindCommand(Command).Name.isEmpty()) continue;
            const auto Label = selector_->itemText(I).section(" · ", 0, 0);
            // 두 보기가 함께 쓰는 명령은 범위마다 한 줄씩 보인다(키를 따로 저장한다).
            QList<InputScope> Scopes{HomeScope(Command)};
            if (IsSharedViewCommand(Command)) Scopes = {InputScope::ProcessView, InputScope::ServiceView};
            for (const auto Scope : Scopes) {
                const int Row = table_->rowCount(); table_->insertRow(Row);
                auto* Item = new QTableWidgetItem(Command); Item->setData(Qt::UserRole, Command); Item->setToolTip(Command); table_->setItem(Row, CommandColumn, Item);
                auto* Description = new QTableWidgetItem(Label); Description->setToolTip(Label); table_->setItem(Row, DescriptionColumn, Description);
                const auto Join = [&](const auto& Keys) { QStringList Labels; for (const auto& K : CommandKeys(Keys, Command)) Labels.append(K.toString()); return Labels.join(", "); };
                const bool CommandInput = IsCommandLineShortcut(Command);
                const bool View = Scope == InputScope::ProcessView || Scope == InputScope::ServiceView;
                const auto Current = View ? M->RetrieveForScope(Scope) : CommandInput ? M->RetrieveForCommand(Command) : Values;
                auto* CurrentItem = new QTableWidgetItem(Join(Current)); table_->setItem(Row, CurrentKeyColumn, CurrentItem);
                if (View) {
                    // 보기 범위 키가 가리는 전역 명령을 알려 준다.
                    QStringList Shadowed;
                    for (const auto& K : CommandKeys(Current, Command))
                        if (const auto Global = CommandId(Values.value(K[0])); !Global.isEmpty()) Shadowed.append(tr("%1 → 전역 '%2'").arg(K.toString(), FindCommand(Global).Desc));
                    if (!Shadowed.isEmpty()) CurrentItem->setToolTip(tr("이 보기에서는 아래 전역 명령보다 먼저 처리합니다:\n%1").arg(Shadowed.join('\n')));
                }
                const auto DefaultKeys = Join(View ? DefaultScopeShortcuts(Scope) : CommandInput ? DefaultCommandLineShortcuts() : Defaults);
                CurrentItem->setData(CombinedKeyDelegate::DefaultRole, DefaultKeys);
                CurrentItem->setToolTip(CurrentItem->toolTip() + tr("\n현재 키: %1\n기본값: %2").arg(Join(Current), DefaultKeys));
                auto* Global = new QTableWidgetItem;
                if (!View && !CommandInput) {
                    Global->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                    Global->setCheckState(M->GlobalShortcutEnabled(Command) ? Qt::Checked : Qt::Unchecked);
                    Global->setText(M->GlobalShortcutError(Command).isEmpty() ? QString{} : tr("실패"));
                    Global->setToolTip(M->GlobalShortcutError(Command).isEmpty() ? tr("다른 앱 사용 중에도 현재 키로 실행합니다. 키가 없으면 등록하지 않습니다.") : M->GlobalShortcutError(Command));
                } else { Global->setFlags(Qt::ItemIsSelectable); Global->setToolTip(tr("이 입력 범위에서만 실행하는 명령입니다.")); }
                table_->setItem(Row, GlobalColumn, Global);
                const int Category = View ? (Scope == InputScope::ProcessView ? ProcessCategory : ServiceCategory) : category(Command);
                table_->setItem(Row, ScopeColumn, new QTableWidgetItem(scope_->itemText(Category)));
                table_->item(Row, ScopeColumn)->setData(Qt::UserRole, Category); table_->item(Row, ScopeColumn)->setData(InputScopeRole, int(Scope));
            }
        }
        { const QSignalBlocker Block(selector_); selector_->SetCommand(Id); }
        editScope_ = Id.isEmpty() || !ScopeAccepts(KeepScope, Id) ? HomeScope(Id) : KeepScope;
        table_->resizeRowsToContents();
        filter(); updateConflict();
    }
private:
    struct FunctionDraft { QStringList commands_, captions_; };
    static int category(const QString& Id) { if (IsCommandLineShortcut(Id)) return CommandLineCategory; if (Id.startsWith("hc.process.") && Id != "hc.process.run") return ProcessCategory; if (Id.startsWith("hc.service.")) return ServiceCategory; if (Id.startsWith("hc.audio.")) return AudioCategory; if (Id.startsWith("hc.file.") || Id.startsWith("hc.archive.") || Id.startsWith("hc.viewer.") || Id.startsWith("hc.jobs.")) return FileCategory; if (Id.startsWith("hc.panel.") || Id.startsWith("hc.tab.") || Id.startsWith("hc.view.") || Id.startsWith("hc.selection.")) return PanelCategory; if (Id.startsWith("hc.navigate.") || Id.startsWith("hc.search.")) return NavigateCategory; return WindowCategory; }
    bool viewScope() const { return editScope_ == InputScope::ProcessView || editScope_ == InputScope::ServiceView; }
    TyMapShortcutToCMDStr scopeKeys(const QString& Id) const { auto* M = TyStCommandMgr::GetInstance(); return viewScope() ? M->RetrieveForScope(editScope_) : M->RetrieveForCommand(Id); }
    void filter() {
        const auto Text = search_->text().trimmed();
        for (int R = 0; R < table_->rowCount(); ++R) { QString Match; for (int C = 0; C < ColumnCount; ++C) Match += " " + table_->item(R, C)->text() + " " + table_->item(R, C)->data(CombinedKeyDelegate::DefaultRole).toString(); table_->setRowHidden(R, (scope_->currentIndex() && table_->item(R, ScopeColumn)->data(Qt::UserRole) != scope_->currentIndex()) || !Match.contains(Text, Qt::CaseInsensitive)); }
        resetScope_->setVisible(scope_->currentIndex() == ProcessCategory || scope_->currentIndex() == ServiceCategory);
    }
    void updateConflict() {
        const auto Key = key_->pending(); const auto Id = selector_->Command();
        const auto Existing = Key.isEmpty() ? QString{} : CommandId(scopeKeys(Id).value(Key[0]));
        const bool Protected = viewScope() && !Key.isEmpty() && IsProtectedScopeKey(Key[0]);
        const bool Valid = !Key.isEmpty() && Key.count() == 1 && !FindCommand(Id).Name.isEmpty() && !Protected;
        const bool Conflict = Valid && !Existing.isEmpty() && Existing != Id; assign_->setEnabled(Valid && !Conflict); reassign_->setVisible(Conflict);
        QString Shadow;
        if (Valid && viewScope())
            if (const auto Global = CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(Key[0])); !Global.isEmpty())
                Shadow = tr(" 이 보기에서는 전역 명령 '%1'보다 먼저 처리합니다.").arg(FindCommand(Global).Desc);
        conflict_->setText(Protected ? tr("%1은(는) 패널 전환·탭·찾기에 쓰는 키라 보기 범위에서 다시 정의할 수 없습니다.").arg(Key.toString())
            : Conflict ? tr("%1은(는) 같은 입력 범위의 '%2'에서 사용 중입니다. 재지정하면 기존 명령에서 이 키가 해제됩니다.").arg(Key.toString(), FindCommand(Existing).Desc) + Shadow
            : Valid ? tr("적용 전 초안으로 키를 지정할 수 있습니다. 명령행 실행 키는 명령행 입력 칸에서만 사용합니다.") + Shadow
            : tr("명령을 선택하고 키 누르기를 실행하세요. Esc로 입력을 취소합니다."));
    }
    void assign(bool Replace) {
        const auto K = key_->pending(); if (K.isEmpty() || K.count() != 1) return; auto* M = TyStCommandMgr::GetInstance();
        const auto Id = selector_->Command();
        if (viewScope() && IsProtectedScopeKey(K[0])) return;
        const auto Existing = CommandId(scopeKeys(Id).value(K[0])); if (!Replace && !Existing.isEmpty() && Existing != Id) return;
        if (viewScope()) M->SetShortcutInScope(K[0], Id, editScope_); else M->SetShortcutWithCMD(K[0], Id);
        Refresh();
    }
    void storeFunctions(int Kind) {
        if (Kind < 0 || Kind > 2) return;
        drafts_[Kind].commands_.clear(); drafts_[Kind].captions_.clear();
        for (int I = 0; I < functions_.size(); ++I) { drafts_[Kind].commands_.append(functions_[I]->Command()); drafts_[Kind].captions_.append(captions_[I]->text()); }
    }
    void showFunctions(int Kind) {
        for (int I = 0; I < functions_.size(); ++I) { functions_[I]->SetCommand(drafts_[Kind].commands_.value(I)); captions_[I]->setText(drafts_[Kind].captions_.value(I)); }
    }
    QLineEdit* search_; QComboBox *scope_, *preset_, *functionKind_; QTableWidget* table_; CommandSelector* selector_; fm::ui::KeyCaptureEdit* key_; QLabel* conflict_; QPushButton *assign_, *reassign_, *resetScope_; QList<CommandSelector*> functions_; QList<QLineEdit*> captions_; bool loading_ = false;
    InputScope editScope_ = InputScope::Global; int shownKind_ = 0; FunctionDraft drafts_[3];
};
}
