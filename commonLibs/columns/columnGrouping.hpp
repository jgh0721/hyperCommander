#pragma once
#include "columnContracts.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonObject>
#include <QTimeZone>
#include <optional>

namespace hc::columns
{
    struct GroupingState
    {
        bool enabled_ = false;
        QUuid columnId_;
        QString mode_ = "value";
        Qt::SortOrder order_ = Qt::AscendingOrder;
        bool operator==(const GroupingState&) const = default;
    };
    struct GroupValue { QString key_, text_; QVariant order_; };
    inline QString GroupText(const char* Text) { return QCoreApplication::translate("ColumnGrouping", Text); }
    inline bool IsGroupingDate(const QString& Field) { return Field == "file.modified" || Field == "file.created"; }
    inline bool SupportsGrouping(const ColumnDefinition& Column)
    {
        return Column.template_.isEmpty() && (IsGroupingDate(Column.fieldId_) || Column.fieldId_ == "file.extension"
            || Column.fieldId_ == "file.type" || Column.fieldId_ == "file.parentPath");
    }
    inline bool SupportsGroupingMode(const QString& Field, const QString& Mode)
    {
        return IsGroupingDate(Field) ? (Mode == "relativeDate" || Mode == "date" || Mode == "month") : Mode == "value";
    }
    inline QJsonObject EncodeGrouping(const GroupingState& State)
    {
        return {{"version", 1}, {"enabled", State.enabled_}, {"columnId", State.columnId_.toString(QUuid::WithoutBraces)},
            {"mode", State.mode_}, {"order", State.order_ == Qt::AscendingOrder ? "ascending" : "descending"}, {"folderPolicy", "withinGroup"}};
    }
    inline std::optional<GroupingState> DecodeGrouping(const QJsonObject& Value)
    {
        if (Value.isEmpty()) return GroupingState{};
        const auto Mode = Value["mode"].toString(), Order = Value["order"].toString();
        if (Value["version"] != QJsonValue(1) || !Value["enabled"].isBool()
            || !Value["columnId"].isString() || (Value["enabled"].toBool() && QUuid(Value["columnId"].toString()).isNull())
            || !QStringList{"value", "relativeDate", "date", "month"}.contains(Mode)
            || (Order != "ascending" && Order != "descending") || Value["folderPolicy"] != QJsonValue("withinGroup")) return {};
        return GroupingState{Value["enabled"].toBool(), QUuid(Value["columnId"].toString()), Mode,
            Order == "ascending" ? Qt::AscendingOrder : Qt::DescendingOrder};
    }
    // Snapshot-only calculation: no provider calls, formatting strings or paint-time I/O.
    inline GroupValue MakeGroupValue(const QString& Field, const QString& Mode, const CellValue& Value,
        QDate Today = QDate::currentDate(), const QTimeZone& Zone = QTimeZone::systemTimeZone())
    {
        const auto Missing = [&](const QString& Key, const QString& Text) { return GroupValue{Mode + "/" + Key, Text, {}}; };
        if (Field == "file.extension" && Value.State() == ValueState::Empty)
            return {Field + "/", GroupText("확장자 없음"), QString{}};
        if (Value.State() != ValueState::Ready) {
            if (Value.State() == ValueState::Error) return Missing("error", GroupText("조회 오류"));
            if (Value.State() == ValueState::Unsupported) return Missing("unsupported", GroupText("지원 안 함"));
            return Missing("missing", IsGroupingDate(Field) ? GroupText("날짜 없음") : GroupText("값 미확인"));
        }
        if (IsGroupingDate(Field)) {
            const auto Time = Value.Raw().toDateTime();
            if (!Time.isValid()) return Missing("missing", GroupText("날짜 없음"));
            const auto Date = Time.toTimeZone(Zone).date();
            if (Mode == "date") return {"date/" + Date.toString(Qt::ISODate), Date.toString(Qt::ISODate), Date.toJulianDay()};
            if (Mode == "month") return {"month/" + Date.toString("yyyy-MM"), Date.toString("yyyy-MM"), Date.year() * 12 + Date.month()};
            const auto Week = Today.addDays(1 - Today.dayOfWeek());
            const auto Month = QDate(Today.year(), Today.month(), 1);
            QString Key, Text; int Rank;
            if (Date > Today) { Key = "future"; Text = GroupText("미래"); Rank = 7; }
            else if (Date == Today) { Key = "today"; Text = GroupText("오늘"); Rank = 6; }
            else if (Date == Today.addDays(-1)) { Key = "yesterday"; Text = GroupText("어제"); Rank = 5; }
            else if (Date >= Week) { Key = "thisWeek"; Text = GroupText("이번 주"); Rank = 4; }
            else if (Date >= Week.addDays(-7)) { Key = "lastWeek"; Text = GroupText("지난주"); Rank = 3; }
            else if (Date >= Month) { Key = "thisMonth"; Text = GroupText("이번 달"); Rank = 2; }
            else if (Date >= Month.addMonths(-1)) { Key = "lastMonth"; Text = GroupText("지난달"); Rank = 1; }
            else { Key = "older"; Text = GroupText("이전"); Rank = 0; }
            return {"relativeDate/" + Key, Text, Rank};
        }
        auto Text = Value.Raw().toString();
        if (Field == "file.parentPath") Text = QDir::cleanPath(QDir::fromNativeSeparators(Text));
        auto Key = Text.toCaseFolded();
        if (Field == "file.extension") {
            Text = Text.isEmpty() ? GroupText("확장자 없음") : Text.toUpper();
        } else if (Text.isEmpty()) Text = GroupText("값 없음");
        return {Field + "/" + Key, Text, Key};
    }
}
