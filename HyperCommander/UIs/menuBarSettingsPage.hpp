#pragma once
#include "UniqueLibs/userMenu.hpp"
#include "commandSelector.hpp"
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
#include <memory>

namespace hc::ui {
class MenuBarSettingsPage final : public QWidget {
public:
    explicit MenuBarSettingsPage(QWidget* Parent = nullptr) : QWidget(Parent) {
        setObjectName("hc.settings.menuBar"); auto* Root = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("메뉴바"));
        Header->setSubtitle(tr("메뉴 이름·순서·명령을 편집합니다. 접근 키는 파일(&F)처럼 &를 붙입니다.")); Root->addWidget(Header);
        preview_ = new QMenuBar(this); preview_->setObjectName("hc.menu.preview"); Root->addWidget(preview_);
        auto* Split = new QSplitter(this); Root->addWidget(Split, 1);
        auto* Left = new fm::ui::Card(Split); auto* LeftRows = new QVBoxLayout(Left);
        tree_ = new QTreeWidget(Left); tree_->setObjectName("hc.menu.tree"); tree_->setHeaderLabels({tr("메뉴 · 명령"), tr("ID")});
        tree_->setDragDropMode(QAbstractItemView::InternalMove); tree_->setSelectionMode(QAbstractItemView::SingleSelection);
        tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch); tree_->setColumnHidden(1, true); LeftRows->addWidget(tree_, 1);
        auto* Actions = new QGridLayout; LeftRows->addLayout(Actions);
        const auto Button = [this](QGridLayout* Into, const QString& Text, const char* Name, int Row, int Col, auto Handler) {
            auto* B = new QPushButton(Text, this); B->setObjectName(Name); Into->addWidget(B, Row, Col); connect(B, &QPushButton::clicked, this, Handler);
        };
        Button(Actions, tr("메뉴 추가"), "hc.menu.addRoot", 0, 0, [this] { addNode(0); });
        Button(Actions, tr("하위 메뉴"), "hc.menu.addSub", 0, 1, [this] { addNode(1); });
        Button(Actions, tr("명령 추가"), "hc.menu.addCommand", 1, 0, [this] { addNode(2); });
        Button(Actions, tr("구분선"), "hc.menu.addSeparator", 1, 1, [this] { addNode(3); });
        Button(Actions, tr("위로"), "hc.menu.up", 2, 0, [this] { moveNode(-1); });
        Button(Actions, tr("아래로"), "hc.menu.down", 2, 1, [this] { moveNode(1); });
        Button(Actions, tr("삭제"), "hc.menu.remove", 3, 0, [this] { delete tree_->currentItem(); updatePreview(); });
        auto* Right = new fm::ui::Card(Split); auto* Form = new QFormLayout(Right);
        label_ = new QLineEdit(Right); label_->setObjectName("hc.menu.label"); Form->addRow(tr("표시 이름(&N)"), label_);
        id_ = new QLabel(Right); id_->setTextFormat(Qt::PlainText); id_->setWordWrap(true); Form->addRow(tr("고정 ID"), id_);
        command_ = new CommandSelector(Right); command_->setObjectName("hc.menu.command"); Form->addRow(tr("명령(&C)"), command_);
        auto* Help = new QLabel(tr("명령의 표시 이름을 비우면 현재 명령 이름을 사용합니다. 단축키는 키보드 페이지에서 변경합니다.\n동적 플러그인·자동화 메뉴의 ID와 종류는 유지됩니다. 위 미리보기는 명령을 실행하지 않습니다."), Right); Help->setWordWrap(true); Form->addRow(Help);
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { loadItem(); });
        connect(tree_->model(), &QAbstractItemModel::rowsMoved, this, [this] { updatePreview(); });
        connect(label_, &QLineEdit::textEdited, this, [this] { storeItem(); });
        connect(command_, &QComboBox::currentTextChanged, this, [this] { storeItem(); });
        auto* Files = new QGridLayout; Root->addLayout(Files);
        Button(Files, tr("가져오기(&I)..."), "hc.menu.import", 0, 0, [this] {
            const auto Name = QFileDialog::getOpenFileName(this, tr("메뉴 가져오기"), {}, "JSON (*.json)"); if (Name.isEmpty()) return;
            QFile File(Name); if (!File.open(QIODevice::ReadOnly) || File.size() > 1024 * 1024) { status_->setText(tr("메뉴 파일을 읽을 수 없습니다.")); return; }
            Import(File.readAll());
        });
        Button(Files, tr("내보내기(&E)..."), "hc.menu.export", 0, 1, [this] {
            if (!Validate()) return; const auto Name = QFileDialog::getSaveFileName(this, tr("메뉴 내보내기"), {}, "JSON (*.json)"); if (Name.isEmpty()) return;
            if (QFileInfo(Name).absoluteFilePath() == QFileInfo(path_).absoluteFilePath()) { status_->setText(tr("사용 중인 메뉴 파일은 적용/확인으로 저장하세요.")); return; }
            QSaveFile File(Name); File.setDirectWriteFallback(false); const auto Data = Bytes();
            if (!File.open(QIODevice::WriteOnly) || File.write(Data) != Data.size() || !File.commit()) status_->setText(tr("내보내기에 실패했습니다."));
        });
        Button(Files, tr("기본값으로(&R)"), "hc.menu.reset", 0, 2, [this] { Import(defaults()); });
        status_ = new QLabel(this); status_->setObjectName("hc.menu.status"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); Root->addWidget(status_);
        pathLabel_ = new QLabel(this); pathLabel_->setTextFormat(Qt::PlainText); pathLabel_->setWordWrap(true); Root->addWidget(pathLabel_);
        Reload();
    }
    void Reload() {
        CancelPrepared(); path_ = QDir(settings::Paths().directory_).filePath("menu-bar.json"); pathLabel_->setText(path_);
        exists_ = QFileInfo::exists(path_); QFile File(path_); original_ = {};
        if (exists_ && (!File.open(QIODevice::ReadOnly) || File.size() > 1024 * 1024)) { editable_ = false; tree_->clear(); status_->setText(tr("원본을 읽을 수 없습니다. 가져오기 또는 기본값 복원으로 초안을 만들 수 있습니다.")); return; }
        if (exists_) original_ = File.readAll();
        editable_ = Import(exists_ ? original_ : defaults()); base_ = Document();
    }
    bool Import(const QByteArray& Data) {
        const auto D = MenuDefinition::Parse(Data, command_->Commands(), {"plugins", "automation"});
        if (!D.error_.isEmpty()) { status_->setText(tr("메뉴 형식 오류: %1").arg(D.error_)); return false; }
        loading_ = true; tree_->clear();
        std::function<void(QTreeWidgetItem*, const QJsonArray&)> Fill = [&](QTreeWidgetItem* Parent, const QJsonArray& Items) {
            for (const auto V : Items) { const auto O = V.toObject(); auto* Item = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(tree_);
                auto Meta = O; Meta.remove("items"); Item->setData(0, Qt::UserRole, Meta); setItemText(Item);
                if (O.contains("items")) Fill(Item, O["items"].toArray());
                else Item->setFlags(Item->flags() & ~Qt::ItemIsDropEnabled);
            }
        };
        Fill(nullptr, QJsonDocument::fromJson(Data).object()["menus"].toArray()); loading_ = false; editable_ = true;
        tree_->expandAll(); tree_->setCurrentItem(tree_->topLevelItem(0)); updatePreview(); return true;
    }
    QJsonObject Document() const {
        std::function<QJsonArray(QTreeWidgetItem*)> Read = [&](QTreeWidgetItem* Parent) {
            QJsonArray Items; const int N = Parent ? Parent->childCount() : tree_->topLevelItemCount();
            for (int I = 0; I < N; ++I) { auto* Item = Parent ? Parent->child(I) : tree_->topLevelItem(I); auto O = Item->data(0, Qt::UserRole).toJsonObject();
                if (!O.contains("command") && !O.contains("section") && !O.contains("type")) O["items"] = Read(Item); Items.append(O);
            } return Items;
        };
        return {{"schemaVersion", 1}, {"menus", Read(nullptr)}};
    }
    QByteArray Bytes() const { auto Data = QJsonDocument(Document()).toJson(); Data.replace("\n", "\r\n"); return Data; }
    bool Dirty() const { return editable_ && Document() != base_; }
    bool Validate() {
        if (!editable_) return !Dirty();
        const auto D = MenuDefinition::Parse(Bytes(), command_->Commands(), {"plugins", "automation"});
        status_->setText(D.error_.isEmpty() ? D.issues_.isEmpty() ? tr("메뉴 구성을 확인했습니다. 적용 전 미리보기입니다.") : D.issues_.join("\n") : tr("메뉴 형식 오류: %1").arg(D.error_));
        return D.error_.isEmpty();
    }
    bool Prepare() {
        CancelPrepared(); if (!Dirty()) return true; if (!Validate()) return false;
        lock_ = std::make_unique<QLockFile>(path_ + ".lock"); lock_->setStaleLockTime(0);
        if (!lock_->tryLock(0)) return fail(tr("메뉴 파일을 다른 작업이 사용 중입니다."));
        QFile Current(path_); const bool Exists = QFileInfo::exists(path_);
        if (Exists != exists_ || (Exists && (!Current.open(QIODevice::ReadOnly) || Current.size() > 1024 * 1024 || Current.readAll() != original_))) return fail(tr("메뉴 파일이 외부에서 변경되었습니다. 다시 열어 병합하세요."));
        prepared_ = std::make_unique<QSaveFile>(path_); prepared_->setDirectWriteFallback(false); const auto Data = Bytes();
        if (!prepared_->open(QIODevice::WriteOnly) || prepared_->write(Data) != Data.size()) return fail(tr("메뉴 저장을 준비하지 못했습니다."));
        return true;
    }
    bool CommitPrepared() {
        if (!prepared_) return true;
        if (!prepared_->commit()) return fail(tr("메뉴 저장에 실패했습니다. 실행 중 메뉴와 기존 파일을 유지합니다."));
        original_ = Bytes(); exists_ = true; base_ = Document(); CancelPrepared(); return true;
    }
    void CancelPrepared() { prepared_.reset(); lock_.reset(); }
private:
    static QByteArray defaults() { QFile File(":/HyperCommander/menu-bar.json"); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    bool fail(const QString& Text) { status_->setText(Text); CancelPrepared(); return false; }
    void setItemText(QTreeWidgetItem* Item) {
        const auto O = Item->data(0, Qt::UserRole).toJsonObject();
        Item->setText(0, O.contains("type") ? tr("──── 구분선 ────") : O["label"].toString().isEmpty() ? O["command"].toString() : O["label"].toString()); Item->setText(1, O["id"].toString());
    }
    void loadItem() {
        loading_ = true; auto* Item = tree_->currentItem(); const auto O = Item ? Item->data(0, Qt::UserRole).toJsonObject() : QJsonObject{};
        label_->setEnabled(Item && !O.contains("type")); command_->setEnabled(O.contains("command")); label_->setText(O["label"].toString()); id_->setText(O["id"].toString()); command_->SetCommand(O["command"].toString()); loading_ = false;
    }
    void storeItem() {
        auto* Item = tree_->currentItem(); if (loading_ || !Item) return; auto O = Item->data(0, Qt::UserRole).toJsonObject();
        if (O.contains("type")) return; if (label_->text().isEmpty()) O.remove("label"); else O["label"] = label_->text();
        if (O.contains("command")) O["command"] = command_->Command(); Item->setData(0, Qt::UserRole, O); setItemText(Item); updatePreview();
    }
    void addNode(int Kind) {
        auto* Parent = Kind == 0 ? nullptr : tree_->currentItem();
        while (Parent && !(Parent->flags() & Qt::ItemIsDropEnabled)) Parent = Parent->parent();
        if (Kind != 0 && !Parent) { status_->setText(tr("먼저 추가할 메뉴를 선택하세요.")); return; }
        QJsonObject O;
        if (Kind == 3) O["type"] = "separator";
        else { O["id"] = "user." + QUuid::createUuid().toString(QUuid::WithoutBraces); O["label"] = Kind < 2 ? tr("새 메뉴") : tr("새 명령"); if (Kind == 2) O["command"] = "hc.file.copy"; }
        auto* Item = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(tree_); Item->setData(0, Qt::UserRole, O);
        if (Kind >= 2) Item->setFlags(Item->flags() & ~Qt::ItemIsDropEnabled); setItemText(Item); tree_->setCurrentItem(Item); if (Parent) Parent->setExpanded(true); editable_ = true; updatePreview();
    }
    void moveNode(int Delta) {
        auto* Item = tree_->currentItem(); if (!Item) return; auto* Parent = Item->parent(); const int Row = Parent ? Parent->indexOfChild(Item) : tree_->indexOfTopLevelItem(Item), Target = Row + Delta;
        if (Target < 0 || Target >= (Parent ? Parent->childCount() : tree_->topLevelItemCount())) return;
        if (Parent) { Parent->takeChild(Row); Parent->insertChild(Target, Item); } else { tree_->takeTopLevelItem(Row); tree_->insertTopLevelItem(Target, Item); } tree_->setCurrentItem(Item); updatePreview();
    }
    void updatePreview() {
        if (!preview_ || !status_) return; preview_->clear(); qDeleteAll(preview_->findChildren<QMenu*>(QString{}, Qt::FindDirectChildrenOnly));
        const auto D = MenuDefinition::Parse(Bytes(), command_->Commands(), {"plugins", "automation"});
        if (!D.error_.isEmpty()) { status_->setText(tr("메뉴 형식 오류: %1").arg(D.error_)); return; }
        std::function<void(QMenu*, const QList<MenuNode>&)> Fill = [&](QMenu* Menu, const QList<MenuNode>& Nodes) {
            for (const auto& N : Nodes) { if (N.separator_) { Menu->addSeparator(); continue; }
                if (N.command_.isEmpty() && N.section_.isEmpty()) { auto* Sub = Menu->addMenu(N.label_); Fill(Sub, N.items_); }
                else { auto Text = N.label_.isEmpty() ? N.command_.isEmpty() ? N.section_ : N.command_ : N.label_;
                    QStringList Keys; for (const auto& Key : CommandKeys(TyStCommandMgr::GetInstance()->Retrieve(), N.command_)) Keys.append(Key.toString());
                    auto* A = Menu->addAction(Text + (Keys.isEmpty() ? QString{} : "\t" + Keys.front())); A->setEnabled(false);
                }
            }
        };
        for (const auto& N : D.menus_) { auto* Menu = preview_->addMenu(N.label_); Fill(Menu, N.items_); } Validate();
    }
    QTreeWidget* tree_; QLineEdit* label_; QLabel *id_, *status_, *pathLabel_; CommandSelector* command_; QMenuBar* preview_;
    QString path_; QByteArray original_; QJsonObject base_; bool loading_ = false, exists_ = false, editable_ = false;
    std::unique_ptr<QLockFile> lock_; std::unique_ptr<QSaveFile> prepared_;
};
}
