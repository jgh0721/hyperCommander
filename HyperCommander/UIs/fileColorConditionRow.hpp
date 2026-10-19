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
        const QStringList Names{tr("확장자"), tr("파일명"), tr("파일/폴더"), tr("속성"), tr("크기"), tr("수정 날짜"), tr("링크"), tr("파일명 정규화")};
        for (int I = 0; I < keys_.size(); ++I) field_->addItem(Names[I], keys_[I]);
        field_->setCurrentIndex(field_->findData(draft_["type"].toString())); Table->setCellWidget(Row, 0, field_);
        op_ = new QComboBox(Table); op_->setObjectName("hc.condition.operator"); op_->setAccessibleName(tr("조건 비교 방법")); Table->setCellWidget(Row, 1, op_);
        auto* Remove = new QPushButton(tr("×"), Table); Remove->setObjectName("hc.condition.remove"); Remove->setAutoDefault(false); Remove->setAccessibleName(tr("이 조건 삭제")); Table->setCellWidget(Row, 3, Remove);
        connect(Remove, &QPushButton::clicked, this, [this] { if (onRemove_) onRemove_(); });
        rebuildOperators(); rebuildValue();
        connect(field_, &QComboBox::currentIndexChanged, this, [this] {
            if (loading_) return;
            const auto Type = field_->currentData().toString(); draft_["type"] = Type; draft_.remove("unit");
            if (Type == "extension") { draft_["op"] = "in"; draft_["value"] = "txt"; }
            else if (Type == "pattern") { draft_["op"] = "matches"; draft_["value"] = QJsonArray{"*.txt"}; }
            else if (Type == "folder") { draft_["op"] = "is"; draft_["value"] = "true"; }
            else if (Type == "attributes") { draft_["op"] = "any"; draft_["value"] = "6"; }
            else if (Type == "size") { draft_["op"] = "ge"; draft_["value"] = "1"; draft_["unit"] = "MiB"; }
            else if (Type == "modified") { draft_["op"] = "within"; draft_["value"] = "24"; draft_["unit"] = "hours"; }
            else if (Type == "normalization") { draft_["op"] = "is"; draft_["value"] = "decomposed"; }
            else { draft_["op"] = "is"; draft_["value"] = "any"; }
            rebuildOperators(); rebuildValue(); edited();
        });
        connect(op_, &QComboBox::currentIndexChanged, this, [this] {
            if (loading_) return;
            const auto Old = draft_["op"].toString(), Next = op_->currentData().toString(); draft_["op"] = Next;
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
        if (Type == "extension") { Add(tr("포함"), "in"); Add(tr("제외"), "notIn"); }
        else if (Type == "pattern") { Add(tr("일치"), "matches"); Add(tr("불일치"), "notMatches"); }
        else if (Type == "attributes") { Add(tr("하나라도"), "any"); Add(tr("모두"), "all"); }
        else if (Type == "size") { Add(tr("이상 (≥)"), "ge"); Add(tr("초과 (>)"), "gt"); Add(tr("이하 (≤)"), "le"); Add(tr("미만 (<)"), "lt"); Add(tr("같음 (=)"), "eq"); Add(tr("다름 (≠)"), "ne"); }
        else if (Type == "modified") { Add(tr("최근 기간"), "within"); Add(tr("이전 (<)"), "before"); Add(tr("이후 (>)"), "after"); }
        else Add(tr("같음"), "is");
        op_->setCurrentIndex(op_->findData(draft_["op"].toString())); loading_ = false;
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
        if (Type == "folder" || Type == "link" || Type == "normalization") {
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
            auto* Value = new QLineEdit(Panel); Value->setObjectName("hc.condition.value"); Value->setAccessibleName(tr("조건 값")); Value->setMaxLength(Type == "pattern" ? 16384 : 512); Fields->addWidget(Value, 1);
            Value->setText(Type == "pattern" ? patternText(draft_["value"].toArray()) : draft_["value"].toString());
            if (Type == "pattern") Value->setToolTip(tr("여러 패턴은 ;로 구분합니다. 예: CMakeLists.txt; *.cmake\n이름에 포함된 ;는 \\;로 입력합니다."));
            else if (Type == "extension") Value->setToolTip(tr("예: txt, cpp, hpp · 확장자가 없으면 <none>"));
            connect(Value, &QLineEdit::textChanged, this, [this, Type](const QString& Text) { if (loading_) return; draft_["value"] = Type == "pattern" ? QJsonValue(patterns(Text)) : QJsonValue(Text); edited(); });
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
    int version_;
    bool loading_ = false, edited_ = false;
    const QStringList keys_{"extension", "pattern", "folder", "attributes", "size", "modified", "link", "normalization"};
};
}
