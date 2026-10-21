#pragma once

#include <QtWidgets>
#include <functional>
#include <limits>
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/document/documentHandlerSettings.hpp"
#include "UniqueLibs/productDefaults.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include <fmwidgets/DialogHeader.h>

namespace hc::ui {
// Only changed leaves are applied after the other preferences pages have saved
// their drafts. Unknown fields and edits owned by those pages remain intact.
class LegacySettingsPage final : public QWidget {
public:
    explicit LegacySettingsPage(QWidget* Parent = nullptr) : QWidget(Parent) {
        setObjectName("hc.settings.legacy");
        auto* Rows = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("레거시"));
        Header->setSubtitle(tr("기존 설정을 변경하여 동작을 확인합니다. 변경은 적용·확인으로 저장합니다.")); Rows->addWidget(Header);
        tabs_ = new QTabWidget(this); tabs_->setObjectName("hc.legacy.tabs"); Rows->addWidget(tabs_, 1);
        createColors(); createDocuments(); createRules(); createPreserved();
        error_ = new QLabel(this); error_->setWordWrap(true); error_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        error_->setObjectName("hc.legacy.error"); Rows->addWidget(error_);
    }

    void Load(const QJsonObject& Document, const QJsonObject& Workspace = {}) {
        loading_ = true; base_ = draft_ = Document; pending_ = {}; useScheme_ = false; error_->clear();
        refreshSchemes(Document["appearance"].toObject()["selected"].toString());
        builtInKeys_->setChecked(Document["general"].toObject()["useBuiltInShortcuts"].toBool(true));
        const auto Handlers = documentHandlers();
        view_->setEditText(Handlers["defaultViewHandlerId"].toString("builtin.hyperviewer"));
        edit_->setEditText(Handlers["defaultEditHandlerId"].toString("builtin.hypereditor"));
        policy_->setEditText(Handlers["selectionPolicy"].toString("askWhenMultiple"));
        const auto Mode = Document["general"].toObject()["themeMode"].toString();
        mode_->setCurrentIndex(qMax(0, mode_->findData(Mode)));
        fillObjects(handlerTable_, Handlers["handlers"].toArray(), {"stableId", "enabled", "view", "edit"}, true);
        QJsonArray Extensions;
        const auto Overrides = Handlers["fileTypeOverrides"].toObject();
        for (auto It = Overrides.begin(); It != Overrides.end(); ++It) { auto Row = It.value().toObject(); Row["extension"] = It.key(); Extensions.append(Row); }
        fillObjects(extensionTable_, Extensions, {"extension", "view", "edit"});
        fillObjects(fileSets_, Document["fileSets"].toArray(), {"name", "flags", "filters", "extensions", "attributes", "size"});
        QJsonArray Wlx;
        for (const auto Value : Document["plugins"].toObject()["wlx"].toArray()) {
            const auto Record = Value.toObject(); Wlx.append(QJsonObject{{"path", Record["path"]}, {"detect", Record["detect"]}});
        }
        fillObjects(wlx_, Wlx, {"path", "detect"});
        for (int Row = 0; Row < wlx_->rowCount(); ++Row) wlx_->item(Row, 0)->setFlags(wlx_->item(Row, 0)->flags() & ~Qt::ItemIsEditable);
        for (auto& Area : json_) Area.editor_->setPlainText(jsonText(value(Document, Area.path_).toObject()));
        auto Snapshot = Document; if (!Workspace.isEmpty()) Snapshot["workspace"] = Workspace;
        inspector_->setPlainText(jsonText(Snapshot));
        loading_ = false;
    }

    bool Prepare() {
        error_->clear(); pending_ = draft_;
        for (int Row = 0; Row < colors_.size(); ++Row) {
            if (colors_[Row].enabled_->isChecked() && !validColor(colors_[Row].value_->text()))
                return fail(tr("%1: #RRGGBB 형식으로 색상을 입력하세요.").arg(colors_[Row].caption_), 0, colors_[Row].value_);
        }
        // Color edits are stored as they happen, including across scheme changes.
        const auto Schemes = draft_["appearance"].toObject()["schemes"].toObject();
        for (auto It = Schemes.begin(); It != Schemes.end(); ++It) {
            const auto Record = It.value().toObject(); const auto Values = Record["colors"].toObject();
            for (auto C = Values.begin(); C != Values.end(); ++C) if (!validColor(C.value().toString()))
                return fail(tr("%1 / %2: 색상 형식을 확인하세요.").arg(It.key(), C.key()), 0, schemes_);
        }
        const auto StoreText = [&](const QStringList& Path, const QString& Text) {
            if (Text != value(base_, Path).toString()) set(pending_, Path, Text);
        };
        if (builtInKeys_->isChecked() != base_["general"].toObject()["useBuiltInShortcuts"].toBool(true))
            set(pending_, {"general", "useBuiltInShortcuts"}, builtInKeys_->isChecked());
        const QStringList HandlerPath{"editors", "documentHandlers"};
        StoreText(HandlerPath + QStringList{"defaultViewHandlerId"}, view_->currentText().trimmed());
        StoreText(HandlerPath + QStringList{"defaultEditHandlerId"}, edit_->currentText().trimmed());
        StoreText(HandlerPath + QStringList{"selectionPolicy"}, policy_->currentText().trimmed());
        if (mode_->currentData().toString() != base_["general"].toObject()["themeMode"].toString())
            set(pending_, {"general", "themeMode"}, mode_->currentData().toString());
        QJsonArray Handlers; QSet<QString> HandlerIds;
        for (int Row = 0; Row < handlerTable_->rowCount(); ++Row) {
            auto Record = rowObject(handlerTable_, Row, {"stableId", "enabled", "view", "edit"}, true);
            const auto Id = Record["stableId"].toString().trimmed();
            if (Id.isEmpty() || HandlerIds.contains(Id)) return fail(tr("처리기 ID는 비워 둘 수 없으며 중복할 수 없습니다."), 1, handlerTable_);
            Record["stableId"] = Id; HandlerIds.insert(Id); Handlers.append(Record);
        }
        if (Handlers != documentHandlers()["handlers"].toArray()) set(pending_, HandlerPath + QStringList{"handlers"}, Handlers);
        QJsonObject Overrides; QSet<QString> Extensions;
        static const QRegularExpression InvalidExtension(R"([.*/\\\s])");
        for (int Row = 0; Row < extensionTable_->rowCount(); ++Row) {
            auto Record = rowObject(extensionTable_, Row, {"extension", "view", "edit"});
            const auto Ext = Record.take("extension").toString().trimmed().toLower();
            if (Ext.isEmpty() || Ext.contains(InvalidExtension) || Extensions.contains(Ext))
                return fail(tr("확장자는 점·경로 없이 입력하고 중복을 제거하세요."), 1, extensionTable_);
            Extensions.insert(Ext); Overrides[Ext] = Record;
        }
        if (Overrides != documentHandlers()["fileTypeOverrides"].toObject()) set(pending_, HandlerPath + QStringList{"fileTypeOverrides"}, Overrides);
        QJsonArray Sets; QSet<QString> Names;
        for (int Row = 0; Row < fileSets_->rowCount(); ++Row) {
            auto Record = rowObject(fileSets_, Row, {"name", "flags", "filters", "extensions", "attributes", "size"});
            const auto Name = Record["name"].toString().trimmed();
            if (Name.isEmpty() || Names.contains(Name)) return fail(tr("파일 집합 이름은 비워 둘 수 없으며 중복할 수 없습니다."), 2, fileSets_);
            Record["name"] = Name; Names.insert(Name);
            for (int Column : {1, 4, 5}) {
                bool Ok = false; const auto Number = fileSets_->item(Row, Column)->text().trimmed().toLongLong(&Ok, 0);
                if (!Ok || Number < 0 || (Column != 5 && quint64(Number) > std::numeric_limits<unsigned>::max()))
                    return fail(tr("파일 집합의 플래그·속성·크기는 0 이상의 정수로 입력하세요. 플래그·속성에는 0x 표기도 가능합니다."), 2, fileSets_);
                Record[QStringList{"name", "flags", "filters", "extensions", "attributes", "size"}[Column]] = Number;
            }
            Sets.append(Record);
        }
        if (Sets != base_["fileSets"].toArray()) pending_["fileSets"] = Sets;
        if (!storeGroupColors()) return false;
        // storeGroupColors updates draft_; transfer only the schemes into the prepared document.
        set(pending_, {"appearance", "schemes"}, draft_["appearance"].toObject()["schemes"]);
        for (auto& Area : json_) {
            QJsonParseError Error; const auto Parsed = QJsonDocument::fromJson(Area.editor_->toPlainText().toUtf8(), &Error);
            if (Error.error != QJsonParseError::NoError || !Parsed.isObject())
                return fail(tr("%1: JSON 객체 형식을 확인하세요. %2").arg(Area.caption_, Error.errorString()), 3, Area.editor_);
            if (Parsed.object() != value(base_, Area.path_).toObject()) set(pending_, Area.path_, Parsed.object());
        }
        return true;
    }

    bool Apply(QJsonObject& Current) {
        if (pending_.isEmpty() && !Prepare()) return false;
        auto Next = settings::UnifiedConfig::Changed(base_, pending_, Current);
        if (useScheme_) set(Next, {"general", "themeMode"}, mode_->currentData().toString());
        if (base_["general"].toObject()["useBuiltInShortcuts"].toBool(true) && !Next["general"].toObject()["useBuiltInShortcuts"].toBool(true)) {
            // SaveSettings materializes the effective key map. Disabling the
            // built-ins must not turn previously implicit keys into overrides.
            auto Keys = Next["shortcuts"].toObject(); const auto Existing = base_["shortcuts"].toObject();
            const auto Defaults = DefaultShortcuts(base_["general"].toObject());
            for (auto It = Defaults.cbegin(); It != Defaults.cend(); ++It) {
                const auto Key = QKeySequence(It.key()).toString(QKeySequence::PortableText);
                if (!Existing.contains(Key) && Keys[Key].toString() == CommandId(It.value())) Keys.remove(Key);
            }
            Next["shortcuts"] = Keys;
        }
        auto Plugins = Next["plugins"].toObject(); auto Wlx = Plugins["wlx"].toArray();
        for (int Row = 0; Row < wlx_->rowCount(); ++Row) {
            const auto Original = wlx_->item(Row, 0)->data(Qt::UserRole).toJsonObject();
            const auto Detect = wlx_->item(Row, 1)->text();
            if (Detect == Original["detect"].toString()) continue;
            bool Found = false;
            for (int Index = 0; Index < Wlx.size(); ++Index) {
                auto Record = Wlx[Index].toObject();
                if (Record["path"] != Original["path"]) continue;
                Record["detect"] = Detect; Wlx[Index] = Record; Found = true; break;
            }
            if (!Found) return fail(tr("감지식을 변경한 WLX가 플러그인 관리에서 제거되었습니다. 목록을 다시 확인하세요."), 2, wlx_);
        }
        Plugins["wlx"] = Wlx; Next["plugins"] = Plugins;
        Current = Next; return true;
    }

private:
    struct ColorRow { QString key_, caption_; QCheckBox* enabled_; QLineEdit* value_; };
    struct JsonArea { QString caption_; QStringList path_; QPlainTextEdit* editor_; };
    QJsonObject base_, draft_, pending_;
    bool loading_ = false, useScheme_ = false;
    QString scheme_;
    QTabWidget* tabs_ = nullptr;
    QLabel *error_ = nullptr, *selected_ = nullptr;
    QComboBox *schemes_ = nullptr, *view_ = nullptr, *edit_ = nullptr, *policy_ = nullptr, *mode_ = nullptr;
    QCheckBox *builtInKeys_ = nullptr, *dark_ = nullptr;
    QPushButton *removeScheme_ = nullptr, *renameScheme_ = nullptr;
    QVector<ColorRow> colors_;
    QVector<JsonArea> json_;
    QTableWidget *handlerTable_ = nullptr, *extensionTable_ = nullptr, *fileSets_ = nullptr, *groupColors_ = nullptr, *wlx_ = nullptr;
    QPlainTextEdit* inspector_ = nullptr;

    static QString jsonText(const QJsonObject& Object) { return QString::fromUtf8(QJsonDocument(Object).toJson(QJsonDocument::Indented)); }
    static bool validColor(const QString& Text) {
        static const QRegularExpression Pattern("^#[0-9a-fA-F]{6}$");
        return Pattern.match(Text.trimmed()).hasMatch();
    }
    static QJsonValue value(QJsonObject Object, const QStringList& Path) {
        return settings::UnifiedConfig::Value(Object, Path);
    }
    static void set(QJsonObject& Object, const QStringList& Path, const QJsonValue& Value) {
        settings::UnifiedConfig::Set(Object, Path, Value);
    }
    bool fail(const QString& Message, int Tab, QWidget* Field) {
        tabs_->setCurrentIndex(Tab); error_->setText(Message); Field->setFocus();
        if (auto* Scroll = qobject_cast<QScrollArea*>(tabs_->widget(Tab))) Scroll->ensureWidgetVisible(Field);
        return false;
    }
    QVBoxLayout* tab(const QString& Name) {
        auto* Scroll = new QScrollArea(tabs_); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        auto* Body = new QWidget; auto* Rows = new QVBoxLayout(Body); Scroll->setWidget(Body); tabs_->addTab(Scroll, Name); return Rows;
    }
    static void note(QVBoxLayout* Rows, const QString& Text) {
        auto* Label = new QLabel(Text, Rows->parentWidget()); Label->setWordWrap(true); Rows->addWidget(Label);
    }
    QTableWidget* table(QVBoxLayout* Rows, const QString& Id, const QStringList& Headers, bool Buttons = true) {
        auto* Table = new QTableWidget(0, Headers.size(), Rows->parentWidget()); Table->setObjectName("hc.legacy." + Id);
        Table->setHorizontalHeaderLabels(Headers); Table->setSelectionBehavior(QAbstractItemView::SelectRows);
        Table->setSelectionMode(QAbstractItemView::SingleSelection); Table->setMinimumHeight(150);
        Table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); Table->verticalHeader()->hide(); Rows->addWidget(Table);
        if (Buttons) {
            auto* Actions = new QHBoxLayout; auto* Add = new QPushButton(tr("추가")); auto* Remove = new QPushButton(tr("제거"));
            Actions->addWidget(Add); Actions->addWidget(Remove); Actions->addStretch(); Rows->addLayout(Actions);
            connect(Add, &QPushButton::clicked, this, [Table] {
                const int Row = Table->rowCount(); Table->insertRow(Row);
                for (int Col = 0; Col < Table->columnCount(); ++Col) {
                    auto* Item = new QTableWidgetItem; if (Col == 0) Item->setData(Qt::UserRole, QJsonObject{});
                    if (Table->property("legacyBools").toBool() && Col > 0) { Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsUserTristate); Item->setCheckState(Qt::PartiallyChecked); }
                    if (Table->objectName() == "hc.legacy.fileSets" && (Col == 1 || Col == 4 || Col == 5)) Item->setText("0");
                    Table->setItem(Row, Col, Item);
                }
                Table->setCurrentCell(Row, 0); Table->editItem(Table->item(Row, 0));
            });
            connect(Remove, &QPushButton::clicked, this, [Table] { if (Table->currentRow() >= 0) Table->removeRow(Table->currentRow()); });
        }
        return Table;
    }
    static QString display(const QJsonValue& Value) {
        if (Value.isArray()) { QStringList Items; for (auto V : Value.toArray()) Items.append(V.toString()); return Items.join("; "); }
        if (Value.isDouble()) return QString::number(Value.toInteger()); return Value.toString();
    }
    static void fillObjects(QTableWidget* Table, const QJsonArray& Items, const QStringList& Keys, bool Bools = false) {
        Table->setProperty("legacyBools", Bools); Table->setRowCount(0);
        for (auto Value : Items) {
            const auto Object = Value.toObject(); const int Row = Table->rowCount(); Table->insertRow(Row);
            for (int Col = 0; Col < Keys.size(); ++Col) {
                auto* Cell = new QTableWidgetItem(display(Object[Keys[Col]]));
                if (!Col) Cell->setData(Qt::UserRole, Object);
                if (Bools && Col) { Cell->setFlags((Cell->flags() & ~Qt::ItemIsEditable) | Qt::ItemIsUserCheckable | Qt::ItemIsUserTristate); Cell->setCheckState(!Object[Keys[Col]].isBool() ? Qt::PartiallyChecked : Object[Keys[Col]].toBool() ? Qt::Checked : Qt::Unchecked); }
                Table->setItem(Row, Col, Cell);
            }
        }
    }
    static QJsonObject rowObject(QTableWidget* Table, int Row, const QStringList& Keys, bool Bools = false) {
        auto Result = Table->item(Row, 0)->data(Qt::UserRole).toJsonObject();
        for (int Col = 0; Col < Keys.size(); ++Col) {
            const auto* Cell = Table->item(Row, Col); const auto& Key = Keys[Col];
            if (Bools && Col) { if (Cell->checkState() == Qt::PartiallyChecked) Result.remove(Key); else Result[Key] = Cell->checkState() == Qt::Checked; }
            else if (Cell->text() != display(Result[Key])) {
                if (Key == "filters" || Key == "extensions") { const auto Parts = Cell->text().split(';', Qt::SkipEmptyParts); QJsonArray Values; for (const auto& Part : Parts) Values.append(Part.trimmed()); Result[Key] = Values; }
                else Result[Key] = Cell->text();
            }
        }
        return Result;
    }
    QJsonObject documentHandlers() const { return base_["editors"].toObject()["documentHandlers"].toObject(); }
    QJsonObject schemeRecord() const { return draft_["appearance"].toObject()["schemes"].toObject()[scheme_].toObject(); }
    void putScheme(QJsonObject Record) { set(draft_, {"appearance", "schemes", scheme_}, Record); }
    bool explicitColor(const QJsonObject& Record, const QString& Key) const {
        if (Record["colorOverrides"].isObject()) return Record["colorOverrides"].toObject()[Key].toBool();
        const auto Name = Record["dark"].toBool() ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트");
        const QColor Color(Record["colors"].toObject()[Key].toString());
        return Color.isValid() && Color != QColor(hc::defaults::Appearance()["schemes"].toObject()[Name].toObject()["colors"].toObject()[Key].toString());
    }
    void storeColor(int Index) {
        if (loading_ || scheme_.isEmpty()) return;
        auto Record = schemeRecord(); auto Flags = Record["colorOverrides"].toObject();
        if (!Record["colorOverrides"].isObject()) for (const auto& Row : colors_) Flags[Row.key_] = explicitColor(Record, Row.key_);
        const auto& Row = colors_[Index]; Flags[Row.key_] = Row.enabled_->isChecked(); Record["colorOverrides"] = Flags;
        auto Values = Record["colors"].toObject(); Values[Row.key_] = Row.value_->text().trimmed(); Record["colors"] = Values; putScheme(Record);
        Row.value_->setEnabled(Row.enabled_->isChecked());
    }
    void refreshSchemes(const QString& Name) {
        const QSignalBlocker Block(schemes_); schemes_->clear(); schemes_->addItems(draft_["appearance"].toObject()["schemes"].toObject().keys());
        schemes_->setCurrentIndex(qMax(0, schemes_->findText(Name))); scheme_ = schemes_->currentText(); showScheme();
    }
    void showScheme() {
        const bool WasLoading = loading_; loading_ = true; const auto Record = schemeRecord();
        dark_->setChecked(Record["dark"].toBool());
        const auto Defaults = hc::defaults::Appearance()["schemes"].toObject()[Record["dark"].toBool() ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트")].toObject()["colors"].toObject();
        for (const auto& Row : colors_) { Row.enabled_->setChecked(explicitColor(Record, Row.key_)); Row.value_->setText(Record["colors"].toObject()[Row.key_].toString(Defaults[Row.key_].toString())); Row.value_->setEnabled(Row.enabled_->isChecked()); }
        const bool Builtin = hc::defaults::Appearance()["schemes"].toObject().contains(scheme_);
        removeScheme_->setEnabled(!Builtin); renameScheme_->setEnabled(!Builtin);
        selected_->setText(tr("사용 중인 구성표: %1").arg(draft_["appearance"].toObject()["selected"].toString()));
        QJsonArray Colors; const auto Mapping = Record["fileSets"].toObject();
        for (auto It = Mapping.begin(); It != Mapping.end(); ++It) { auto Row = It.value().toObject(); Row["name"] = It.key(); Colors.append(Row); }
        if (groupColors_) fillObjects(groupColors_, Colors, {"name", "foreground", "background"});
        loading_ = WasLoading;
    }
    bool storeGroupColors() {
        if (!groupColors_ || scheme_.isEmpty()) return true;
        QJsonObject Mapping;
        for (int Row = 0; Row < groupColors_->rowCount(); ++Row) {
            auto Record = rowObject(groupColors_, Row, {"name", "foreground", "background"}); const auto Name = Record.take("name").toString().trimmed();
            if (Name.isEmpty() || Mapping.contains(Name)) return fail(tr("집합 색상의 이름은 비워 둘 수 없으며 중복할 수 없습니다."), 2, groupColors_);
            for (const auto* Key : {"foreground", "background"}) if (!Record[Key].toString().isEmpty() && !validColor(Record[Key].toString())) return fail(tr("집합 색상은 #RRGGBB 형식으로 입력하거나 비워 두세요."), 2, groupColors_);
            Mapping[Name] = Record;
        }
        auto Record = schemeRecord(); if (Mapping != Record["fileSets"].toObject()) { Record["fileSets"] = Mapping; putScheme(Record); } return true;
    }
    void createColors() {
        auto* Rows = tab(tr("색상·구성표"));
        note(Rows, tr("기존 개별 색상이 테마 색상보다 우선합니다. 개별 지정을 끄면 테마 색상을 따릅니다. 구성표 선택은 조회이며 ‘이 구성표 사용’으로 사용 대상을 변경합니다."));
        auto* Actions = new QHBoxLayout; schemes_ = new QComboBox; schemes_->setObjectName("hc.legacy.scheme"); Actions->addWidget(schemes_, 1);
        auto* Use = new QPushButton(tr("이 구성표 사용")); Use->setObjectName("hc.legacy.useScheme"); Actions->addWidget(Use);
        auto* Clone = new QPushButton(tr("복제…")); Actions->addWidget(Clone); renameScheme_ = new QPushButton(tr("이름 변경…")); Actions->addWidget(renameScheme_);
        removeScheme_ = new QPushButton(tr("삭제")); Actions->addWidget(removeScheme_); Rows->addLayout(Actions);
        selected_ = new QLabel; Rows->addWidget(selected_); dark_ = new QCheckBox(tr("이 구성표는 다크 모드")); dark_->setObjectName("hc.legacy.schemeDark"); Rows->addWidget(dark_);
        auto* Form = new QGridLayout; Form->addWidget(new QLabel(tr("항목")), 0, 0); Form->addWidget(new QLabel(tr("개별 지정")), 0, 1); Form->addWidget(new QLabel(tr("저장 색상 · #RRGGBB")), 0, 2); Rows->addLayout(Form);
        const QStringList Keys{"dialogText", "dialogBackground", "listText", "listBackground", "listCursor", "listSelection"};
        const QStringList Captions{tr("창 글자"), tr("창 바탕"), tr("목록 글자"), tr("목록 바탕"), tr("커서"), tr("선택")};
        for (int Index = 0; Index < Keys.size(); ++Index) {
            auto* Check = new QCheckBox; Check->setObjectName("hc.legacy.override." + Keys[Index]); Check->setAccessibleName(Captions[Index] + tr(" 개별 지정"));
            auto* Text = new QLineEdit; Text->setObjectName("hc.legacy.color." + Keys[Index]); Text->setAccessibleName(Captions[Index]); Text->setMaxLength(7);
            auto* Pick = new QPushButton(tr("색 선택…")); Pick->setAccessibleName(Captions[Index] + tr(" 색 선택"));
            Form->addWidget(new QLabel(Captions[Index]), Index + 1, 0); Form->addWidget(Check, Index + 1, 1); Form->addWidget(Text, Index + 1, 2); Form->addWidget(Pick, Index + 1, 3);
            colors_.append({Keys[Index], Captions[Index], Check, Text});
            connect(Check, &QCheckBox::toggled, this, [this, Index] { storeColor(Index); });
            connect(Text, &QLineEdit::textChanged, this, [this, Index] { storeColor(Index); });
            connect(Pick, &QPushButton::clicked, this, [this, Index] { const auto Color = QColorDialog::getColor(QColor(colors_[Index].value_->text()), this, colors_[Index].caption_); if (Color.isValid()) { colors_[Index].enabled_->setChecked(true); colors_[Index].value_->setText(Color.name()); } });
        }
        auto* Follow = new QPushButton(tr("이 구성표의 색상 6종을 모두 테마 따름")); Follow->setObjectName("hc.legacy.followTheme"); Rows->addWidget(Follow);
        connect(Follow, &QPushButton::clicked, this, [this] { for (const auto& Row : colors_) Row.enabled_->setChecked(false); });
        connect(dark_, &QCheckBox::toggled, this, [this](bool Dark) { if (!loading_) { auto Record = schemeRecord(); Record["dark"] = Dark; putScheme(Record); } });
        connect(schemes_, &QComboBox::currentTextChanged, this, [this](const QString& Name) { if (loading_) return; if (!storeGroupColors()) { const QSignalBlocker Block(schemes_); schemes_->setCurrentText(scheme_); return; } scheme_ = Name; showScheme(); });
        connect(Use, &QPushButton::clicked, this, [this] { useScheme_ = true; set(draft_, {"appearance", "selected"}, scheme_); set(draft_, {"general", "themeMode"}, QString()); mode_->setCurrentIndex(0); selected_->setText(tr("사용 중인 구성표: %1").arg(scheme_)); });
        const auto NameScheme = [this](bool Rename) {
            if (!storeGroupColors()) return; bool Ok = false; const auto Name = QInputDialog::getText(this, Rename ? tr("구성표 이름 변경") : tr("구성표 복제"), tr("이름"), QLineEdit::Normal, Rename ? scheme_ : scheme_ + tr(" 복사"), &Ok).trimmed();
            if (!Ok || Name.isEmpty()) return; auto Schemes = draft_["appearance"].toObject()["schemes"].toObject();
            if (Schemes.contains(Name)) { fail(tr("이미 같은 이름의 구성표가 있습니다."), 0, schemes_); return; }
            Schemes[Name] = schemeRecord(); if (Rename) { Schemes.remove(scheme_); if (draft_["appearance"].toObject()["selected"] == scheme_) set(draft_, {"appearance", "selected"}, Name); }
            set(draft_, {"appearance", "schemes"}, Schemes); refreshSchemes(Name);
        };
        connect(Clone, &QPushButton::clicked, this, [NameScheme] { NameScheme(false); }); connect(renameScheme_, &QPushButton::clicked, this, [NameScheme] { NameScheme(true); });
        connect(removeScheme_, &QPushButton::clicked, this, [this] { auto Schemes = draft_["appearance"].toObject()["schemes"].toObject(); Schemes.remove(scheme_); if (draft_["appearance"].toObject()["selected"] == scheme_) set(draft_, {"appearance", "selected"}, QStringLiteral("기본 라이트")); set(draft_, {"appearance", "schemes"}, Schemes); refreshSchemes(draft_["appearance"].toObject()["selected"].toString()); });
        Rows->addStretch();
    }
    void createDocuments() {
        auto* Rows = tab(tr("키보드·문서"));
        builtInKeys_ = new QCheckBox(tr("제품 기본 단축키 사용")); builtInKeys_->setObjectName("hc.legacy.builtInShortcuts"); Rows->addWidget(builtInKeys_);
        note(Rows, tr("끄면 사용자 지정 단축키만 사용합니다. 명시적으로 저장한 키는 유지하며 기본 키 초기화와 별개의 설정입니다."));
        auto* Form = new QFormLayout; Rows->addLayout(Form);
        const auto Choice = [this](const QString& Id) { auto* Combo = new QComboBox; Combo->setEditable(true); Combo->setObjectName("hc.legacy." + Id); Combo->addItems({"builtin.hyperviewer", "builtin.hypereditor", "builtin.rhwp-editor"}); return Combo; };
        view_ = Choice("defaultView"); edit_ = Choice("defaultEdit"); Form->addRow(tr("F3 기본 후보"), view_); Form->addRow(tr("F4 기본 후보"), edit_);
        note(Rows, tr("사용 가능한 처리기 ID를 입력합니다. 기본 후보는 메뉴의 우선 항목이며 복수 후보의 자동 실행을 뜻하지 않습니다."));
        note(Rows, tr("확장자별 기본 후보 · 점 없이 입력합니다. 보기·편집 ID를 비우면 전역 기본값을 따릅니다."));
        extensionTable_ = table(Rows, "extensions", {tr("확장자"), tr("보기 ID"), tr("편집 ID")});
        note(Rows, tr("처리기 재정의 · 사용/보기/편집의 중간 상태는 ‘처리기 기본값 따름’입니다. 지원 형식이나 승인을 추가하지는 않습니다."));
        handlerTable_ = table(Rows, "handlers", {tr("처리기 ID"), tr("사용"), tr("보기"), tr("편집")});
        policy_ = new QComboBox; policy_->setObjectName("hc.legacy.selectionPolicy"); policy_->setEditable(true); policy_->addItem("askWhenMultiple");
        auto* PolicyForm = new QFormLayout; PolicyForm->addRow(tr("선택 정책 · 현재 미사용"), policy_); Rows->addLayout(PolicyForm);
        note(Rows, tr("선택 정책은 저장·비교할 수 있으나 현재 실행에서는 읽지 않습니다. 후보가 하나면 열고 여러 개면 선택 메뉴를 표시합니다."));
    }
    void createRules() {
        auto* Rows = tab(tr("기존 규칙"));
        note(Rows, tr("구형 파일 집합 · 목록 조건은 세미콜론으로 구분합니다. 속성·확장자 조건은 호환 경로에서 적용하며 이름 패턴·크기 조건은 완전하게 구현되어 있지 않습니다."));
        note(Rows, tr("조건 플래그: 확장자 0x2 · 속성 0x100. 이름 패턴 0x1과 크기 비교 0x1000/0x2000/0x4000은 현재 미구현입니다. 속성 값은 Windows 파일 속성 비트입니다."));
        fileSets_ = table(Rows, "fileSets", {tr("이름"), tr("플래그"), tr("이름 패턴"), tr("확장자"), tr("속성"), tr("크기")});
        note(Rows, tr("집합별 색상 · 색상·구성표 탭에서 조회 중인 구성표에 저장합니다. 새 파일 그룹 규칙이 일치하면 새 규칙이 우선합니다."));
        groupColors_ = table(Rows, "groupColors", {tr("집합 이름"), tr("글자 · #RRGGBB"), tr("바탕 · #RRGGBB")});
        note(Rows, tr("기존 WLX 감지식 · 기존 실행 방식에서 사용합니다. 별도 프로세스는 플러그인 고유 감지와 플러그인 관리의 추가 조건을 사용합니다."));
        wlx_ = table(Rows, "wlxDetect", {tr("플러그인 경로"), tr("기존 감지식")}, false);
    }
    void createPreserved() {
        auto* Rows = tab(tr("보존 설정"));
        mode_ = new QComboBox; mode_->setObjectName("hc.legacy.themeMode");
        mode_->addItem(tr("구성표 모드 따름 · 기존 호환"), ""); mode_->addItem(tr("라이트"), "light"); mode_->addItem(tr("다크"), "dark"); mode_->addItem(tr("시스템"), "system");
        auto* Form = new QFormLayout; Form->addRow(tr("테마 모드"), mode_); Rows->addLayout(Form);
        note(Rows, tr("공용 테마의 보존 값은 JSON 객체로 편집합니다. Standard·Navy는 현재 메인창에서 선택하지 않는 변형입니다."));
        const auto AddJson = [this, Rows](const QString& Caption, const QStringList& Path, const QString& Id) {
            Rows->addWidget(new QLabel(Caption)); auto* Editor = new QPlainTextEdit; Editor->setObjectName("hc.legacy." + Id); Editor->setAccessibleName(Caption); Editor->setMinimumHeight(140); Rows->addWidget(Editor); json_.append({Caption, Path, Editor});
        };
        AddJson(tr("Standard 보존 값"), {"appearance", "watercolorTheme", "designs", "standard"}, "standard");
        AddJson(tr("Navy 보존 값"), {"appearance", "watercolorTheme", "designs", "watercolor", "navy"}, "navy");
        AddJson(tr("테마 프리셋 보존 값"), {"appearance", "watercolorPresets"}, "presets");
        note(Rows, tr("내부 저장값 조회 · 형식 버전·식별자·검사·승인 정보와 자동 저장 상태는 원본 확인용입니다. 각 기능의 화면에서 변경합니다."));
        inspector_ = new QPlainTextEdit; inspector_->setObjectName("hc.legacy.inspector"); inspector_->setReadOnly(true); inspector_->setMinimumHeight(180); Rows->addWidget(inspector_);
    }
};
}
