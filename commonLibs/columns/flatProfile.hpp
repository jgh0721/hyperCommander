#pragma once
#include "columnProfileStorage.hpp"
#include "builtInColumns.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <functional>
#include <optional>

// 평면 폴더 세트: 평면 보기에서만 쓰는 표시 방식과 열 구성.
// 일반 열 세트(ColumnsV2/Profiles)와 따로 저장한다(ColumnsV2/FlatProfiles · ColumnsV2/FlatSelection).
// 이전 버전의 설정 화면은 일반 키만 다시 쓰므로 평면 키가 사라지지 않는다.
namespace hc::columns
{
    inline constexpr auto FlatProfilesKey = "ColumnsV2/FlatProfiles";
    inline constexpr auto FlatSelectionKey = "ColumnsV2/FlatSelection";
    inline constexpr auto FlatMigration = "flat-sets-1";

    // 단계형 표시 설정. 상위 값이 하위 항목의 사용 여부와 고정 값을 정한다(NormalizeFlatPresentation).
    struct FlatPresentation
    {
        enum class View { List, Thumbnails };
        enum class Structure { None, Folder, Column, Tree };
        enum class Location { Prefix, Column, SecondLine };
        enum class SecondRight { Modified, Size, None };
        View view_ = View::List;
        Structure structure_ = Structure::None;
        // 묶음 없음 · 열 값 그룹에서 상대 경로를 보이는 자리.
        Location location_ = Location::Prefix;
        int lines_ = 1;
        // 이름 앞: 경로를 흐리게(또는 이름과 같은 색), 이름 정렬을 경로 깊이 → 이름(또는 파일 이름만)으로.
        bool prefixDim_ = true, prefixSortDepth_ = true;
        // 별도 열: 이름 다음(또는 마지막).
        bool columnLast_ = false;
        // 둘째 줄: 오른쪽 값.
        SecondRight secondRight_ = SecondRight::Modified;
        // 폴더별 그룹: 제목을 마지막 폴더 이름으로, 순서를 최근 수정 먼저로.
        bool groupTitleLeaf_ = false, groupOrderRecent_ = false;
        // 열 값 그룹: 그룹 기준 필드 · 방식 · 순서.
        QString groupField_ = "file.modified";
        QString groupMode_ = "relativeDate";
        Qt::SortOrder groupOrder_ = Qt::DescendingOrder;
        // 그룹 · 구획 공통: 처음 펼침과 제목 정보.
        ProfileGrouping::Expand expand_ = ProfileGrouping::Expand::All;
        bool titleCount_ = true, titleSize_ = false, titleLatest_ = false;
        // 트리: 폴더 행에 하위 합계, 처음 펼침(-1 모두 · 1 첫 단계 · 0 모두 접기), 트리 선.
        bool treeFolderTotal_ = true, treeLines_ = true;
        int treeExpandLevel_ = -1;
        // 썸네일: 타일 크기, 묶음 없음일 때 타일 아래 폴더 칩.
        int tileSize_ = 96;
        bool tileFolderChip_ = true;
        bool operator==(const FlatPresentation&) const = default;
    };
    struct FlatProfile
    {
        QUuid id_ = QUuid::createUuid();
        QString title_;
        // Origin: 평면 보기를 시작한 탭의 일반 세트 열을 쓴다. Self: columns_.
        enum class ColumnsFrom { Origin, Self } columnsFrom_ = ColumnsFrom::Origin;
        ColumnProfile columns_;
        FlatPresentation presentation_;
        // 이 버전이 모르는 키(다음 버전의 설정)를 다시 저장할 때 되돌린다.
        QJsonObject raw_;
        bool operator==(const FlatProfile&) const = default;
    };

    inline const QUuid FlatBuiltInProfileId{"3c6f2a9e-5d1b-4f0e-9b7a-2e8c4d6f1a35"};
    inline bool IsBuiltInFlatProfile(const QUuid& Id) { return Id == FlatBuiltInProfileId; }
    inline bool IsFlatGroupingField(const QString& Field)
    {
        return Field == "file.modified" || Field == "file.created" || Field == "file.extension" || Field == "file.type";
    }
    inline const QStringList& FlatTileSizes() { static const QStringList Sizes{"64", "96", "160", "256"}; return Sizes; }

    // 단계 규칙을 한 곳에 둔다. 편집기 · 적용 · 판정이 모두 이 결과를 쓴다.
    inline FlatPresentation NormalizeFlatPresentation(FlatPresentation Value)
    {
        using P = FlatPresentation;
        Value.lines_ = Value.lines_ == 2 ? 2 : 1;
        if (!FlatTileSizes().contains(QString::number(Value.tileSize_))) Value.tileSize_ = 96;
        if (Value.treeExpandLevel_ != 0 && Value.treeExpandLevel_ != 1) Value.treeExpandLevel_ = -1;
        if (!IsFlatGroupingField(Value.groupField_)) Value.groupField_ = "file.modified";
        if (!SupportsGroupingMode(Value.groupField_, Value.groupMode_)) Value.groupMode_ = IsGroupingDate(Value.groupField_) ? "relativeDate" : "value";
        if (Value.view_ == P::View::Thumbnails) {
            // 썸네일은 묶음 없음 · 폴더 구획만 있다. 경로 위치 · 줄은 쓰지 않는다.
            if (Value.structure_ != P::Structure::Folder) Value.structure_ = P::Structure::None;
            Value.location_ = P::Location::Prefix; Value.lines_ = 1;
            return Value;
        }
        switch (Value.structure_) {
        case P::Structure::Tree: Value.lines_ = 1; Value.location_ = P::Location::Prefix; break;   // 트리 계층이 경로다.
        case P::Structure::Folder: Value.location_ = P::Location::Prefix; break;                     // 그룹 제목이 경로다.
        case P::Structure::None: case P::Structure::Column:
            if (Value.location_ == P::Location::SecondLine) Value.lines_ = 2;
            break;
        }
        return Value;
    }
    // 그 단계에서 의미가 있는 항목인지(편집기의 행 표시와 저장 · 비교에 쓴다).
    struct FlatPresentationRows
    {
        bool structure_ = true, location_ = false, prefix_ = false, column_ = false, secondLine_ = false, lines_ = false, linesFixed_ = false,
            folder_ = false, columnGroup_ = false, groupTitle_ = false, tree_ = false, tile_ = false, tileChip_ = false;
    };
    inline FlatPresentationRows FlatRows(const FlatPresentation& Value)
    {
        using P = FlatPresentation;
        FlatPresentationRows Rows;
        if (Value.view_ == P::View::Thumbnails) {
            Rows.tile_ = true; Rows.tileChip_ = Value.structure_ == P::Structure::None;
            Rows.groupTitle_ = Value.structure_ == P::Structure::Folder;
            return Rows;
        }
        Rows.lines_ = true;
        Rows.linesFixed_ = Value.structure_ == P::Structure::Tree || ((Value.structure_ == P::Structure::None || Value.structure_ == P::Structure::Column) && Value.location_ == P::Location::SecondLine);
        Rows.location_ = Value.structure_ == P::Structure::None || Value.structure_ == P::Structure::Column;
        Rows.prefix_ = Rows.location_ && Value.location_ == P::Location::Prefix;
        Rows.column_ = Rows.location_ && Value.location_ == P::Location::Column;
        Rows.secondLine_ = Rows.location_ && Value.location_ == P::Location::SecondLine;
        Rows.folder_ = Value.structure_ == P::Structure::Folder;
        Rows.columnGroup_ = Value.structure_ == P::Structure::Column;
        Rows.groupTitle_ = Rows.folder_ || Rows.columnGroup_;
        Rows.tree_ = Value.structure_ == P::Structure::Tree;
        return Rows;
    }

    inline FlatProfile FlatDefaultProfile()
    {
        FlatProfile Profile; Profile.id_ = FlatBuiltInProfileId;
        Profile.title_ = QCoreApplication::translate("FlatView", "경로 접두");
        return Profile;
    }

    namespace detail
    {
        template<class E> QString FlatName(E Value, const QStringList& Names) { return Names.value(int(Value)); }
        template<class E> std::optional<E> FlatParse(const QJsonValue& Value, const QStringList& Names)
        {
            const auto Index = Value.isString() ? Names.indexOf(Value.toString()) : -1;
            return Index < 0 ? std::optional<E>{} : std::optional<E>(static_cast<E>(Index));
        }
        inline const QStringList ViewNames{"list", "thumbnails"};
        inline const QStringList StructureNames{"none", "folder", "column", "tree"};
        inline const QStringList LocationNames{"prefix", "column", "secondLine"};
        inline const QStringList SecondRightNames{"modified", "size", "none"};
        inline const QStringList ExpandNames{"all", "none", "first"};
    }

    inline QJsonObject EncodeFlatPresentation(const FlatPresentation& Source, QJsonObject Result = {})
    {
        using namespace detail;
        const auto Value = NormalizeFlatPresentation(Source);
        Result["view"] = FlatName(Value.view_, ViewNames);
        Result["structure"] = FlatName(Value.structure_, StructureNames);
        Result["location"] = FlatName(Value.location_, LocationNames);
        Result["lines"] = Value.lines_;
        Result["prefixDim"] = Value.prefixDim_; Result["prefixSortDepth"] = Value.prefixSortDepth_;
        Result["columnLast"] = Value.columnLast_;
        Result["secondRight"] = FlatName(Value.secondRight_, SecondRightNames);
        Result["groupTitleLeaf"] = Value.groupTitleLeaf_; Result["groupOrderRecent"] = Value.groupOrderRecent_;
        Result["group"] = QJsonObject{{"field", Value.groupField_}, {"mode", Value.groupMode_}, {"order", Value.groupOrder_ == Qt::AscendingOrder ? "ascending" : "descending"}};
        Result["expand"] = FlatName(Value.expand_, ExpandNames);
        Result["title"] = QJsonObject{{"count", Value.titleCount_}, {"size", Value.titleSize_}, {"latest", Value.titleLatest_}};
        Result["tree"] = QJsonObject{{"folderTotal", Value.treeFolderTotal_}, {"expandLevel", Value.treeExpandLevel_}, {"lines", Value.treeLines_}};
        Result["tile"] = QJsonObject{{"size", Value.tileSize_}, {"folderChip", Value.tileFolderChip_}};
        return Result;
    }
    // 형식이 틀린 항목은 기본값으로 둔다. 표시 설정 하나가 어긋났다고 세트 전체를 버리지 않는다.
    inline FlatPresentation DecodeFlatPresentation(const QJsonObject& Object)
    {
        using namespace detail;
        FlatPresentation Value;
        if (const auto Parsed = FlatParse<FlatPresentation::View>(Object["view"], ViewNames)) Value.view_ = *Parsed;
        if (const auto Parsed = FlatParse<FlatPresentation::Structure>(Object["structure"], StructureNames)) Value.structure_ = *Parsed;
        if (const auto Parsed = FlatParse<FlatPresentation::Location>(Object["location"], LocationNames)) Value.location_ = *Parsed;
        if (const auto Parsed = FlatParse<FlatPresentation::SecondRight>(Object["secondRight"], SecondRightNames)) Value.secondRight_ = *Parsed;
        if (const auto Parsed = FlatParse<ProfileGrouping::Expand>(Object["expand"], ExpandNames)) Value.expand_ = *Parsed;
        Value.lines_ = Object["lines"].toInt(Value.lines_);
        Value.prefixDim_ = Object["prefixDim"].toBool(Value.prefixDim_); Value.prefixSortDepth_ = Object["prefixSortDepth"].toBool(Value.prefixSortDepth_);
        Value.columnLast_ = Object["columnLast"].toBool(Value.columnLast_);
        Value.groupTitleLeaf_ = Object["groupTitleLeaf"].toBool(Value.groupTitleLeaf_); Value.groupOrderRecent_ = Object["groupOrderRecent"].toBool(Value.groupOrderRecent_);
        const auto Group = Object["group"].toObject();
        Value.groupField_ = Group["field"].toString(Value.groupField_); Value.groupMode_ = Group["mode"].toString(Value.groupMode_);
        if (Group["order"].isString()) Value.groupOrder_ = Group["order"].toString() == "ascending" ? Qt::AscendingOrder : Qt::DescendingOrder;
        const auto Title = Object["title"].toObject();
        Value.titleCount_ = Title["count"].toBool(Value.titleCount_); Value.titleSize_ = Title["size"].toBool(Value.titleSize_); Value.titleLatest_ = Title["latest"].toBool(Value.titleLatest_);
        const auto Tree = Object["tree"].toObject();
        Value.treeFolderTotal_ = Tree["folderTotal"].toBool(Value.treeFolderTotal_); Value.treeExpandLevel_ = Tree["expandLevel"].toInt(Value.treeExpandLevel_);
        Value.treeLines_ = Tree["lines"].toBool(Value.treeLines_);
        const auto Tile = Object["tile"].toObject();
        Value.tileSize_ = Tile["size"].toInt(Value.tileSize_); Value.tileFolderChip_ = Tile["folderChip"].toBool(Value.tileFolderChip_);
        return NormalizeFlatPresentation(Value);
    }
    inline QString ValidateFlatProfile(const FlatProfile& Profile)
    {
        if (Profile.id_.isNull()) return QCoreApplication::translate("FlatView", "평면 폴더 세트 식별자가 없습니다.");
        if (Profile.title_.trimmed().isEmpty()) return QCoreApplication::translate("FlatView", "평면 폴더 세트 이름을 입력하세요.");
        if (Profile.columnsFrom_ == FlatProfile::ColumnsFrom::Self) {
            if (const auto Error = Validate(Profile.columns_); !Error.isEmpty()) return Error;
            if (!IsSupportedProfile(Profile.columns_)) return QCoreApplication::translate("FlatView", "지원하지 않는 열이 있습니다.");
        }
        return {};
    }
    inline QJsonObject EncodeFlatProfile(const FlatProfile& Profile)
    {
        if (!ValidateFlatProfile(Profile).isEmpty()) return {};
        auto Result = Profile.raw_;
        Result["version"] = 1; Result["id"] = IdText(Profile.id_); Result["title"] = Profile.title_;
        Result["columnsFrom"] = Profile.columnsFrom_ == FlatProfile::ColumnsFrom::Self ? "self" : "origin";
        if (Profile.columnsFrom_ == FlatProfile::ColumnsFrom::Self) Result["columns"] = EncodeProfile(Profile.columns_);
        else Result.remove("columns");
        Result["presentation"] = EncodeFlatPresentation(Profile.presentation_, Profile.raw_["presentation"].toObject());
        return Result;
    }
    inline std::optional<FlatProfile> DecodeFlatProfile(const QJsonObject& Object)
    {
        if (Object["version"] != QJsonValue(1) || !Object["title"].isString() || QUuid(Object["id"].toString()).isNull()) return {};
        FlatProfile Profile; Profile.raw_ = Object;
        Profile.id_ = QUuid(Object["id"].toString()); Profile.title_ = Object["title"].toString();
        Profile.columnsFrom_ = Object["columnsFrom"].toString() == "self" ? FlatProfile::ColumnsFrom::Self : FlatProfile::ColumnsFrom::Origin;
        if (Profile.columnsFrom_ == FlatProfile::ColumnsFrom::Self) {
            const auto Columns = DecodeProfile(Object["columns"].toObject());
            if (!Columns) return {};
            Profile.columns_ = *Columns;
        }
        Profile.presentation_ = DecodeFlatPresentation(Object["presentation"].toObject());
        if (!ValidateFlatProfile(Profile).isEmpty()) return {};
        return Profile;
    }
    // 저장된 라이브러리에 기본 제공 세트를 더한다(삭제되었거나 아직 이관되지 않은 경우).
    inline QJsonObject WithBuiltInFlatProfiles(QJsonObject Library)
    {
        const auto Id = IdText(FlatBuiltInProfileId);
        if (!Library.contains(Id) || !DecodeFlatProfile(Library[Id].toObject())) Library[Id] = EncodeFlatProfile(FlatDefaultProfile());
        return Library;
    }
    inline QJsonObject DefaultFlatSelection()
    {
        return {{"version", 1}, {"enabled", true}, {"defaultProfile", IdText(FlatBuiltInProfileId)}, {"rules", QJsonArray{}}};
    }
    inline QString ValidateFlatSelection(const QJsonObject& Selection, const QJsonObject& Library)
    {
        if (Selection["version"] != QJsonValue(1) || !Selection["enabled"].isBool() || !Selection["rules"].isArray() || !Selection["defaultProfile"].isString())
            return QCoreApplication::translate("FlatView", "평면 보기 적용 조건의 형식을 확인하세요.");
        if (Selection["rules"].toArray().size() > 128) return QCoreApplication::translate("FlatView", "평면 보기 적용 조건은 최대 128개입니다.");
        for (const auto& Value : Selection["rules"].toArray()) {
            const auto Rule = Value.toObject();
            if (!Value.isObject() || !Rule["enabled"].isBool() || !Rule["path"].isString() || !Rule["profile"].isString())
                return QCoreApplication::translate("FlatView", "평면 보기 적용 조건의 형식을 확인하세요.");
            if (!Rule["enabled"].toBool()) continue;
            const auto Path = Rule["path"].toString().trimmed();
            if (Path.isEmpty()) return QCoreApplication::translate("FlatView", "기준 폴더 조건을 입력하세요.");
            if (Path.size() > 4096 || Path.contains(QChar::Null)) return QCoreApplication::translate("FlatView", "기준 폴더 조건이 너무 길거나 올바르지 않습니다.");
            if (!DecodeFlatProfile(WithBuiltInFlatProfiles(Library)[Rule["profile"].toString()].toObject()))
                return QCoreApplication::translate("FlatView", "적용 조건에서 사용할 평면 폴더 세트를 선택하세요.");
        }
        return {};
    }
    // 첫 실행에 기본 제공 세트와 적용 조건을 한 번 넣는다. 반복 호출해도 결과가 같다.
    inline bool MigrateFlatColumnSets(QJsonObject& Columns)
    {
        auto Migrations = Columns.value("ColumnsV2/Migrations").toArray();
        if (Migrations.contains(FlatMigration)) return false;
        Columns[FlatProfilesKey] = WithBuiltInFlatProfiles(Columns.value(FlatProfilesKey).toObject());
        if (!Columns.value(FlatSelectionKey).isObject()) Columns[FlatSelectionKey] = DefaultFlatSelection();
        Migrations.append(FlatMigration); Columns["ColumnsV2/Migrations"] = Migrations;
        return true;
    }

    // 평면 세트를 탭에 적용할 실효 열 세트와 보기 설정으로 합성한다.
    struct FlatEffective
    {
        ColumnProfile columns_;
        FlatPresentation presentation_;
        bool prefix_ = false, prefixDim_ = true, depthSort_ = false;
        bool thumbnails_ = false, tree_ = false, sections_ = false;
        bool groupTitleLeaf_ = false, groupOrderRecent_ = false;
    };
    inline ColumnDefinition FlatRelativeColumn(const QUuid& Id = QUuid("{6e1d6c33-3b1a-4c4c-8f6e-0a9f4e2d7b51}"))
    {
        ColumnDefinition Column; Column.id_ = Id; Column.fieldId_ = "file.relativePath";
        Column.title_ = QCoreApplication::translate("FlatView", "상대 경로");
        for (auto& Width : Column.widths_) { Width.unit_ = hc::view::WidthUnit::Percent; Width.mode_ = hc::view::WidthMode::Flexible; Width.value_ = 34; Width.hidden_ = false; }
        return Column;
    }
    inline int FlatNameColumn(const ColumnProfile& Profile)
    {
        for (int Index = 0; Index < Profile.columns_.size(); ++Index) if (Profile.columns_[Index].id_ == Profile.primaryNameId_) return Index;
        for (int Index = 0; Index < Profile.columns_.size(); ++Index) if (Profile.columns_[Index].fieldId_ == "file.name") return Index;
        return 0;
    }
    inline int FlatFieldColumn(const ColumnProfile& Profile, const QString& Field)
    {
        for (int Index = 0; Index < Profile.columns_.size(); ++Index)
            if (Profile.columns_[Index].fieldId_ == Field && Profile.columns_[Index].template_.isEmpty()) return Index;
        return -1;
    }
    inline FlatEffective ComposeFlatProfile(const FlatProfile& Flat, const ColumnProfile& Origin)
    {
        using P = FlatPresentation;
        FlatEffective Result; Result.presentation_ = NormalizeFlatPresentation(Flat.presentation_);
        const auto& Value = Result.presentation_;
        auto Columns = Flat.columnsFrom_ == FlatProfile::ColumnsFrom::Self ? Flat.columns_ : Origin;
        Columns.id_ = Flat.id_; Columns.title_ = Flat.title_;
        Columns.grouping_.reset();
        const auto ShowColumn = [&](int Index) { for (auto& Width : Columns.columns_[Index].widths_) Width.hidden_ = false; };
        const auto EnsureRelative = [&](int Position) {
            auto Found = FlatFieldColumn(Columns, "file.relativePath");
            if (Found < 0) { Columns.columns_.insert(std::clamp(Position, 0, int(Columns.columns_.size())), FlatRelativeColumn()); Found = FlatFieldColumn(Columns, "file.relativePath"); }
            else if (Found != Position && Position >= 0 && Position < Columns.columns_.size()) { const auto Column = Columns.columns_.takeAt(Found); Columns.columns_.insert(std::clamp(Position, 0, int(Columns.columns_.size())), Column); Found = FlatFieldColumn(Columns, "file.relativePath"); }
            return Found;
        };
        const auto GroupOn = [&](int ColumnIndex, const QString& Mode, Qt::SortOrder Order, bool HideColumn) {
            ProfileGrouping Grouping; Grouping.levels_ = {{true, Columns.columns_[ColumnIndex].id_, Mode, Order}};
            Grouping.expand_ = Value.expand_; Grouping.count_ = Value.titleCount_; Grouping.sizeTotal_ = Value.titleSize_; Grouping.latest_ = Value.titleLatest_;
            Grouping.hideGroupedColumn_ = HideColumn;
            Columns.grouping_ = Grouping;
        };
        if (Value.view_ == P::View::Thumbnails) {
            Result.thumbnails_ = true;
            Result.sections_ = Value.structure_ == P::Structure::Folder;
            Columns.mode_ = LayoutMode::Details;
            if (Result.sections_) GroupOn(EnsureRelative(int(Columns.columns_.size())), "value", Qt::AscendingOrder, true);
            Result.columns_ = Columns; return Result;
        }
        const int Name = FlatNameColumn(Columns);
        // 묶음 없음 · 열 값 그룹: 경로 위치.
        if (Value.structure_ == P::Structure::None || Value.structure_ == P::Structure::Column) {
            if (Value.location_ == P::Location::Prefix) {
                Result.prefix_ = true; Result.prefixDim_ = Value.prefixDim_; Result.depthSort_ = Value.prefixSortDepth_;
            } else if (Value.location_ == P::Location::Column) {
                ShowColumn(EnsureRelative(Value.columnLast_ ? int(Columns.columns_.size()) : Name + 1));
            } else {
                // 둘째 줄: 이름 아래 줄에 상대 경로, 그 오른쪽에 고른 값.
                ShowColumn(EnsureRelative(Name + 1));
                const auto Right = Value.secondRight_ == P::SecondRight::Modified ? QStringLiteral("file.modified")
                    : Value.secondRight_ == P::SecondRight::Size ? QStringLiteral("file.size") : QString{};
                for (int Index = 0; Index < Columns.columns_.size(); ++Index) {
                    const auto& Field = Columns.columns_[Index].fieldId_;
                    if (Index == FlatNameColumn(Columns) || Field == "file.relativePath") continue;
                    // 둘째 줄에는 상대 경로와 고른 값만 둔다. 한 줄 배치 너비는 그대로 둔다.
                    for (int Mode = 1; Mode < 3; ++Mode) Columns.columns_[Index].widths_[Mode].hidden_ = Field != Right;
                }
                if (!Right.isEmpty() && FlatFieldColumn(Columns, Right) >= 0) {
                    const auto Index = FlatFieldColumn(Columns, Right);
                    for (int Mode = 1; Mode < 3; ++Mode) Columns.columns_[Index].widths_[Mode].hidden_ = false;
                }
            }
        }
        if (Value.structure_ == P::Structure::Folder) {
            GroupOn(EnsureRelative(int(Columns.columns_.size())), "value", Qt::AscendingOrder, true);
            Result.groupTitleLeaf_ = Value.groupTitleLeaf_; Result.groupOrderRecent_ = Value.groupOrderRecent_;
        } else if (Value.structure_ == P::Structure::Column) {
            auto Index = FlatFieldColumn(Columns, Value.groupField_);
            bool Hide = false;
            if (Index < 0) {
                // 같은 필드면 같은 식별자: 탭의 그룹 펼침 기억이 세트를 다시 적용해도 이어진다.
                ColumnDefinition Column; Column.id_ = QUuid::createUuidV5(FlatBuiltInProfileId, Value.groupField_); Column.fieldId_ = Value.groupField_;
                for (const auto& Field : BuiltInFields()) if (Field.id_ == Value.groupField_) Column.title_ = Field.title_;
                for (auto& Width : Column.widths_) { Width.unit_ = hc::view::WidthUnit::Percent; Width.value_ = 20; Width.hidden_ = true; }
                Columns.columns_.append(Column); Index = int(Columns.columns_.size()) - 1; Hide = true;
            }
            GroupOn(Index, Value.groupMode_, Value.groupOrder_, Hide);
        } else if (Value.structure_ == P::Structure::Tree) {
            Result.tree_ = true;
        }
        Columns.mode_ = Value.lines_ == 2 ? LayoutMode::NameFirst : LayoutMode::Details;
        Result.columns_ = Columns;
        return Result;
    }
    // 기준 폴더로 평면 세트를 고른다. 조건이 맞지 않으면 기본 세트, 그것도 없으면 기본 제공 세트다.
    inline FlatProfile SelectFlatProfile(const QJsonObject& Selection, const QJsonObject& Library, const QString& BasePath,
        const std::function<bool(const QString& Pattern, const QString& Path)>& Matches, int* MatchedRule = nullptr)
    {
        if (MatchedRule) *MatchedRule = -1;
        const auto Profiles = WithBuiltInFlatProfiles(Library);
        const auto Decode = [&](const QString& Id) { return DecodeFlatProfile(Profiles[Id].toObject()); };
        if (Selection["version"] == QJsonValue(1) && Selection["enabled"].toBool(true)) {
            const auto Rules = Selection["rules"].toArray();
            for (int Index = 0; Index < Rules.size(); ++Index) {
                const auto Rule = Rules[Index].toObject();
                if (!Rule["enabled"].toBool() || Rule["path"].toString().trimmed().isEmpty()) continue;
                if (!Matches || !Matches(Rule["path"].toString().trimmed(), BasePath)) continue;
                if (const auto Profile = Decode(Rule["profile"].toString())) { if (MatchedRule) *MatchedRule = Index; return *Profile; }
            }
        }
        if (const auto Profile = Decode(Selection["defaultProfile"].toString())) return *Profile;
        return *Decode(IdText(FlatBuiltInProfileId));
    }
}
