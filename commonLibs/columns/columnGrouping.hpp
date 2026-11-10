#pragma once
#include "columnContracts.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimeZone>
#include <optional>
#include <limits>

namespace hc::columns
{
    inline constexpr int GroupKeyRole = Qt::UserRole + 70, GroupOrderRole = Qt::UserRole + 71, GroupTextRole = Qt::UserRole + 72;
    struct GroupValue { QString key_, text_; QVariant order_; };
    inline QString GroupText(const char* Text) { return QCoreApplication::translate("ColumnGrouping", Text); }
    inline bool IsGroupingDate(const QString& Field) { return Field == "file.modified" || Field == "file.created"; }
    inline bool SupportsGrouping(const ColumnDefinition& Column)
    {
        return Column.template_.isEmpty() && (IsGroupingDate(Column.fieldId_) || Column.fieldId_ == "file.extension"
            || Column.fieldId_ == "file.type" || Column.fieldId_ == "file.parentPath" || Column.fieldId_ == "file.relativePath");
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
    inline QJsonValue EncodeProfileGrouping(const ProfileGrouping& State)
    {
        if (!State.raw_.isUndefined()) return State.raw_;
        QJsonArray Levels;
        for (const auto& Level : State.levels_) Levels.append(QJsonObject{{"columnId", IdText(Level.columnId_)},
            {"mode", Level.mode_}, {"order", Level.order_ == Qt::AscendingOrder ? "ascending" : "descending"}});
        return QJsonObject{{"version", 1}, {"levels", Levels}, {"folderPolicy", "withinGroup"},
            {"expand", State.expand_ == ProfileGrouping::Expand::All ? "all" : State.expand_ == ProfileGrouping::Expand::None ? "none" : "first"},
            {"title", QJsonObject{{"count", State.count_}, {"sizeTotal", State.sizeTotal_}, {"latest", State.latest_}}},
            {"groupsHeader", State.groupsHeader_}, {"hideGroupedColumn", State.hideGroupedColumn_}};
    }
    inline ProfileGrouping DecodeProfileGrouping(const QJsonValue& Value)
    {
        ProfileGrouping Result;
        const auto Invalid = [&] { ProfileGrouping Raw; Raw.raw_ = Value; return Raw; };
        const auto Object = Value.toObject();
        const auto Expand = Object["expand"].toString();
        const auto Title = Object["title"].toObject();
        if (!Value.isObject() || Object["version"] != QJsonValue(1) || !Object["levels"].isArray()
            || Object["folderPolicy"] != QJsonValue("withinGroup") || !QStringList{"all", "none", "first"}.contains(Expand)
            || !Title["count"].isBool() || !Title["sizeTotal"].isBool() || !Title["latest"].isBool()
            || !Object["groupsHeader"].isBool() || !Object["hideGroupedColumn"].isBool()) return Invalid();
        for (const auto& Item : Object["levels"].toArray()) {
            auto Level = Item.toObject(); Level["version"] = 1; Level["enabled"] = true; Level["folderPolicy"] = "withinGroup";
            const auto Decoded = DecodeGrouping(Level);
            if (!Item.isObject() || !Decoded) return Invalid();
            Result.levels_.append(*Decoded);
        }
        Result.expand_ = Expand == "all" ? ProfileGrouping::Expand::All : Expand == "none" ? ProfileGrouping::Expand::None : ProfileGrouping::Expand::First;
        Result.count_ = Title["count"].toBool(); Result.sizeTotal_ = Title["sizeTotal"].toBool(); Result.latest_ = Title["latest"].toBool();
        Result.groupsHeader_ = Object["groupsHeader"].toBool(); Result.hideGroupedColumn_ = Object["hideGroupedColumn"].toBool();
        return Result;
    }
    inline QString ValidateGrouping(const ColumnProfile& Profile)
    {
        if (!Profile.grouping_) return {};
        const auto& State = *Profile.grouping_;
        if (!State.raw_.isUndefined()) return GroupText("지원하지 않는 그룹 설정 원문을 보존하고 있습니다.");
        if (State.levels_.size() > 3) return GroupText("그룹 기준은 최대 3개입니다.");
        QSet<QUuid> Seen;
        for (const auto& Level : State.levels_) {
            const auto It = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [&](const auto& Column) { return Column.id_ == Level.columnId_; });
            if (!Level.enabled_ || Seen.contains(Level.columnId_) || It == Profile.columns_.end() || !SupportsGrouping(*It)
                || !SupportsGroupingMode(It->fieldId_, Level.mode_)) return GroupText("그룹 기준 열과 방식을 확인하세요.");
            Seen.insert(Level.columnId_);
        }
        return {};
    }
    inline GroupingState DefaultGrouping(const ColumnProfile& Profile)
    {
        return Profile.grouping_ && ValidateGrouping(Profile).isEmpty() && !Profile.grouping_->levels_.isEmpty()
            ? Profile.grouping_->levels_.front() : GroupingState{};
    }
    inline QVector<GroupingState> DefaultGroupingLevels(const ColumnProfile& Profile)
    {
        return Profile.grouping_ && ValidateGrouping(Profile).isEmpty() ? Profile.grouping_->levels_ : QVector<GroupingState>{};
    }
    inline QJsonArray EncodeGroupingLevels(const QVector<GroupingState>& Levels)
    {
        QJsonArray Result; for (const auto& Level : Levels) Result.append(EncodeGrouping(Level)); return Result;
    }
    inline std::optional<QVector<GroupingState>> DecodeGroupingLevels(const QJsonValue& Value)
    {
        if (!Value.isArray() || Value.toArray().size() > 3) return {};
        QVector<GroupingState> Result; QSet<QUuid> Seen;
        for (const auto& Item : Value.toArray()) {
            const auto Level = DecodeGrouping(Item.toObject());
            if (!Item.isObject() || !Level || !Level->enabled_ || Seen.contains(Level->columnId_)) return {};
            Seen.insert(Level->columnId_); Result.append(*Level);
        }
        return Result;
    }
    inline constexpr int GroupSizeRole = Qt::UserRole + 80, GroupLatestRole = Qt::UserRole + 81, GroupEntryRole = Qt::UserRole + 82;
    inline QString GroupPathKey(const QStringList& Parts)
    {
        if (Parts.size() == 1) return Parts.front(); // Keep existing one-level expansion keys.
        QStringList Escaped;
        for (auto Part : Parts) { Part.replace('%', "%25"); Part.replace('|', "%7C"); Escaped.append(Part); }
        return Escaped.join('|');
    }
    struct GroupSummary
    {
        quint64 count_ = 0, size_ = 0, unknown_ = 0;
        bool overflow_ = false;
        QDateTime latest_;
        void Add(const QVariant& Size, const QDateTime& Date) {
            ++count_;
            if (!Size.isValid() || Size.isNull()) ++unknown_;
            else { const auto Bytes = Size.toULongLong(); if (Bytes > std::numeric_limits<quint64>::max() - size_) overflow_ = true; else size_ += Bytes; }
            if (Date.isValid() && (!latest_.isValid() || Date > latest_)) latest_ = Date;
        }
        QString Caption(const QString& Text, const ProfileGrouping& Options) const {
            QStringList Parts{Text};
            if (Options.count_) Parts.append(GroupText("%1개").arg(count_));
            if (Options.sizeTotal_) {
                FormatOptions FormatOptions; FormatOptions.size_ = SizeFormat::Adaptive; FormatOptions.precision_ = 1;
                Parts.append(overflow_ ? GroupText("크기 합계 범위 초과") : GroupText("크기 %1").arg(Format(CellValue::Ready(size_), FormatOptions)));
                if (unknown_) Parts.append(GroupText("크기 미확인 %1개").arg(unknown_));
            }
            if (Options.latest_) Parts.append(latest_.isValid() ? GroupText("최근 %1").arg(latest_.toLocalTime().toString("yyyy-MM-dd HH:mm")) : GroupText("최근 날짜 없음"));
            return Parts.join(" · ");
        }
    };
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
            if (Date > Today) { Key = "future"; Text = GroupText("미래"); Rank = 8; }
            else if (Date == Today) { Key = "today"; Text = GroupText("오늘"); Rank = 7; }
            else if (Date == Today.addDays(-1)) { Key = "yesterday"; Text = GroupText("어제"); Rank = 6; }
            else if (Date >= Week) { Key = "thisWeek"; Text = GroupText("이번 주"); Rank = 5; }
            else if (Date >= Week.addDays(-7)) { Key = "lastWeek"; Text = GroupText("지난주"); Rank = 4; }
            else if (Date >= Month) { Key = "thisMonth"; Text = GroupText("이번 달"); Rank = 3; }
            else if (Date >= Month.addMonths(-1)) { Key = "lastMonth"; Text = GroupText("지난달"); Rank = 2; }
            else if (Date >= QDate(Today.year(), 1, 1)) { Key = "thisYear"; Text = GroupText("올해"); Rank = 1; }
            else { Key = "older"; Text = GroupText("오래 전"); Rank = 0; }
            return {"relativeDate/" + Key, Text, Rank};
        }
        auto Text = Value.Raw().toString();
        if (Field == "file.parentPath") Text = QDir::cleanPath(QDir::fromNativeSeparators(Text));
        auto Key = Text.toCaseFolded();
        if (Field == "file.extension") {
            Text = Text.isEmpty() ? GroupText("확장자 없음") : Text.toUpper();
        } else if (Field == "file.relativePath" && Text.isEmpty()) {
            Text = GroupText("기준 폴더");
        } else if (Text.isEmpty()) Text = GroupText("값 없음");
        return {Field + "/" + Key, Text, Key};
    }
}
