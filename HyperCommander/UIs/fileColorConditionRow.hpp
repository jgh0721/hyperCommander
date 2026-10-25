#pragma once
#include <QtWidgets>
#include "UniqueLibs/settings/fileColorRules.hpp"

namespace hc::ui {
// A row keeps its original JSON until edited. Opening the settings dialog or
// changing a group's name therefore never rewrites v1 patterns or metadata.
class FileColorConditionRow final : public QObject
{
public:
    FileColorConditionRow(QTableWidget* Table, const QJsonObject& Record, int Version)
        : QObject(Table), table_(Table), original_(Record), draft_(settings::FileColorRules::NormalizeCondition(Record, Version)), version_(Version) {
        const int Row = Table->rowCount(); Table->insertRow(Row);
        auto* Anchor = new QTableWidgetItem; Anchor->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); Table->setItem(Row, 0, Anchor);
        field_ = new QComboBox(Table); field_->setObjectName("hc.condition.field"); field_->setAccessibleName(tr("조건 항목"));
        const QStringList Names{tr("확장자"), tr("파일명"), tr("파일/폴더"), tr("속성"), tr("크기"), tr("수정 날짜"), tr("링크"), tr("파일명 정규화"), tr("RTL 문자")};
        for (int I = 0; I < keys_.size(); ++I) field_->addItem(Names[I], keys_[I]);
        field_->setCurrentIndex(field_->findData(draft_["type"].toString())); Table->setCellWidget(Row, 0, field_);
        op_ = new QComboBox(Table); op_->setObjectName("hc.condition.operator"); op_->setAccessibleName(tr("조건 비교 방법")); Table->setCellWidget(Row, 1, op_);
        auto* Remove = new QPushButton(tr("×"), Table); Remove->setObjectName("hc.condition.remove"); Remove->setAutoDefault(false); Remove->setAccessibleName(tr("이 조건 삭제")); Table->setCellWidget(Row, 3, Remove);
        connect(Remove, &QPushButton::clicked, this, [this] { if (onRemove_) onRemove_(); });
        rebuildOperators(); rebuildValue();
        connect(field_, &QComboBox::currentIndexChanged, this, [this] {
            if (loading_) return;
            const auto Type = field_->currentData().toString(); draft_["type"] = Type; draft_.remove("unit"); draft_.remove("syntax");
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
    void rebuildOperators() {
        loading_ = true; op_->clear(); const auto Type = draft_["type"].toString();
        const auto Add = [this](const QString& Text, const char* Key) { op_->addItem(Text, Key); };
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
    void rebuildValue() {
        loading_ = true; const int CurrentRow = Row(); if (CurrentRow < 0) { loading_ = false; return; }
        auto* Panel = new QWidget(table_); auto* Fields = new QHBoxLayout(Panel); Fields->setContentsMargins(0, 0, 0, 0); Fields->setSpacing(4);
        const auto Type = draft_["type"].toString();
        const auto Mode = settings::FileColorRules::Syntax(draft_);
        const bool List = (Type == "extension" || Type == "pattern") && Mode == "wildcard";
        if (Type == "rtl") {
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
            Choice->setCurrentIndex(Choice->findData(draft_["value"].toString()));
            connect(Choice, &QComboBox::currentIndexChanged, this, [this, Choice] { if (loading_) return; draft_["value"] = Choice->currentData().toString(); edited(); });
        } else if (Type == "modified" && draft_["op"] != "within") {
            auto* Date = new QDateTimeEdit(Panel); Date->setObjectName("hc.condition.value"); Date->setAccessibleName(tr("수정 날짜 기준 · UTC")); Date->setTimeZone(QTimeZone::UTC); Date->setCalendarPopup(true); Date->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
            Date->setMinimumDate(QDate(1, 1, 1)); Date->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            Date->setDateTime(QDateTime::fromString(draft_["value"].toString(), Qt::ISODateWithMs)); Fields->addWidget(Date);
            connect(Date, &QDateTimeEdit::dateTimeChanged, this, [this, Date] { if (loading_) return; draft_["value"] = Date->dateTime().toUTC().toString(Qt::ISODateWithMs); edited(); });
        } else {
            auto* Value = new QLineEdit(Panel); Value->setObjectName("hc.condition.value"); Value->setAccessibleName(tr("조건 값")); Value->setMaxLength(List ? 16446 : 512); Fields->addWidget(Value, 1);
            Value->setText(List ? patternText(draft_["value"].toArray()) : draft_["value"].toString());
            if (List) { Value->setPlaceholderText(Type == "extension" ? tr("예: jp*; h??") : tr("예: report_*.txt; image_??.png")); Value->setToolTip(tr("와일드카드는 전체 문자열과 비교합니다. * 여러 글자 · ? 한 글자\n여러 패턴은 ;로 구분하고 이름에 포함된 ;는 \\;로 입력합니다.")); }
            else if (Mode == "regex" && (Type == "pattern" || Type == "extension")) { Value->setPlaceholderText(Type == "extension" ? "\\A(?:jpe?g|png)\\z" : "\\Areport_\\d{4}\\.txt\\z"); Value->setToolTip(tr("정규표현식은 일치하는 부분을 찾습니다. 전체 비교: \\A…\\z\n;, |, 공백과 역슬래시를 원문 그대로 입력합니다. 기본은 대소문자 무시입니다.")); }
            else if (Type == "pattern") { Value->setPlaceholderText(tr("예: report")); Value->setToolTip(tr("입력한 문자열을 문자 그대로 포함하는지 비교합니다. 대소문자를 구분하지 않습니다.")); }
            else if (Type == "extension") Value->setToolTip(tr("예: txt, cpp, hpp · 확장자가 없으면 <none>"));
            connect(Value, &QLineEdit::textChanged, this, [this, List](const QString& Text) { if (loading_) return; draft_["value"] = List ? QJsonValue(patterns(Text)) : QJsonValue(Text); edited(); });
            if (Type == "size" || Type == "modified") {
                auto* Unit = new QComboBox(Panel); Unit->setObjectName("hc.condition.unit"); Unit->setAccessibleName(tr("조건 값의 단위")); Fields->addWidget(Unit);
                if (Type == "size") for (const auto& Key : {"B", "KiB", "MiB", "GiB"}) Unit->addItem(Key, Key);
                else { Unit->addItem(tr("분"), "minutes"); Unit->addItem(tr("시간"), "hours"); Unit->addItem(tr("일"), "days"); }
                Unit->setCurrentIndex(Unit->findData(draft_["unit"].toString()));
                connect(Unit, &QComboBox::currentIndexChanged, this, [this, Unit] { if (loading_) return; draft_["unit"] = Unit->currentData().toString(); edited(); });
            } else if (Type == "attributes") {
                auto* Pick = new QToolButton(Panel); Pick->setObjectName("hc.condition.attributes"); Pick->setAccessibleName(tr("파일 속성 선택")); Pick->setPopupMode(QToolButton::InstantPopup); Fields->addWidget(Pick);
                auto* Menu = new QMenu(Pick); Pick->setMenu(Menu);
                const QStringList Names{tr("읽기 전용 (R)"), tr("숨김 (H)"), tr("시스템 (S)"), tr("보관 (A)"), tr("압축 (C)"), tr("암호화 (E)")};
                const QList<quint64> Bits{1, 2, 4, 32, 2048, 16384}; const auto Mask = draft_["value"].toString().toULongLong();
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
    QComboBox *field_, *op_;
    QJsonObject original_, draft_;
    QMap<QString, QJsonValue> modeValues_;
    int version_;
    bool loading_ = false, edited_ = false;
    const QStringList keys_{"extension", "pattern", "folder", "attributes", "size", "modified", "link", "normalization", "rtl"};
};
}
