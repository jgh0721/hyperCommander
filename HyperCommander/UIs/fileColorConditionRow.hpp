#pragma once
#include <QtWidgets>
#include "UniqueLibs/settings/fileColorRules.hpp"
#include "UniqueLibs/qtFieldPicker.hpp"
#include "commonLibs/columns/builtInColumns.hpp"

namespace hc::ui {
// A row keeps its original JSON until edited. Opening the settings dialog or
// changing a group's name therefore never rewrites v1 patterns or metadata.
// The item picker lists built-in condition types and column fields in one categorized menu
// (design 3): a field that duplicates a built-in type (name, extension, size, ...) is stored as
// that type, so its fast path and file format stay as before.
class FileColorConditionRow final : public QObject
{
public:
    FileColorConditionRow(QTableWidget* Table, const QJsonObject& Record, int Version,
        QVector<hc::columns::FieldDefinition> Catalog = hc::columns::BuiltInFields())
        : QObject(Table), table_(Table), original_(Record), draft_(settings::FileColorRules::NormalizeCondition(Record, Version)), version_(Version), catalog_(std::move(Catalog)) {
        const int Row = Table->rowCount(); Table->insertRow(Row);
        auto* Anchor = new QTableWidgetItem; Anchor->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); Table->setItem(Row, 0, Anchor);
        field_ = new hc::columns::FieldPicker(Table); field_->setObjectName("hc.condition.field"); field_->setAccessibleName(tr("조건 항목"));
        field_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); field_->setMinimumContentsLength(8);
        populateField(); selectField(); Table->setCellWidget(Row, 0, field_);
        op_ = new QComboBox(Table); op_->setObjectName("hc.condition.operator"); op_->setAccessibleName(tr("조건 비교 방법")); Table->setCellWidget(Row, 1, op_);
        auto* Remove = new QPushButton(tr("×"), Table); Remove->setObjectName("hc.condition.remove"); Remove->setAutoDefault(false); Remove->setAccessibleName(tr("이 조건 삭제")); Table->setCellWidget(Row, 3, Remove);
        connect(Remove, &QPushButton::clicked, this, [this] { if (onRemove_) onRemove_(); });
        rebuildOperators(); rebuildValue();
        connect(field_, &QComboBox::currentIndexChanged, this, [this] {
            if (loading_) return;
            const auto Type = field_->currentData().toString();
            if (Type.contains('.')) { setField(Type); rebuildOperators(); rebuildValue(); edited(); return; }
            // Unknown keys of the record survive a type change, as before field conditions existed.
            for (const auto* Key : {"field", "valueType", "fieldTitle", "unit", "syntax"}) draft_.remove(Key);
            draft_["type"] = Type;
            if (Type == "extension") { draft_["op"] = "in"; draft_["value"] = "txt"; }
            else if (Type == "pattern") { draft_["op"] = "matches"; draft_["value"] = QJsonArray{"*.txt"}; }
            else if (Type == "folder") { draft_["op"] = "is"; draft_["value"] = "true"; }
            else if (Type == "attributes") { draft_["op"] = "any"; draft_["value"] = "6"; }
            else if (Type == "size") { draft_["op"] = "ge"; draft_["value"] = "1"; draft_["unit"] = "MiB"; }
            else if (Type == "modified") { draft_["op"] = "within"; draft_["value"] = "24"; draft_["unit"] = "hours"; }
            else if (Type == "normalization") { draft_["op"] = "is"; draft_["value"] = "decomposed"; }
            else if (Type == "rtl") { draft_["op"] = "contains"; draft_.remove("value"); }
            else { draft_["op"] = "is"; draft_["value"] = "any"; }
            rebuildOperators(); rebuildValue(); edited();
        });
        connect(op_, &QComboBox::currentIndexChanged, this, [this] {
            if (loading_) return;
            const auto Old = draft_["op"].toString(), Next = op_->currentData().toString();
            const auto Type = draft_["type"].toString();
            if (Type == "field") { setFieldOperator(Next); rebuildValue(); edited(); return; }
            if (Type == "extension" || Type == "pattern") {
                const auto Before = settings::FileColorRules::Syntax(draft_);
                modeValues_[Type + ':' + Before] = draft_["value"];
                const QString Mode = Next.startsWith("regex") ? "regex" : Next.startsWith("glob") || Next == "matches" || Next == "notMatches" ? "wildcard" : "literal";
                const QString Op = Next.startsWith("regex") || Next.startsWith("glob") ? Next.endsWith("NotMatches") ? "notMatches" : "matches" : Next;
                draft_["syntax"] = Mode; draft_["op"] = Op;
                if (Before != Mode) draft_["value"] = modeValues_.value(Type + ':' + Mode, Mode == "wildcard" ? QJsonValue(QJsonArray{}) : QJsonValue(QString{}));
            } else draft_["op"] = Next;
            if (draft_["type"] == "modified" && (Old == "within") != (Next == "within")) {
                if (Next == "within") { draft_["value"] = "24"; draft_["unit"] = "hours"; }
                else { draft_["value"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); draft_.remove("unit"); }
            }
            rebuildValue(); edited();
        });
    }
    int Row() const { for (int I = 0; I < table_->rowCount(); ++I) if (table_->cellWidget(I, 0) == field_) return I; return -1; }
    QJsonObject Record() const { return edited_ ? draft_ : original_; }
    void Upgrade() { original_ = settings::FileColorRules::NormalizeCondition(original_, version_); version_ = 2; }
    std::function<void()> onEdited_, onRemove_;
private:
    void edited() { edited_ = true; if (onEdited_) onEdited_(); }
    const hc::columns::FieldDefinition* definition(const QString& Id) const {
        for (const auto& Field : catalog_) if (Field.id_ == Id) return &Field;
        return nullptr;
    }
    // Built-in groups in catalog order; overlapping fields become the built-in condition type.
    void populateField() {
        using namespace hc::columns;
        const QMap<QString, QString> Native{{"file.name", "pattern"}, {"file.extension", "extension"}, {"file.isDirectory", "folder"},
            {"file.size", "size"}, {"file.modified", "modified"}, {"file.attributes", "attributes"}};
        const QStringList Titles{tr("확장자"), tr("파일명"), tr("파일/폴더"), tr("속성"), tr("크기"), tr("수정 날짜"), tr("링크 종류"), tr("파일명 정규화"), tr("RTL 문자")};
        const auto AddNative = [&](const QString& Key, const QString& Group) { field_->AddEntry({Titles[keys_.indexOf(Key)], Key, Group, int(ProviderKind::BuiltIn)}); };
        const auto* Name = definition("file.name"); const auto* Link = definition("file.linkTarget");
        const auto NameGroup = Name ? Name->group_ : tr("이름·형식"), LinkGroup = Link ? Link->group_ : tr("링크");
        QSet<QString> Added; QString Previous;
        const auto Close = [&](const QString& Group) {
            if (Group == NameGroup) { AddNative("normalization", Group); AddNative("rtl", Group); Added += {"normalization", "rtl"}; }
        };
        for (const auto& Field : catalog_) {
            if (Field.provider_ == ProviderKind::BuiltIn && Field.group_ != Previous) {
                Close(Previous); Previous = Field.group_;
                if (Field.group_ == LinkGroup) { AddNative("link", LinkGroup); Added.insert("link"); }
            }
            if (Native.contains(Field.id_)) { AddNative(Native[Field.id_], Field.group_); Added.insert(Native[Field.id_]); continue; }
            if (IsConditionField(Field.id_)) field_->AddField(Field);
        }
        Close(Previous);
        for (const auto& Key : keys_) if (!Added.contains(Key)) AddNative(Key, NameGroup);
        // Shown, not hidden, so its absence explains itself (design 13.1).
        field_->AddEntry({tr("계산 필드 (Lua) — 사용할 수 없음"), "lua", {}, -1, {}, false,
            tr("Lua 계산 필드는 열 세트 안에 정의되어 파일 그룹·선택 조건에 쓸 수 없습니다.")});
    }
    void selectField() {
        const bool Field = draft_["type"] == "field";
        const auto Key = Field ? draft_.value("field").toString() : draft_["type"].toString();
        if (Field && field_->findData(Key) < 0) {
            const auto Title = draft_.value("fieldTitle").toString(Key);
            field_->AddEntry({tr("사용할 수 없는 필드 · %1").arg(Title), Key, {}, -1, {}, true, tr("현재 필드 목록에 없습니다. 조건은 보존하며 일치하지 않습니다.")});
        }
        field_->setCurrentIndex(field_->findData(Key));
    }
    static QStringList choiceList(const QJsonValue& Value) { QStringList Result; for (const auto V : Value.toArray()) Result.append(V.toString()); return Result; }
    hc::columns::FieldType fieldType() const { return hc::columns::ParseFieldType(draft_["valueType"].toString()).value_or(hc::columns::FieldType::Text); }
    void setField(const QString& Id) {
        using namespace hc::columns;
        const auto* Field = definition(Id); const auto Type = Field ? Field->type_ : FieldType::Text;
        draft_ = QJsonObject{{"type", "field"}, {"field", Id}, {"valueType", FieldTypeNames()[int(Type)]}};
        if (Field && Field->provider_ != ProviderKind::BuiltIn) draft_["fieldTitle"] = Field->title_;
        const auto Ops = FieldOperators(Type, Id);
        QString Op = Ops.front();
        if (Type == FieldType::Text && Ops.contains("exists") && Id != "file.alternateDataStream.names") Op = "exists";
        if (Field && !Field->choices_.isEmpty() && Ops.contains("in")) Op = "in";
        setFieldOperator(Op);
    }
    // A new operator may need a differently shaped value; keep what still fits.
    void setFieldOperator(const QString& Key) {
        using namespace hc::columns;
        const auto Type = fieldType(); const auto* Field = definition(draft_.value("field").toString());
        const bool Numeric = Type == FieldType::Integer || Type == FieldType::UnsignedInteger || Type == FieldType::Real;
        const QJsonValue Previous = draft_.value("value");
        QString Op = Key;
        draft_.remove("syntax");
        if (Key.startsWith("glob") || Key.startsWith("regex")) {
            Op = Key.endsWith("NotMatches") ? "notMatches" : "matches"; draft_["syntax"] = Key.startsWith("regex") ? "regex" : "wildcard";
        }
        draft_["op"] = Op;
        if (Op != "within" && !(Numeric && Field && Field->unit_ == FieldUnit::Bytes)) draft_.remove("unit");
        if (Op == "exists" || Op == "notExists") { draft_.remove("value"); return; }
        if (Op == "in" || Op == "notIn") {
            if (!Previous.isArray()) draft_["value"] = Field && !Field->choices_.isEmpty() ? QJsonArray{Field->choices_.front().value_} : QJsonArray{};
            return;
        }
        if (Op == "between") { draft_["value"] = QJsonArray{Previous.isString() ? Previous.toString() : QStringLiteral("1"), QStringLiteral("10")}; if (Field && Field->unit_ == FieldUnit::Bytes && !draft_.contains("unit")) draft_["unit"] = "MiB"; return; }
        if (Op == "is") { draft_["value"] = Previous == QJsonValue("false") ? "false" : "true"; return; }
        if (Op == "within") { draft_["value"] = "7"; draft_["unit"] = "days"; return; }
        if (Op == "before" || Op == "after") {
            draft_["value"] = Type == FieldType::Time ? QStringLiteral("12:00:00") : QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); return;
        }
        if (Op == "matches" || Op == "notMatches") {
            if (draft_.value("syntax") == "wildcard") draft_["value"] = Previous.isArray() ? Previous : QJsonValue(QJsonArray{Previous.isString() && !Previous.toString().isEmpty() ? Previous.toString() : QStringLiteral("*")});
            else draft_["value"] = Previous.isString() ? Previous : QJsonValue(choiceList(Previous).value(0));
            return;
        }
        if (Numeric) {
            draft_["value"] = Previous.isString() ? Previous : QJsonValue("1");
            if (Field && Field->unit_ == FieldUnit::Bytes && !draft_.contains("unit")) draft_["unit"] = "MiB";
            return;
        }
        draft_["value"] = Previous.isString() ? Previous : QJsonValue(draft_.value("field") == "file.alternateDataStream.names" ? QStringLiteral("Zone.Identifier") : choiceList(Previous).value(0));
    }
    void rebuildOperators() {
        loading_ = true; op_->clear(); const auto Type = draft_["type"].toString();
        const auto Add = [this](const QString& Text, const char* Key) { op_->addItem(Text, Key); };
        if (Type == "field") {
            const QMap<QString, QString> Labels{{"ge", tr("이상 (≥)")}, {"gt", tr("초과 (>)")}, {"le", tr("이하 (≤)")}, {"lt", tr("미만 (<)")},
                {"eq", tr("같음 (=)")}, {"ne", tr("다름 (≠)")}, {"between", tr("범위")}, {"in", tr("다음 중 하나")}, {"notIn", tr("다음 중 아님")},
                {"exists", tr("값 있음")}, {"notExists", tr("값 없음")}, {"contains", tr("포함")}, {"notContains", tr("포함 아님")},
                {"is", tr("같음")}, {"within", tr("최근 기간")}, {"before", tr("이전 (<)")}, {"after", tr("이후 (>)")}};
            for (const auto& Op : hc::columns::FieldOperators(fieldType(), draft_.value("field").toString())) {
                if (Op == "matches") { op_->addItem(tr("와일드카드 일치"), "globMatches"); op_->addItem(tr("정규표현식 일치"), "regexMatches"); }
                else if (Op == "notMatches") { op_->addItem(tr("와일드카드 불일치"), "globNotMatches"); op_->addItem(tr("정규표현식 불일치"), "regexNotMatches"); }
                else op_->addItem(Labels.value(Op, Op), Op);
            }
            auto Key = draft_["op"].toString();
            if (Key == "matches" || Key == "notMatches") Key = (draft_.value("syntax") == "regex" ? QStringLiteral("regex") : QStringLiteral("glob")) + (Key == "matches" ? "Matches" : "NotMatches");
            op_->setCurrentIndex(op_->findData(Key));
        } else {
            if (Type == "extension") { Add(tr("목록 포함"), "in"); Add(tr("목록 포함 아님"), "notIn"); Add(tr("와일드카드 일치"), "globMatches"); Add(tr("와일드카드 불일치"), "globNotMatches"); Add(tr("정규표현식 일치"), "regexMatches"); Add(tr("정규표현식 불일치"), "regexNotMatches"); }
            else if (Type == "pattern") { Add(tr("문자열 포함"), "contains"); Add(tr("문자열 포함 아님"), "notContains"); Add(tr("와일드카드 일치"), "matches"); Add(tr("와일드카드 불일치"), "notMatches"); Add(tr("정규표현식 일치"), "regexMatches"); Add(tr("정규표현식 불일치"), "regexNotMatches"); }
            else if (Type == "attributes") { Add(tr("하나라도"), "any"); Add(tr("모두"), "all"); }
            else if (Type == "size") { Add(tr("이상 (≥)"), "ge"); Add(tr("초과 (>)"), "gt"); Add(tr("이하 (≤)"), "le"); Add(tr("미만 (<)"), "lt"); Add(tr("같음 (=)"), "eq"); Add(tr("다름 (≠)"), "ne"); }
            else if (Type == "modified") { Add(tr("최근 기간"), "within"); Add(tr("이전 (<)"), "before"); Add(tr("이후 (>)"), "after"); }
            else if (Type == "rtl") { Add(tr("포함"), "contains"); Add(tr("비포함"), "notContains"); }
            else Add(tr("같음"), "is");
            auto Key = draft_["op"].toString();
            const auto Mode = settings::FileColorRules::Syntax(draft_);
            if (Type == "extension" || Type == "pattern") {
                if (Mode == "regex") Key = Key == "notMatches" ? "regexNotMatches" : "regexMatches";
                else if (Mode == "wildcard" && Type == "extension") Key = Key == "notMatches" ? "globNotMatches" : "globMatches";
            }
            op_->setCurrentIndex(op_->findData(Key));
        }
        int Width = 0; for (int I = 0; I < op_->count(); ++I) Width = qMax(Width, op_->fontMetrics().horizontalAdvance(op_->itemText(I)));
        table_->setColumnWidth(1, qMax(table_->columnWidth(1), Width + 40)); loading_ = false;
    }
    static QString patternText(const QJsonArray& Patterns) {
        QStringList Parts; for (const auto P : Patterns) { auto Text = P.toString(); Text.replace(";", "\\;"); Parts.append(Text); } return Parts.join("; ");
    }
    static QJsonArray patterns(const QString& Text) {
        QJsonArray Result; QString Part;
        for (int I = 0; I < Text.size(); ++I) {
            if (Text[I] == '\\' && I + 1 < Text.size() && Text[I + 1] == ';') { Part += ';'; ++I; }
            else if (Text[I] == ';') { if (!Part.trimmed().isEmpty()) Result.append(Part.trimmed()); Part.clear(); }
            else Part += Text[I];
        }
        if (!Part.trimmed().isEmpty()) Result.append(Part.trimmed()); return Result;
    }
    QLineEdit* valueEdit(QWidget* Panel, QHBoxLayout* Fields, const QString& Text, const char* Name = "hc.condition.value") {
        auto* Value = new QLineEdit(Panel); Value->setObjectName(Name); Value->setAccessibleName(tr("조건 값")); Value->setMaxLength(512); Value->setText(Text); Fields->addWidget(Value, 1); return Value;
    }
    void unitChoice(QWidget* Panel, QHBoxLayout* Fields, const QList<QPair<QString, QString>>& Units) {
        auto* Unit = new QComboBox(Panel); Unit->setObjectName("hc.condition.unit"); Unit->setAccessibleName(tr("조건 값의 단위")); Fields->addWidget(Unit);
        for (const auto& [Key, Title] : Units) Unit->addItem(Title, Key);
        Unit->setCurrentIndex(qMax(0, Unit->findData(draft_.value("unit").toString())));
        connect(Unit, &QComboBox::currentIndexChanged, this, [this, Unit] { if (loading_) return; draft_["unit"] = Unit->currentData().toString(); edited(); });
    }
    void rebuildFieldValue(QWidget* Panel, QHBoxLayout* Fields) {
        using namespace hc::columns;
        const auto Type = fieldType(); const auto Op = draft_["op"].toString(); const auto* Field = definition(draft_.value("field").toString());
        const auto Unit = Field ? Field->unit_ : FieldUnit::None;
        const bool Numeric = Type == FieldType::Integer || Type == FieldType::UnsignedInteger || Type == FieldType::Real;
        const auto UnitLabel = [&] {
            const QString Text = Unit == FieldUnit::Pixels ? QStringLiteral("px") : Unit == FieldUnit::Count ? tr("개") : Unit == FieldUnit::Characters ? tr("자") : QString{};
            if (!Text.isEmpty()) { auto* Label = new QLabel(Text, Panel); Label->setObjectName("hc.condition.unitLabel"); Fields->addWidget(Label); }
        };
        const QList<QPair<QString, QString>> ByteUnits{{"B", "B"}, {"KiB", "KiB"}, {"MiB", "MiB"}, {"GiB", "GiB"}};
        if (Op == "exists" || Op == "notExists") {
            auto* Label = new QLabel(tr("값 입력 없음"), Panel); Label->setObjectName("hc.condition.value"); Fields->addWidget(Label); return;
        }
        if ((Op == "in" || Op == "notIn") && Field && !Field->choices_.isEmpty()) {
            auto* Pick = new QToolButton(Panel); Pick->setObjectName("hc.condition.choices"); Pick->setAccessibleName(tr("값 선택")); Pick->setPopupMode(QToolButton::InstantPopup); Fields->addWidget(Pick, 1);
            auto* Menu = new QMenu(Pick); Pick->setMenu(Menu); const auto Selected = choiceList(draft_.value("value"));
            const auto Refresh = [this, Pick, Field] {
                QStringList Titles; const auto Values = choiceList(draft_.value("value"));
                for (const auto& Choice : Field->choices_) if (Values.contains(Choice.value_)) Titles.append(Choice.title_);
                Pick->setText(Titles.isEmpty() ? tr("값 선택") : Titles.join(" · "));
            };
            for (const auto& Choice : Field->choices_) {
                auto* Action = Menu->addAction(Choice.title_); Action->setCheckable(true); Action->setChecked(Selected.contains(Choice.value_));
                connect(Action, &QAction::toggled, this, [this, Value = Choice.value_, Field, Refresh](bool On) {
                    auto Values = choiceList(draft_.value("value")); Values.removeAll(Value); if (On) Values.append(Value);
                    QJsonArray Ordered; for (const auto& Choice : Field->choices_) if (Values.contains(Choice.value_)) Ordered.append(Choice.value_);
                    draft_["value"] = Ordered; Refresh(); edited();
                });
            }
            Refresh(); return;
        }
        if (Op == "in" || Op == "notIn") {
            auto* Value = valueEdit(Panel, Fields, choiceList(draft_.value("value")).join("; "));
            Value->setPlaceholderText(tr("값을 ;로 구분")); Value->setMaxLength(4096);
            connect(Value, &QLineEdit::textChanged, this, [this](const QString& Text) {
                if (loading_) return; QJsonArray Items; for (const auto& Part : Text.split(';', Qt::SkipEmptyParts)) if (!Part.trimmed().isEmpty()) Items.append(Part.trimmed());
                draft_["value"] = Items; edited();
            });
            UnitLabel(); return;
        }
        if (Op == "between") {
            const auto Pair = draft_.value("value").toArray();
            auto* Low = valueEdit(Panel, Fields, Pair.at(0).toString());
            auto* Dash = new QLabel(QStringLiteral("~"), Panel); Fields->addWidget(Dash);
            auto* High = valueEdit(Panel, Fields, Pair.at(1).toString(), "hc.condition.valueHigh");
            const auto Store = [this, Low, High] { if (loading_) return; draft_["value"] = QJsonArray{Low->text(), High->text()}; edited(); };
            connect(Low, &QLineEdit::textChanged, this, Store); connect(High, &QLineEdit::textChanged, this, Store);
            if (Unit == FieldUnit::Bytes) unitChoice(Panel, Fields, ByteUnits); else UnitLabel();
            return;
        }
        if (Type == FieldType::Boolean) {
            auto* Choice = new QComboBox(Panel); Choice->setObjectName("hc.condition.value"); Choice->setAccessibleName(tr("조건 값")); Fields->addWidget(Choice);
            Choice->addItem(tr("예"), "true"); Choice->addItem(tr("아니요"), "false"); Choice->setCurrentIndex(qMax(0, Choice->findData(draft_.value("value").toString())));
            connect(Choice, &QComboBox::currentIndexChanged, this, [this, Choice] { if (loading_) return; draft_["value"] = Choice->currentData().toString(); edited(); });
            return;
        }
        if (Op == "within") {
            auto* Value = valueEdit(Panel, Fields, draft_.value("value").toString());
            connect(Value, &QLineEdit::textChanged, this, [this](const QString& Text) { if (loading_) return; draft_["value"] = Text; edited(); });
            unitChoice(Panel, Fields, {{"minutes", tr("분")}, {"hours", tr("시간")}, {"days", tr("일")}}); return;
        }
        if (Type == FieldType::Time) {
            auto* Time = new QTimeEdit(QTime::fromString(draft_.value("value").toString(), Qt::ISODate), Panel); Time->setObjectName("hc.condition.value"); Time->setDisplayFormat("HH:mm:ss"); Fields->addWidget(Time);
            connect(Time, &QTimeEdit::timeChanged, this, [this](const QTime& Value) { if (loading_) return; draft_["value"] = Value.toString(Qt::ISODate); edited(); });
            return;
        }
        if (Type == FieldType::Date || Type == FieldType::Timestamp) {
            auto* Date = new QDateTimeEdit(Panel); Date->setObjectName("hc.condition.value"); Date->setAccessibleName(tr("날짜 기준 · UTC")); Date->setTimeZone(QTimeZone::UTC); Date->setCalendarPopup(true); Date->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
            Date->setMinimumDate(QDate(1, 1, 1)); Date->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            Date->setDateTime(QDateTime::fromString(draft_.value("value").toString(), Qt::ISODateWithMs)); Fields->addWidget(Date);
            connect(Date, &QDateTimeEdit::dateTimeChanged, this, [this, Date] { if (loading_) return; draft_["value"] = Date->dateTime().toUTC().toString(Qt::ISODateWithMs); edited(); });
            return;
        }
        if (Numeric) {
            auto* Value = valueEdit(Panel, Fields, draft_.value("value").toString()); Value->setMaxLength(24);
            connect(Value, &QLineEdit::textChanged, this, [this](const QString& Text) { if (loading_) return; draft_["value"] = Text; edited(); });
            if (Unit == FieldUnit::Bytes) unitChoice(Panel, Fields, ByteUnits); else UnitLabel();
            return;
        }
        // Text.
        const bool List = (Op == "matches" || Op == "notMatches") && draft_.value("syntax") != "regex";
        auto* Value = valueEdit(Panel, Fields, List ? patternText(draft_.value("value").toArray()) : draft_.value("value").toString());
        if (List) { Value->setMaxLength(16446); Value->setPlaceholderText(tr("예: \\\\nas\\*; D:\\Archive\\*")); Value->setToolTip(tr("와일드카드는 전체 값과 비교합니다. * 여러 글자(경로 구분자 포함) · ? 한 글자\n여러 패턴은 ;로 구분하고 값에 포함된 ;는 \\;로 입력합니다.")); }
        else if (draft_.value("syntax") == "regex") Value->setToolTip(tr("정규표현식은 일치하는 부분을 찾습니다. 전체 비교: \\A…\\z. 기본은 대소문자 무시입니다."));
        else Value->setToolTip(tr("입력한 문자열을 문자 그대로 비교합니다. 대소문자를 구분하지 않습니다."));
        if (draft_.value("field") == "file.alternateDataStream.names") Value->setToolTip(tr("ADS 이름을 \", \"로 이어 붙인 문자열에서 찾습니다. 기본 ::$DATA 스트림은 포함하지 않습니다."));
        connect(Value, &QLineEdit::textChanged, this, [this, List](const QString& Text) { if (loading_) return; draft_["value"] = List ? QJsonValue(patterns(Text)) : QJsonValue(Text); edited(); });
    }
    void rebuildValue() {
        loading_ = true; const int CurrentRow = Row(); if (CurrentRow < 0) { loading_ = false; return; }
        auto* Panel = new QWidget(table_); auto* Fields = new QHBoxLayout(Panel); Fields->setContentsMargins(0, 0, 0, 0); Fields->setSpacing(4);
        const auto Type = draft_["type"].toString();
        const auto Mode = settings::FileColorRules::Syntax(draft_);
        const bool List = (Type == "extension" || Type == "pattern") && Mode == "wildcard";
        if (Type == "field") rebuildFieldValue(Panel, Fields);
        else if (Type == "rtl") {
            auto* Label = new QLabel(tr("파일명 전체"), Panel); Label->setObjectName("hc.condition.value"); Fields->addWidget(Label);
            Label->setToolTip(tr("아랍어·히브리어 등 오른쪽에서 왼쪽으로 쓰는 문자와 RLM·RLE·RLO·RLI 등의 방향 제어 문자를 검사합니다."));
        } else if (Type == "folder" || Type == "link" || Type == "normalization") {
            auto* Choice = new QComboBox(Panel); Choice->setObjectName("hc.condition.value"); Choice->setAccessibleName(tr("조건 값")); Choice->setMinimumContentsLength(1); Choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); Fields->addWidget(Choice);
            if (Type == "folder") { Choice->addItem(tr("폴더"), "true"); Choice->addItem(tr("파일"), "false"); }
            else if (Type == "normalization") {
                Choice->addItem(tr("자소 분리 있음 (NFD·혼합)"), "decomposed"); Choice->addItem(tr("분리 없음 (NFC)"), "composed");
                Choice->setToolTip(tr("파일명을 NFC로 조합하면 달라지는 항목을 찾습니다.\nNFD와 NFC/NFD 혼합 이름에 적용되며 파일명은 변경하지 않습니다.\n영문·숫자처럼 조합 전후가 같은 이름은 분리 없음으로 분류합니다."));
            }
            else { Choice->addItem(tr("링크 또는 바로 가기"), "any"); Choice->addItem(tr("바로 가기 (.lnk · .url)"), "shortcut"); Choice->addItem(tr("심볼릭 링크 · 정션"), "symbolic"); Choice->addItem(tr("링크 아님"), "none"); }
            Choice->setCurrentIndex(Choice->findData(draft_.value("value").toString()));
            connect(Choice, &QComboBox::currentIndexChanged, this, [this, Choice] { if (loading_) return; draft_["value"] = Choice->currentData().toString(); edited(); });
        } else if (Type == "modified" && draft_["op"] != "within") {
            auto* Date = new QDateTimeEdit(Panel); Date->setObjectName("hc.condition.value"); Date->setAccessibleName(tr("수정 날짜 기준 · UTC")); Date->setTimeZone(QTimeZone::UTC); Date->setCalendarPopup(true); Date->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
            Date->setMinimumDate(QDate(1, 1, 1)); Date->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            Date->setDateTime(QDateTime::fromString(draft_.value("value").toString(), Qt::ISODateWithMs)); Fields->addWidget(Date);
            connect(Date, &QDateTimeEdit::dateTimeChanged, this, [this, Date] { if (loading_) return; draft_["value"] = Date->dateTime().toUTC().toString(Qt::ISODateWithMs); edited(); });
        } else {
            auto* Value = new QLineEdit(Panel); Value->setObjectName("hc.condition.value"); Value->setAccessibleName(tr("조건 값")); Value->setMaxLength(List ? 16446 : 512); Fields->addWidget(Value, 1);
            Value->setText(List ? patternText(draft_.value("value").toArray()) : draft_.value("value").toString());
            if (List) { Value->setPlaceholderText(Type == "extension" ? tr("예: jp*; h??") : tr("예: report_*.txt; image_??.png")); Value->setToolTip(tr("와일드카드는 전체 문자열과 비교합니다. * 여러 글자 · ? 한 글자\n여러 패턴은 ;로 구분하고 이름에 포함된 ;는 \\;로 입력합니다.")); }
            else if (Mode == "regex" && (Type == "pattern" || Type == "extension")) { Value->setPlaceholderText(Type == "extension" ? "\\A(?:jpe?g|png)\\z" : "\\Areport_\\d{4}\\.txt\\z"); Value->setToolTip(tr("정규표현식은 일치하는 부분을 찾습니다. 전체 비교: \\A…\\z\n;, |, 공백과 역슬래시를 원문 그대로 입력합니다. 기본은 대소문자 무시입니다.")); }
            else if (Type == "pattern") { Value->setPlaceholderText(tr("예: report")); Value->setToolTip(tr("입력한 문자열을 문자 그대로 포함하는지 비교합니다. 대소문자를 구분하지 않습니다.")); }
            else if (Type == "extension") Value->setToolTip(tr("예: txt, cpp, hpp · 확장자가 없으면 <none>"));
            connect(Value, &QLineEdit::textChanged, this, [this, List](const QString& Text) { if (loading_) return; draft_["value"] = List ? QJsonValue(patterns(Text)) : QJsonValue(Text); edited(); });
            if (Type == "size" || Type == "modified") {
                auto* Unit = new QComboBox(Panel); Unit->setObjectName("hc.condition.unit"); Unit->setAccessibleName(tr("조건 값의 단위")); Fields->addWidget(Unit);
                if (Type == "size") for (const auto& Key : {"B", "KiB", "MiB", "GiB"}) Unit->addItem(Key, Key);
                else { Unit->addItem(tr("분"), "minutes"); Unit->addItem(tr("시간"), "hours"); Unit->addItem(tr("일"), "days"); }
                Unit->setCurrentIndex(Unit->findData(draft_.value("unit").toString()));
                connect(Unit, &QComboBox::currentIndexChanged, this, [this, Unit] { if (loading_) return; draft_["unit"] = Unit->currentData().toString(); edited(); });
            } else if (Type == "attributes") {
                auto* Pick = new QToolButton(Panel); Pick->setObjectName("hc.condition.attributes"); Pick->setAccessibleName(tr("파일 속성 선택")); Pick->setPopupMode(QToolButton::InstantPopup); Fields->addWidget(Pick);
                auto* Menu = new QMenu(Pick); Pick->setMenu(Menu);
                const QStringList Names{tr("읽기 전용 (R)"), tr("숨김 (H)"), tr("시스템 (S)"), tr("보관 (A)"), tr("압축 (C)"), tr("암호화 (E)")};
                const QList<quint64> Bits{1, 2, 4, 32, 2048, 16384}; const auto Mask = draft_.value("value").toString().toULongLong();
                const QStringList Codes{"R", "H", "S", "A", "C", "E"}; QStringList Active;
                for (int I = 0; I < Bits.size(); ++I) {
                    auto* Action = Menu->addAction(Names[I]); Action->setCheckable(true); Action->setChecked((Mask & Bits[I]) != 0); if (Action->isChecked()) Active.append(Codes[I]);
                    connect(Action, &QAction::toggled, this, [this, Value, Bit = Bits[I]](bool On) { bool Ok = false; auto Number = Value->text().toULongLong(&Ok); if (!Ok) Number = 0; Number = On ? Number | Bit : Number & ~Bit; Value->setText(QString::number(Number)); });
                }
                Pick->setText(Active.isEmpty() ? tr("속성 선택") : Active.join(" · ")); Pick->setToolTip(Names.join(" · "));
                connect(Value, &QLineEdit::textChanged, this, [Pick, Menu, Bits, Codes](const QString& Text) { const auto Number = Text.toULongLong(); QStringList Active; const auto Actions = Menu->actions();
                    for (int I = 0; I < Bits.size(); ++I) { const QSignalBlocker Block(Actions[I]); Actions[I]->setChecked((Number & Bits[I]) != 0); if (Actions[I]->isChecked()) Active.append(Codes[I]); }
                    Pick->setText(Active.isEmpty() ? tr("속성 선택") : Active.join(" · "));
                });
            }
        }
        table_->setCellWidget(CurrentRow, 2, Panel); table_->setRowHeight(CurrentRow, qMax(table_->verticalHeader()->defaultSectionSize(), Panel->sizeHint().height() + 2)); loading_ = false;
    }
    QTableWidget* table_;
    hc::columns::FieldPicker* field_;
    QComboBox* op_;
    QJsonObject original_, draft_;
    QMap<QString, QJsonValue> modeValues_;
    int version_;
    QVector<hc::columns::FieldDefinition> catalog_;
    bool loading_ = false, edited_ = false;
    const QStringList keys_{"extension", "pattern", "folder", "attributes", "size", "modified", "link", "normalization", "rtl"};
};
}
