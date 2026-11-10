#include "UniqueLibs/columnWidthSettings.hpp"
#include <QtTest>
#include "commonLibs/columns/luaColumns.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/columns/columnSort.hpp"
#include "commonLibs/columns/columnGrouping.hpp"
#include <atomic>
#include <thread>
#include <limits>
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/columns/fieldCondition.hpp"
#include "commonLibs/columns/columnProfileStorage.hpp"
#include "commonLibs/view/listIconSize.hpp"
#include "UniqueLibs/columnAutoSelection.hpp"
#include "commonLibs/columns/flatProfile.hpp"
using namespace hc::view;

class ColumnTests : public QObject
{
    Q_OBJECT
private slots:
    void folderConditionTokens()
    {
        using namespace hc::shell;
        const QString Guid = "{374DE290-123F-4565-9164-39C4925E467B}";
        for (const auto& Text : {Guid, "shell:" + Guid, " SHELL:" + Guid + " "}) {
            const auto Token = ParseFolderToken(Text);
            QCOMPARE(Token.kind_, FolderToken::Kind::KnownFolder); QCOMPARE(Token.id_, QUuid(Guid));
        }
        QCOMPARE(ParseFolderToken(Guid + "\\*").rest_, QString("/*"));
        QCOMPARE(ParseFolderToken("shell:Personal\\Projects\\*").name_, QString("Personal"));
        QCOMPARE(ParseFolderToken("shell:Personal").kind_, FolderToken::Kind::KnownFolderName);
        QCOMPARE(ParseFolderToken("::" + Guid).kind_, FolderToken::Kind::Namespace);
        QCOMPARE(ParseFolderToken("shell:::" + Guid).kind_, FolderToken::Kind::Namespace);
        for (const auto& Text : {QString("{abc}"), QString("shell:"), Guid + "bad", QString("::{bad}")})
            QCOMPARE(ParseFolderToken(Text).kind_, FolderToken::Kind::Invalid);
        QCOMPARE(ParseFolderToken("D:\\Work\\*").kind_, FolderToken::Kind::None);
    }
    void profileGroupingRoundTrip()
    {
        using namespace hc::columns;
        for (const auto& Profile : {DefaultProfile(), ReferenceProfile(), ReferenceProfile(true)}) {
            const auto Before = EncodeProfile(Profile); QVERIFY(!Before.contains("grouping"));
            QCOMPARE(QJsonDocument(EncodeProfile(*DecodeProfile(Before))).toJson(QJsonDocument::Compact), QJsonDocument(Before).toJson(QJsonDocument::Compact));
        }
        auto Profile = DefaultProfile(); Profile.grouping_ = ProfileGrouping{};
        Profile.grouping_->levels_ = {{true, Profile.columns_[4].id_, "relativeDate", Qt::DescendingOrder}};
        Profile.grouping_->expand_ = ProfileGrouping::Expand::First; Profile.grouping_->hideGroupedColumn_ = true;
        const auto Encoded = EncodeProfile(Profile); const auto Decoded = DecodeProfile(Encoded); QVERIFY(Decoded);
        QCOMPARE(Decoded->grouping_, Profile.grouping_); QVERIFY(ValidateGrouping(*Decoded).isEmpty());
        const QList<QJsonValue> Invalid{QJsonObject{{"version", 99}}, QJsonObject{}, QJsonArray{1, "future"}, QJsonValue("future"), QJsonValue(QJsonValue::Null), QJsonValue(17)};
        for (const auto& Raw : Invalid) {
            auto Object = EncodeProfile(DefaultProfile()); Object["grouping"] = Raw;
            const auto Loaded = DecodeProfile(Object); QVERIFY(Loaded); QVERIFY(Loaded->grouping_);
            QCOMPARE(Loaded->grouping_->raw_, Raw); QVERIFY(Loaded->grouping_->levels_.isEmpty());
            QVERIFY(IsSupportedProfile(*Loaded)); QVERIFY(!ValidateGrouping(*Loaded).isEmpty());
            QVERIFY(!DefaultGrouping(*Loaded).enabled_);
            QCOMPARE(QJsonDocument(EncodeProfile(*Loaded)).toJson(QJsonDocument::Compact), QJsonDocument(Object).toJson(QJsonDocument::Compact));
        }
    }
    void profileGroupingValidation()
    {
        using namespace hc::columns;
        auto Profile = DefaultProfile(); Profile.grouping_ = ProfileGrouping{};
        auto& Levels = Profile.grouping_->levels_;
        Levels = {{true, QUuid::createUuid(), "value"}}; QVERIFY(!ValidateGrouping(Profile).isEmpty());
        Levels = {{true, Profile.columns_[0].id_, "value"}}; QVERIFY(!ValidateGrouping(Profile).isEmpty());
        Levels = {{true, Profile.columns_[4].id_, "value"}}; QVERIFY(!ValidateGrouping(Profile).isEmpty());
        Levels[0].mode_ = "date"; QVERIFY(ValidateGrouping(Profile).isEmpty());
        Profile.columns_[4].template_ = "{file.modified}"; QVERIFY(!ValidateGrouping(Profile).isEmpty()); Profile.columns_[4].template_.clear();
        Levels.append(Levels.front()); QVERIFY(!ValidateGrouping(Profile).isEmpty());
        QVERIFY(IsSupportedProfile(Profile));
    }
    void groupSummaryAndLevels()
    {
        using namespace hc::columns;
        GroupSummary Summary; ProfileGrouping Options;
        const auto Older = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
        const auto Newer = Older.addDays(1);
        Summary.Add(QVariant::fromValue(quint64(1024)), Older); Summary.Add({}, Newer);
        Summary.Add(QVariant::fromValue(quint64(2048)), {});
        QCOMPARE(Summary.count_, quint64(3)); QCOMPARE(Summary.size_, quint64(3072)); QCOMPARE(Summary.unknown_, quint64(1)); QCOMPARE(Summary.latest_, Newer);
        Options.count_ = false; Options.sizeTotal_ = false; Options.latest_ = false;
        QCOMPARE(Summary.Caption("title", Options), QString("title"));
        Options.count_ = true; QVERIFY(Summary.Caption("title", Options).contains("3개"));
        Options.sizeTotal_ = true; QVERIFY(Summary.Caption("title", Options).contains("미확인 1개"));
        Options.latest_ = true; QVERIFY(Summary.Caption("title", Options).contains(Newer.toLocalTime().toString("yyyy-MM-dd HH:mm")));
        Summary.Add(QVariant::fromValue(std::numeric_limits<quint64>::max()), Older); QVERIFY(Summary.overflow_);
        auto Profile = DefaultProfile(); Profile.grouping_ = ProfileGrouping{};
        for (const auto& Definition : Profile.columns_) if (SupportsGrouping(Definition) && Profile.grouping_->levels_.size() < 3)
            Profile.grouping_->levels_.append({true, Definition.id_, IsGroupingDate(Definition.fieldId_) ? "date" : "value"});
        QCOMPARE(Profile.grouping_->levels_.size(), 3); QVERIFY(ValidateGrouping(Profile).isEmpty());
        QCOMPARE(*DecodeGroupingLevels(EncodeGroupingLevels(Profile.grouping_->levels_)), Profile.grouping_->levels_);
        Profile.grouping_->levels_.append(Profile.grouping_->levels_.front()); QVERIFY(!ValidateGrouping(Profile).isEmpty());
        QVERIFY(!DecodeGroupingLevels(EncodeGroupingLevels(Profile.grouping_->levels_)));
        QVERIFY(GroupPathKey({"a|b", "c"}) != GroupPathKey({"a", "b|c"})); QCOMPARE(GroupPathKey({"old/key"}), QString("old/key"));
    }
    void downloadsProfileDefaults()
    {
        using namespace hc::columns;
        const auto Profile = DownloadsProfile(); QVERIFY(Validate(Profile).isEmpty()); QVERIFY(IsSupportedProfile(Profile));
        QVERIFY(ValidateGrouping(Profile).isEmpty()); QVERIFY(IsBuiltInProfile(Profile.id_));
        QCOMPARE(Profile.columns_.size(), 6); QSet<QUuid> Ids;
        for (const auto& Column : Profile.columns_) { QVERIFY(!Ids.contains(Column.id_)); Ids.insert(Column.id_); }
        QCOMPARE(Profile.columns_[5].fieldId_, QString("file.alternateDataStream.zone"));
        for (const auto& Width : Profile.columns_[5].widths_) QVERIFY(Width.hidden_);
        QCOMPARE(Profile.sorts_[0].columnId_, Profile.columns_[1].id_); QCOMPARE(Profile.sorts_[0].order_, Qt::DescendingOrder);
        QCOMPARE(DefaultGrouping(Profile).mode_, QString("relativeDate"));
        QVERIFY(!EmptyAutoSelection()["enabled"].toBool()); QVERIFY(EmptyAutoSelection()["rules"].toArray().isEmpty());
        QJsonObject Columns; QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/Downloads"));
        QVERIFY(ValidateAutoSelection(Columns[AutoSelectionKey].toObject(), WithBuiltInProfiles({}), {}).isEmpty());
    }
    void profileRuleSummary()
    {
        using namespace hc::columns;
        const auto Id = DownloadsProfileId;
        const QVector<hc::shell::KnownFolderChoice> Choices{{"downloads", "사용자 다운로드 폴더", QUuid("{374DE290-123F-4565-9164-39C4925E467B}")}};
        auto Policy = EmptyAutoSelection();
        QCOMPARE(ProfileRuleSummary(Id, Policy, {}, Choices), QString("기본 제공 · 수동"));
        Policy["enabled"] = true;
        QJsonObject Rule{{"enabled", true}, {"profile", IdText(Id)}, {"path", "{374DE290-123F-4565-9164-39C4925E467B}\\*"}, {"group", ""}, {"percent", 50}};
        Policy["rules"] = QJsonArray{Rule};
        QCOMPARE(ProfileRuleSummary(Id, Policy, {}, Choices), QString("기본 제공 · 자동: 사용자 다운로드 폴더"));
        auto Other = Rule; Other["path"] = ""; Other["group"] = "images"; Other["percent"] = 70;
        Policy["rules"] = QJsonArray{Other, Rule};
        const QJsonObject Colors{{"groups", QJsonArray{QJsonObject{{"id", "images"}, {"name", "사진"}}}}};
        QCOMPARE(ProfileRuleSummary(Id, Policy, Colors, Choices), QString("기본 제공 · 자동: 사진 70% 이상 · 외 1개"));
        Other["enabled"] = false; Policy["rules"] = QJsonArray{Other, Rule};
        QVERIFY(ProfileRuleSummary(Id, Policy, Colors, Choices).endsWith("사용자 다운로드 폴더"));
        QCOMPARE(ProfileRuleSummary(QUuid::createUuid(), Policy, Colors, Choices), QString("수동"));
    }
    void downloadsColumnSetMigration()
    {
        using namespace hc::columns;
        const auto Id = IdText(DownloadsProfileId);
        QJsonObject Columns; QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/Downloads"));
        QVERIFY(Columns["ColumnsV2/Profiles"].toObject().contains(Id));
        QVERIFY(Columns[AutoSelectionKey].toObject()["enabled"].toBool());
        QCOMPARE(Columns[AutoSelectionKey].toObject()["rules"].toArray().size(), 1);
        const auto Saved = Columns; QVERIFY(!MigrateDownloadsColumnSet(Columns, "D:/Elsewhere")); QCOMPARE(Columns, Saved);
        auto Policy = EmptyAutoSelection(); Columns = {{AutoSelectionKey, Policy}};
        QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/Downloads")); QVERIFY(Columns[AutoSelectionKey].toObject()["enabled"].toBool());
        QJsonObject User{{"enabled", true}, {"profile", IdText(BuiltInProfileId)}, {"path", "E:/Other/*"}, {"group", ""}, {"percent", 50}};
        for (const bool Matching : {false, true}) {
            User["path"] = Matching ? "D:/Downloads/*" : "E:/Other/*"; Policy["rules"] = QJsonArray{User};
            Columns = {{AutoSelectionKey, Policy}}; QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/Downloads"));
            const auto Result = Columns[AutoSelectionKey].toObject(); QVERIFY(!Result["enabled"].toBool());
            QCOMPARE(Result["rules"].toArray()[Matching ? 0 : 1].toObject(), User);
        }
        User["path"] = "shell:Profile/*"; Policy["rules"] = QJsonArray{User}; Columns = {{AutoSelectionKey, Policy}};
        const auto ProfileToken = hc::shell::ParseFolderToken(User["path"].toString());
        QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/User/Downloads", {{ProfileToken.token_, "D:/User"}}));
        QCOMPARE(Columns[AutoSelectionKey].toObject()["rules"].toArray()[0].toObject(), User);
        for (const bool HasProfile : {false, true}) {
            Columns = {};
            if (HasProfile) Columns["ColumnsV2/Profiles"] = QJsonObject{{Id, EncodeProfile(DownloadsProfile())}};
            else { Policy["rules"] = QJsonArray{QJsonObject{{"profile", Id}}}; Columns[AutoSelectionKey] = Policy; }
            const auto Before = Columns; QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/Downloads"));
            auto WithoutMarker = Columns; WithoutMarker.remove("ColumnsV2/Migrations"); QCOMPARE(WithoutMarker, Before);
        }
        for (const bool Future : {false, true}) {
            Policy = EmptyAutoSelection();
            if (Future) Policy["version"] = 2;
            else { QJsonArray Full; for (int I = 0; I < 128; ++I) Full.append(User); Policy["rules"] = Full; }
            Columns = {{AutoSelectionKey, Policy}}; QVERIFY(MigrateDownloadsColumnSet(Columns, "D:/Downloads"));
            QCOMPARE(Columns[AutoSelectionKey].toObject(), Policy); QVERIFY(!Columns.contains("ColumnsV2/Profiles"));
        }
        Columns = Saved; auto Removed = Columns[AutoSelectionKey].toObject(); Removed["rules"] = QJsonArray{}; Columns[AutoSelectionKey] = Removed;
        QVERIFY(!MigrateDownloadsColumnSet(Columns, "D:/Downloads")); QVERIFY(Columns[AutoSelectionKey].toObject()["rules"].toArray().isEmpty());
    }
    void folderConditionMatching()
    {
        using namespace hc::columns;
        const QString Guid = "{374de290-123f-4565-9164-39c4925e467b}";
        const QHash<QString, QString> Resolved{{Guid, "D:\\user[1]\\Down*loads"}};
        QVERIFY(MatchFolderCondition(Guid, "d:/user[1]/Down*loads/", Resolved));
        QVERIFY(MatchFolderCondition(Guid + "\\*", "D:/user[1]/Down*loads", Resolved));
        QVERIFY(MatchFolderCondition(Guid + "\\*", "D:/user[1]/Down*loads/child/grandchild", Resolved));
        QVERIFY(!MatchFolderCondition(Guid, "D:/user1/Downloads", Resolved));
        QVERIFY(!MatchFolderCondition(Guid, "D:/user[1]/Down*loads"));
        QVERIFY(!MatchFolderCondition(Guid + "\\*", "D:/user[1]/Down*loads-other", Resolved));
        QVERIFY(MatchFolderCondition("D:\\Work\\*", "d:/work"));
        QVERIFY(MatchFolderCondition("D:\\Work\\*", "d:/work/child/grandchild"));
        QVERIFY(!MatchFolderCondition("D:\\Work\\*", "d:/work-other"));
        QVERIFY(MatchFolderCondition("D:/W?rk/*", "d:/Work/child"));
        QVERIFY(MatchFolderCondition("*Work*", "d:/Work/child"));
        QVERIFY(MatchFolderCondition(Guid + "\\*", "D:/", {{Guid, "D:\\"}}));
        QVERIFY(MatchFolderCondition(Guid, "D:/new", {{Guid, "D:/new"}}));
        QVERIFY(!MatchFolderCondition(Guid, "D:/old", {{Guid, "D:/new"}}));
    }
    void autoSelectionShellValidation()
    {
        using namespace hc::columns;
        auto Policy = EmptyAutoSelection(); Policy["enabled"] = true;
        const auto Profile = DefaultProfile();
        const QJsonObject Library{{IdText(Profile.id_), EncodeProfile(Profile)}};
        QJsonObject Rule{{"enabled", true}, {"profile", IdText(Profile.id_)}, {"path", "{abc}"}, {"group", ""}, {"percent", 50}};
        Policy["rules"] = QJsonArray{Rule};
        QVERIFY(ValidateAutoSelection(Policy, Library, {}).contains("GUID"));
        Rule["path"] = "{11111111-1111-1111-1111-111111111111}"; Policy["rules"] = QJsonArray{Rule};
        QVERIFY(ValidateAutoSelection(Policy, Library, {}).isEmpty());
        auto Miss = Rule; Miss["path"] = "D:/Other"; Rule["path"] = "D:/Downloads/*";
        Policy["rules"] = QJsonArray{Miss, Rule}; int MatchedRule = 77;
        const auto Match = SelectAutomaticColumns(Policy, Library, {}, "D:/Downloads", {}, {}, {}, &MatchedRule);
        QVERIFY(Match.HasValue()); QVERIFY(Match.Value()); QCOMPARE(MatchedRule, 1);
        QVERIFY(SelectAutomaticColumns(Policy, Library, {}, "E:/Elsewhere", {}, {}, {}, &MatchedRule).HasValue()); QCOMPARE(MatchedRule, -1);
    }
    void detailsDefaultUsesAutomaticSizeUnits()
    {
        using namespace hc::columns;
        auto Profile = DefaultProfile();
        const auto& Size = Profile.columns_[1];
        QCOMPARE(Size.fieldId_, QString("file.size")); QCOMPARE(Size.format_.size_, SizeFormat::Adaptive);
        QVERIFY(Format(CellValue::Ready(quint64(1024 * 1024)), Size.format_).contains("MiB"));
        Profile.columns_[1].format_.size_ = SizeFormat::Bytes;
        const auto Stored = EncodeProfile(Profile); const auto Loaded = DecodeProfile(Stored); QVERIFY(Loaded);
        QCOMPARE(Loaded->columns_[1].format_.size_, SizeFormat::Bytes); QCOMPARE(EncodeProfile(*Loaded), Stored);
    }
    void fitColumnsToPane()
    {
        const std::vector<ColumnWidthAllocation> Columns{{{460, 200, 1000000}, false},
            {{120, 88, 1000000}, false}, {{190, 190, 1000000}, false}, {{56, 56, 1000000}, false}};
        const auto Wide = FitColumnWidths(Columns, 1200, 0);
        QCOMPARE(Wide[0].width_, 834);
        QCOMPARE(Wide[2].width_, 190);
        QCOMPARE(Wide[3].width_, 56);
        for (int Available : {128, 240, 350, 500, 799, 801, 1200}) {
            const auto Fitted = FitColumnWidths(Columns, Available, 0);
            int Total = 0;
            for (const auto& Width : Fitted) {
                QVERIFY(Width.width_ > 0);
                QVERIFY(Width.minimum_ <= Width.width_ && Width.width_ <= Width.maximum_);
                Total += Width.width_;
            }
            QCOMPARE(Total, Available);
        }
        QCOMPARE(Columns[0].size_.width_, 460);
        const auto Fixed = FitColumnWidths({{{320, 320, 320}, true}, {{320, 320, 320}, true},
            {{320, 320, 320}, true}, {{320, 320, 320}, true}}, 801);
        int Total = 0;
        for (const auto& Width : Fixed) {
            QCOMPARE(Width.minimum_, Width.width_); QCOMPARE(Width.maximum_, Width.width_); Total += Width.width_;
        }
        QCOMPARE(Total, 801);
        const auto Mixed = FitColumnWidths({{{200, 200, 200}, true}, {{900, 100, 1000}, false}}, 500);
        QCOMPARE(Mixed[0].width_, 200); QCOMPARE(Mixed[1].width_, 300);
        const auto Bounded = FitColumnWidths({{{100, 50, 250}, false}, {{80, 80, 80}, true}}, 1000, 0);
        QCOMPARE(Bounded[0].width_, 250);
    }
    void groupingDateBoundariesAndStorage()
    {
        using namespace hc::columns;
        const auto Zone = QTimeZone::utc(); const QDate Today(2026, 3, 2);
        const auto Group = [&](QDate Date) { return MakeGroupValue("file.modified", "relativeDate", CellValue::Ready(QDateTime(Date, QTime(12, 0), Zone)), Today, Zone); };
        QCOMPARE(Group(Today).key_, QString("relativeDate/today"));
        QCOMPARE(Group(Today.addDays(-1)).key_, QString("relativeDate/yesterday"));
        QCOMPARE(Group(QDate(2026, 2, 25)).key_, QString("relativeDate/lastWeek"));
        QCOMPARE(Group(QDate(2026, 2, 15)).key_, QString("relativeDate/lastMonth"));
        QCOMPARE(Group(QDate(2026, 1, 1)).key_, QString("relativeDate/thisYear"));
        QCOMPARE(Group(QDate(2025, 12, 31)).key_, QString("relativeDate/older"));
        QCOMPARE(Group(QDate(2025, 12, 31)).text_, QString::fromUtf8("오래 전"));
        QCOMPARE(Group(Today.addDays(1)).key_, QString("relativeDate/future"));
        QVERIFY(Group(Today).order_.toInt() > Group(Today.addDays(-1)).order_.toInt());
        QVERIFY(!MakeGroupValue("file.modified", "date", CellValue::Missing(ValueState::Empty)).order_.isValid());
        const auto Leap = MakeGroupValue("file.created", "date", CellValue::Ready(QDateTime(QDate(2024, 2, 29), QTime(12, 0), Zone)), Today, Zone);
        QCOMPARE(Leap.key_, QString("date/2024-02-29"));
        const auto Local = MakeGroupValue("file.created", "date", CellValue::Ready(QDateTime(QDate(2026, 3, 1), QTime(16, 0), Zone)), Today, QTimeZone::fromSecondsAheadOfUtc(9 * 3600));
        QCOMPARE(Local.key_, QString("date/2026-03-02"));
        QCOMPARE(MakeGroupValue("file.extension", "value", CellValue::Ready(QString("TXT"))).key_, MakeGroupValue("file.extension", "value", CellValue::Ready(QString("txt"))).key_);
        QVERIFY(MakeGroupValue("file.extension", "value", CellValue::Ready(QString{})).order_.isValid());
        GroupingState State{true, QUuid::createUuid(), "relativeDate", Qt::DescendingOrder};
        QVERIFY(DecodeGrouping(EncodeGrouping(State)) == State);
        auto Future = EncodeGrouping(State); Future["version"] = 2; QVERIFY(!DecodeGrouping(Future));
        Future = EncodeGrouping(State); Future["order"] = "sideways"; QVERIFY(!DecodeGrouping(Future));
        Future = EncodeGrouping(State); Future["enabled"] = "yes"; QVERIFY(!DecodeGrouping(Future));
    }
    void listIconSizeFollowsRecordHeight()
    {
        // 1-line: largest of 16/24/32/48/64 within the record minus 2 px top and bottom.
        QCOMPARE(ListIconSize(10, false), 16);   // never below 16
        QCOMPARE(ListIconSize(21, false), 16);   // watercolor default row
        QCOMPARE(ListIconSize(24, false), 16);   // default row
        QCOMPARE(ListIconSize(28, false), 24);
        QCOMPARE(ListIconSize(36, false), 32);
        QCOMPARE(ListIconSize(52, false), 48);
        QCOMPARE(ListIconSize(68, false), 64);
        QCOMPARE(ListIconSize(200, false), 64);  // 128 is two-line only
        // 2-line: the icon spans both lines and may grow to 128.
        QCOMPARE(ListIconSize(38, true), 32);
        QCOMPARE(ListIconSize(44, true), 32);
        QCOMPARE(ListIconSize(58, true), 48);
        QCOMPARE(ListIconSize(131, true), 64);
        QCOMPARE(ListIconSize(132, true), 128);
        QCOMPARE(ListIconTextGap(false), 6); QCOMPARE(ListIconTextGap(true), 8);
    }
    void referenceProfilesKeepLegacyAndFullNames()
    {
        using namespace hc::columns;
        const auto Legacy = DefaultProfile(); const auto EncodedLegacy = EncodeProfile(Legacy);
        QVERIFY(!Legacy.splitNameExtension_); QVERIFY(!EncodedLegacy.contains("splitNameExtension"));
        QCOMPARE(Legacy.columns_[2].fieldId_, QString("file.created"));
        for (bool TwoLine : {false, true}) {
            auto Profile = ReferenceProfile(TwoLine); QVERIFY2(Validate(Profile).isEmpty(), qPrintable(Validate(Profile)));
            QVERIFY(IsSupportedProfile(Profile)); QVERIFY(Profile.id_ != Legacy.id_);
            QVERIFY(DecodeProfile(EncodeProfile(Profile)).value() == Profile);
            QStringList Visible;
            for (const auto& Column : Profile.columns_) if (!Column.widths_[int(Profile.mode_)].hidden_) Visible.append(Column.fieldId_);
            QCOMPARE(Visible, TwoLine ? (QStringList{"file.name", "file.type", "file.size", "file.modified", "file.attributes"})
                : (QStringList{"file.name", "file.extension", "file.size", "file.modified", "file.attributes"}));
            FileSnapshot File; File.name_ = "report.final.txt";
            QCOMPARE(Resolve(DisplayField(Profile, Profile.columns_.front()), File).Raw().toString(), TwoLine ? QString("report.final.txt") : QString("report.final"));
            QCOMPARE(Resolve(Profile.columns_.front().fieldId_, File).Raw().toString(), QString("report.final.txt"));
            Profile.mode_ = LayoutMode::NameLast; QCOMPARE(DisplayField(Profile, Profile.columns_.front()), QString("file.name"));
            Profile.mode_ = LayoutMode::Details; Profile.splitNameExtension_ = false;
            QCOMPARE(DisplayField(Profile, Profile.columns_.front()), QString("file.name"));
            auto Invalid = EncodeProfile(Profile); Invalid["splitNameExtension"] = "true"; QVERIFY(!DecodeProfile(Invalid));
        }
        QCOMPARE(EncodeProfile(DefaultProfile()), EncodedLegacy);
        QVERIFY(ReferenceProfile().id_ != ReferenceProfile(true).id_);
    }
    void numericDateAndProviderFormats()
    {
        using namespace hc::columns;
        FormatOptions Options; Options.grouping_ = true; Options.plusSign_ = true; Options.precision_ = 3;
        QCOMPARE(Format(CellValue::Ready(quint64(9007199254740993ULL)), Options), QLocale().positiveSign() + QLocale().toString(quint64(9007199254740993ULL)));
        QCOMPARE(Format(CellValue::Ready(-12.5), Options), QLocale().toString(-12.5, 'f', 3));
        const auto Raw = CellValue::Ready(3.25).WithDisplay("plugin text"); QCOMPARE(Format(Raw, Options), QString("plugin text"));
        Options.providerDisplay_ = false; QCOMPARE(Format(Raw, Options), QLocale().positiveSign() + QLocale().toString(3.25, 'f', 3));
        Options.utc_ = true; Options.datePattern_ = "yyyy/MM/dd HH:mm";
        QCOMPARE(Format(CellValue::Ready(QDateTime::fromString("2026-10-02T12:30:00+09:00", Qt::ISODate)), Options), QString("2026/10/02 03:30"));
        auto Profile = DefaultProfile(); Profile.columns_[1].format_ = Options; QVERIFY(DecodeProfile(EncodeProfile(Profile)).value() == Profile);
    }

    void luaColumnsAndDependencyValidation()
    {
        using namespace hc::columns;
        auto Profile = DefaultProfile(); ScriptDefinition Script;
        Script.title_ = "test"; Script.dependencies_ = {"file.size"};
        Script.outputs_ = {{"value", "Value", FieldType::UnsignedInteger}, {"label", "Label", FieldType::Text}};
        Script.source_ = "return function(item) return {value=item['file.size'], label='한글'} end";
        Profile.scripts_.append(Script); const auto Field = ScriptPrefix(Script) + "value"; Profile.columns_[1].fieldId_ = Field;
        QVERIFY(IsSupportedProfile(Profile)); QVERIFY(DecodeProfile(EncodeProfile(Profile)).value() == Profile);
        auto Load = WithLuaColumns({}, Profile); QJsonObject Values{{"file.size", EncodeScriptInput(CellValue::Ready(quint64(UINT64_MAX)))}};
        const auto Job = QString::fromUtf8(QJsonDocument(QJsonObject{{"values", Values}}).toJson(QJsonDocument::Compact));
        const auto Result = Load(Job, QueryFamily(Field), {});
        QCOMPARE(Result[Field].Raw().toULongLong(), quint64(UINT64_MAX)); QCOMPARE(Result[ScriptPrefix(Script) + "label"].Raw().toString(), QString("한글"));
        QCOMPARE(ScriptCell(hc::script::ScriptValue(2.5), FieldType::Integer).State(), ValueState::Error);
        Profile.scripts_[0].dependencies_.append(Field); QVERIFY(!ValidateScripts(Profile).isEmpty());
        Profile.scripts_[0].dependencies_ = {"file.size"}; ScriptDefinition Other = Script; Other.id_ = QUuid::createUuid();
        Profile.scripts_.append(Other); Profile.scripts_[0].dependencies_ = {ScriptPrefix(Other) + "value"}; Profile.scripts_[1].dependencies_ = {Field};
        QVERIFY(!ValidateScripts(Profile).isEmpty());
        Profile.scripts_[1].dependencies_ = {"file.unknown"}; QVERIFY(!IsSupportedProfile(Profile));
        Profile.scripts_[1].dependencies_ = {"file.size"}; QVERIFY(IsSupportedProfile(Profile));
    }

    void templateCompilerAndStorage()
    {
        using namespace hc::columns;
        const auto Template = CompileTemplate("$${literal} ${file.name} · ${file.size} / ${file.size}");
        QVERIFY(Template.Valid()); QCOMPARE(Template.dependencies_, (QStringList{"file.name", "file.size"}));
        FormatOptions Options; Options.size_ = SizeFormat::KiB;
        const auto Value = RenderTemplate(Template, Options, [](const QString& Field) { return Field == "file.name" ? CellValue::Ready(QString("파일.txt")) : CellValue::Ready(quint64(2048)); });
        QCOMPARE(Value.Raw().toString(), QString("${literal} 파일.txt · %1 KiB / %1 KiB").arg(QLocale().toString(2.0, 'f', 2)));
        QCOMPARE(CompileTemplate("abc ${file.name").errorOffset_, 4);
        QVERIFY(!CompileTemplate("${file.name + 1}").Valid()); QVERIFY(!CompileTemplate(QString(4097, 'x')).Valid());
        int Reads = 0;
        QCOMPARE(RenderTemplate(Template, {}, [&](const QString&) { ++Reads; return CellValue::Missing(ValueState::Pending); }).State(), ValueState::Pending);
        QCOMPARE(Reads, 3); // All dependencies can be queued in one pass.
        QCOMPARE(RenderTemplate(CompileTemplate("${file.fileVersion} ${file.imageWidth}"), {}, [](const QString& Field) {
            return CellValue::Missing(Field == "file.fileVersion" ? ValueState::Pending : ValueState::Unsupported);
        }).State(), ValueState::Pending);
        auto Profile = DefaultProfile(); Profile.columns_[1].template_ = "${file.name}: ${file.allocatedSize}"; Profile.columns_[1].sortField_ = "file.allocatedSize";
        QVERIFY(IsSupportedProfile(Profile)); const auto Restored = DecodeProfile(EncodeProfile(Profile)); QVERIFY(Restored); QVERIFY(*Restored == Profile);
        Profile.columns_[1].template_ = "${file.doesNotExist}"; QVERIFY(!IsSupportedProfile(Profile));
        Profile = DefaultProfile(); Profile.columns_[0].template_ = "${file.name}"; QVERIFY(!Validate(Profile).isEmpty());
    }
    void builtInFieldGroupsAndQueryFamilies()
    {
        using namespace hc::columns;
        // Every built-in field belongs to one picker topic, and each topic is contiguous.
        const auto Fields = BuiltInFields(); QSet<QString> Ids, Closed; QString Previous;
        for (const auto& Field : Fields) {
            QVERIFY2(!Field.group_.isEmpty(), qPrintable(Field.id_)); QVERIFY(IsFieldId(Field.id_));
            QVERIFY2(!Ids.contains(Field.id_), qPrintable(Field.id_)); Ids.insert(Field.id_);
            if (Field.group_ != Previous) { QVERIFY2(!Closed.contains(Field.group_), qPrintable(Field.group_)); Closed.insert(Previous); Previous = Field.group_; }
        }
        const QStringList Link{"file.linkTarget", "file.softlink.target", "file.junction.target"};
        const QStringList Streams{"file.alternateDataStream.count", "file.alternateDataStream.names",
            "file.alternateDataStream.totalSize", "file.alternateDataStream.zone"};
        for (const auto& Id : Link + Streams + QStringList{"file.hardlink.count"}) {
            QVERIFY2(Ids.contains(Id), qPrintable(Id)); QVERIFY(!QueryFamily(Id).isEmpty());
            QVERIFY(CompileTemplate("${" + Id + "}").Valid());
        }
        for (const auto& Id : Link) { QCOMPARE(QueryFamily(Id), QString("link")); QVERIFY(QueriesFolders(Id)); }
        // Folders carry no Mark of the Web; the zone is never queried for them.
        for (const auto& Id : Streams) { QCOMPARE(QueryFamily(Id), Id.endsWith(".zone") ? QString("zone") : QString("streams")); QCOMPARE(QueriesFolders(Id), !Id.endsWith(".zone")); }
        // Allocation and link count share one read; a folder has neither.
        QCOMPARE(QueryFamily("file.hardlink.count"), QueryFamily("file.allocatedSize"));
        QVERIFY(!QueriesFolders("file.hardlink.count")); QVERIFY(!QueriesFolders("file.fileVersion")); QVERIFY(!QueriesFolders("file.name"));
        auto Profile = DefaultProfile(); Profile.columns_[1].template_ = "${file.alternateDataStream.names} · ${file.junction.target}";
        Profile.columns_[1].sortField_ = "file.alternateDataStream.zone"; QVERIFY(IsSupportedProfile(Profile));
        // Regrouping the picker must not move the default profile's fields.
        const QStringList Expected{"file.name", "file.size", "file.created", "file.attributes", "file.modified", "file.extension", "file.stem", "file.type"};
        const auto Default = DefaultProfile(); for (int Index = 0; Index < Expected.size(); ++Index) QCOMPARE(Default.columns_[Index].fieldId_, Expected[Index]);
    }
    void fieldConditionCompileAndEvaluate()
    {
        using namespace hc::columns;
        const auto Compile = [](const QJsonObject& Json) { FieldCondition C; const auto Error = CompileFieldCondition(Json, C); return std::make_pair(C, Error); };
        const auto Number = [](const QString& Field, const QString& Type, const QString& Op, const QJsonValue& Value, const QString& Unit = {}) {
            QJsonObject Json{{"type", "field"}, {"field", Field}, {"valueType", Type}, {"op", Op}, {"value", Value}}; if (!Unit.isEmpty()) Json["unit"] = Unit; return Json;
        };
        // Profile- and listing-scoped fields, unknown types and narrowed operators are rejected.
        QVERIFY(Compile(Number("lua.0123456789abcdef0123456789abcdef.area", "integer", "ge", "1")).second.contains("Lua"));
        QVERIFY(!Compile(Number("dopusvfs.0123456789abcdef0123456789abcdef.1", "text", "exists", {})).second.isEmpty());
        QVERIFY(!Compile(Number("file.imageWidth", "pixels", "ge", "1")).second.isEmpty());
        QVERIFY(!Compile(Number("file.alternateDataStream.names", "text", "matches", QJsonArray{"Zone*"})).second.isEmpty());
        QVERIFY(Compile(Number("file.alternateDataStream.names", "text", "contains", "Zone.Identifier")).second.isEmpty());
        QVERIFY(!Compile(Number("file.alternateDataStream.totalSize", "unsigned", "gt", "-1")).second.isEmpty());
        QVERIFY(!Compile(Number("file.alternateDataStream.totalSize", "unsigned", "gt", "18446744073709551615", "KiB")).second.isEmpty());
        QVERIFY(!Compile(Number("file.alternateDataStream.count", "unsigned", "between", QJsonArray{"3", "1"})).second.isEmpty());
        QVERIFY(!Compile(Number("file.junction.target", "text", "matches", "(", {})).second.isEmpty());

        // Units scale the operand; comparisons stay exact across signed/unsigned payloads.
        auto [Size, SizeError] = Compile(Number("file.alternateDataStream.totalSize", "unsigned", "gt", "64", "KiB"));
        QVERIFY2(SizeError.isEmpty(), qPrintable(SizeError));
        QCOMPARE(EvaluateFieldCondition(Size, CellValue::Ready(quint64(65536)), 0), Truth::False);
        QCOMPARE(EvaluateFieldCondition(Size, CellValue::Ready(quint64(65537)), 0), Truth::True);
        QCOMPARE(EvaluateFieldCondition(Size, CellValue::Ready(qint64(-1)), 0), Truth::False);
        QCOMPARE(EvaluateFieldCondition(Size, CellValue::Ready(quint64(UINT64_MAX)), 0), Truth::True);
        auto [Range, RangeError] = Compile(Number("file.alternateDataStream.count", "unsigned", "between", QJsonArray{"1", "3"}));
        QVERIFY(RangeError.isEmpty());
        for (const auto [Count, Expected] : std::initializer_list<std::pair<quint64, Truth>>{{0, Truth::False}, {1, Truth::True}, {3, Truth::True}, {4, Truth::False}})
            QCOMPARE(EvaluateFieldCondition(Range, CellValue::Ready(Count), 0), Expected);
        auto [Zone, ZoneError] = Compile(Number("file.alternateDataStream.zone", "unsigned", "in", QJsonArray{"3", "4"}));
        QVERIFY(ZoneError.isEmpty());
        QCOMPARE(EvaluateFieldCondition(Zone, CellValue::Ready(quint64(3)).WithDisplay("인터넷 (3)"), 0), Truth::True);
        QCOMPARE(EvaluateFieldCondition(Zone, CellValue::Ready(quint64(2)), 0), Truth::False);

        // Design 4.3: missing values never match, even negated; values being read are unknown.
        auto [Not, NotError] = Compile(Number("file.imageWidth", "unsigned", "ne", "100"));
        auto [Has, HasError] = Compile(Number("file.imageWidth", "unsigned", "exists", {}));
        auto [Lacks, LacksError] = Compile(Number("file.imageWidth", "unsigned", "notExists", {}));
        QVERIFY(NotError.isEmpty() && HasError.isEmpty() && LacksError.isEmpty());
        struct Row { CellValue value_; Truth ne_, exists_, notExists_; };
        const QList<Row> Table{{CellValue::Ready(quint64(5)), Truth::True, Truth::True, Truth::False},
            {CellValue::Missing(ValueState::Empty), Truth::False, Truth::False, Truth::True},
            {CellValue::Missing(ValueState::Unsupported), Truth::False, Truth::False, Truth::False},
            {CellValue::Missing(ValueState::Error), Truth::False, Truth::False, Truth::False},
            {CellValue::Missing(ValueState::Pending), Truth::Unknown, Truth::Unknown, Truth::Unknown},
            {CellValue::Missing(ValueState::OnDemand), Truth::Unknown, Truth::Unknown, Truth::Unknown}};
        for (const auto& Item : Table) {
            QCOMPARE(EvaluateFieldCondition(Not, Item.value_, 0), Item.ne_);
            QCOMPARE(EvaluateFieldCondition(Has, Item.value_, 0), Item.exists_);
            QCOMPARE(EvaluateFieldCondition(Lacks, Item.value_, 0), Item.notExists_);
        }

        // Text: case-insensitive substring, anchored wildcard across separators, bounded regex.
        auto [Names, NamesError] = Compile(Number("file.alternateDataStream.names", "text", "notContains", "zone.identifier"));
        QVERIFY(NamesError.isEmpty());
        QCOMPARE(EvaluateFieldCondition(Names, CellValue::Ready(QString("Zone.Identifier, note")), 0), Truth::False);
        QCOMPARE(EvaluateFieldCondition(Names, CellValue::Ready(QString("note")), 0), Truth::True);
        QJsonObject Wild = Number("file.junction.target", "text", "matches", QJsonArray{"\\\\NAS\\*"}); Wild["syntax"] = "wildcard";
        auto [Target, TargetError] = Compile(Wild);
        QVERIFY2(TargetError.isEmpty(), qPrintable(TargetError));
        QCOMPARE(EvaluateFieldCondition(Target, CellValue::Ready(QString("\\\\nas\\share\\deep\\dir")), 0), Truth::True);
        QCOMPARE(EvaluateFieldCondition(Target, CellValue::Ready(QString("D:\\\\nas\\share")), 0), Truth::False);
        QJsonObject Regex = Number("file.fileVersion", "text", "notMatches", "\\A10\\."); Regex["syntax"] = "regex";
        auto [Version, VersionError] = Compile(Regex);
        QVERIFY(VersionError.isEmpty());
        QCOMPARE(EvaluateFieldCondition(Version, CellValue::Ready(QString("10.0.26100.1")), 0), Truth::False);
        QCOMPARE(EvaluateFieldCondition(Version, CellValue::Ready(QString("6.3.9600")), 0), Truth::True);

        // Boolean and dates.
        auto [ReadOnly, ReadOnlyError] = Compile(Number("file.readOnly", "boolean", "is", "true"));
        QVERIFY(ReadOnlyError.isEmpty());
        QCOMPARE(EvaluateFieldCondition(ReadOnly, CellValue::Ready(true), 0), Truth::True);
        QCOMPARE(EvaluateFieldCondition(ReadOnly, CellValue::Ready(false), 0), Truth::False);
        auto [Recent, RecentError] = Compile(Number("file.created", "timestamp", "within", "7", "days"));
        QVERIFY(RecentError.isEmpty());
        const auto Now = QDateTime::fromString("2026-11-04T12:00:00Z", Qt::ISODate);
        QCOMPARE(EvaluateFieldCondition(Recent, CellValue::Ready(Now.addDays(-6)), Now.toMSecsSinceEpoch()), Truth::True);
        QCOMPARE(EvaluateFieldCondition(Recent, CellValue::Ready(Now.addDays(-8)), Now.toMSecsSinceEpoch()), Truth::False);
        QCOMPARE(EvaluateFieldCondition(Recent, CellValue::Ready(Now.addDays(1)), Now.toMSecsSinceEpoch()), Truth::False);
        auto [Before, BeforeError] = Compile(Number("file.accessed", "timestamp", "before", "2026-01-01T00:00:00+09:00"));
        QVERIFY(BeforeError.isEmpty());
        QCOMPARE(EvaluateFieldCondition(Before, CellValue::Ready(QDateTime::fromString("2025-12-31T14:59:59Z", Qt::ISODate)), 0), Truth::True);
        QCOMPARE(EvaluateFieldCondition(Before, CellValue::Ready(QDateTime::fromString("2025-12-31T15:00:00Z", Qt::ISODate)), 0), Truth::False);

        // Kleene AND / OR.
        const QList<Truth> All{Truth::False, Truth::True, Truth::Unknown};
        for (const auto L : All) for (const auto R : All) {
            const auto And = Combine(L, R, true), Or = Combine(L, R, false);
            QCOMPARE(And, L == Truth::False || R == Truth::False ? Truth::False : L == Truth::Unknown || R == Truth::Unknown ? Truth::Unknown : Truth::True);
            QCOMPARE(Or, L == Truth::True || R == Truth::True ? Truth::True : L == Truth::Unknown || R == Truth::Unknown ? Truth::Unknown : Truth::False);
        }

        // Built-in metadata the editors rely on.
        const auto Fields = BuiltInFields();
        const auto Find = [&](const QString& Id) { return *std::find_if(Fields.begin(), Fields.end(), [&](const auto& F) { return F.id_ == Id; }); };
        QCOMPARE(Find("file.alternateDataStream.totalSize").unit_, FieldUnit::Bytes);
        QCOMPARE(Find("file.imageWidth").unit_, FieldUnit::Pixels);
        QCOMPARE(Find("file.alternateDataStream.zone").choices_.size(), 5);
        QCOMPARE(Find("file.alternateDataStream.zone").choices_[3].value_, QString("3"));
        QCOMPARE(CostOf(Find("file.stem")), ConditionCost::Immediate);
        QCOMPARE(CostOf(Find("file.alternateDataStream.count")), ConditionCost::Slow);
    }
    void vfsColumnTypesAndScope()
    {
        using namespace hc::columns;
        QCOMPARE(VfsColumnCell("9007199254740993", 16).Raw().toULongLong(), quint64(9007199254740993ULL));
        QCOMPARE(VfsColumnCell("-2", 16).Raw().toLongLong(), -2);
        QCOMPARE(VfsColumnCell("12.5", 32).Raw().toDouble(), 12.5); QCOMPARE(Format(VfsColumnCell("12.5", 32)), QString("12.5%"));
        QCOMPARE(VfsColumnCell("invalid", 16).State(), ValueState::Error); QCOMPARE(VfsColumnCell("", 16).State(), ValueState::Empty);
        const auto Ranks = SortRanks({{"ten", false, false, {VfsColumnCell("10", 16)}}, {"two", false, false, {VfsColumnCell("2", 16)}}}, {Qt::AscendingOrder});
        QVERIFY(IsFieldId("dopusviewer.0123456789abcdef0123456789abcdef.0.0"));
        QVERIFY(IsFieldId("dopusvfs.0123456789abcdef0123456789abcdef.1"));
        QCOMPARE(Ranks, (QVector<int>{1, 0})); QVERIFY(IsVfsColumn("dopusvfs.0123456789abcdef0123456789abcdef.1"));
    }
    void metadataDisplayAndTypedSort()
    {
        using namespace hc::columns; using namespace hc::plugins;
        MetadataValue Value; Value.status_ = MetadataStatus::Value; Value.type_ = MetadataType::Real; Value.value_ = 0.000002; Value.display_ = "2e-6";
        const auto Cell = MetadataCell(Value); QCOMPARE(Format(Cell), QString("2e-6")); QCOMPARE(Cell.Raw().toDouble(), 0.000002);
        Value.type_ = MetadataType::Date; Value.value_ = std::string("2026-10-02"); Value.display_.clear(); QCOMPARE(MetadataCell(Value).Type(), std::optional(FieldType::Date));
        Value.type_ = MetadataType::Timestamp; Value.value_ = std::int64_t(116444736000000000LL); QCOMPARE(MetadataCell(Value).Raw().toDateTime().toMSecsSinceEpoch(), 0);
        auto Profile = DefaultProfile(); Profile.columns_[1].fieldId_ = "wdx.0123456789abcdef0123456789abcdef.2.0"; QVERIFY(IsSupportedProfile(Profile));
        Profile.columns_[1].fieldId_ += ".bad"; QVERIFY(!IsSupportedProfile(Profile));
    }
    void compositeSortValuesAndMissing()
    {
        using namespace hc::columns;
        QVector<SortRow> Rows{
            {"z", false, false, {CellValue::Ready(quint64(2)), CellValue::Ready(QString("a"))}},
            {"a", false, false, {CellValue::Ready(quint64(2)), CellValue::Ready(QString("b"))}},
            {"large", false, false, {CellValue::Ready(std::numeric_limits<quint64>::max()), CellValue::Ready(QString("c"))}},
            {"missing", false, false, {CellValue::Missing(ValueState::Error), CellValue::Ready(QString("d"))}},
            {"parent", true, true, {}}, {"folder", false, true, {}}};
        const auto Ranks = SortRanks(Rows, {Qt::AscendingOrder, Qt::DescendingOrder});
        QCOMPARE(Ranks[4], 0); QCOMPARE(Ranks[5], 1); QCOMPARE(Ranks[1], 2); QCOMPARE(Ranks[0], 3); QCOMPARE(Ranks[2], 4); QCOMPARE(Ranks[3], 5);
        const auto Desc = SortRanks(Rows, {Qt::DescendingOrder}); QCOMPARE(Desc[4], 0); QCOMPARE(Desc[5], 1); QCOMPARE(Desc[2], 2); QCOMPARE(Desc[3], 5);
        Rows = {{"id2", false, false, {CellValue::Ready(QString("same"))}}, {"id1", false, false, {CellValue::Ready(QString("same"))}}};
        QCOMPARE(SortRanks(Rows, {Qt::AscendingOrder}), (QVector<int>{1, 0}));
    }
    void queryDedupCancellationAndLifetime()
    {
        using namespace hc::columns;
        auto Calls = std::make_shared<std::atomic<int>>(0);
        QueryEngine Engine([Calls](const QString&, const QString&, std::stop_token) {
            const int Call = ++*Calls; std::this_thread::sleep_for(std::chrono::milliseconds(Call == 1 ? 180 : 20));
            return FieldValues{{"file.fileVersion", CellValue::Ready(QString::number(Call))}, {"file.productName", CellValue::Ready(QString("product"))}};
        });
        QCOMPARE(Engine.Read("one", "file.fileVersion").State(), ValueState::Pending);
        QCOMPARE(Engine.Read("one", "file.productName").State(), ValueState::Pending); QCOMPARE(Engine.Pending(), 1);
        QTRY_COMPARE(Calls->load(), 1);
        int Beats = 0; QTimer Heartbeat; connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Beats; }); Heartbeat.start(5);
        Engine.Reset(); Engine.Read("one", "file.fileVersion");
        QTRY_COMPARE(Engine.Read("one", "file.fileVersion", false).Raw().toString(), QString("2"));
        QTest::qWait(200); QCOMPARE(Engine.Read("one", "file.fileVersion", false).Raw().toString(), QString("2")); QVERIFY(Beats > 5);
        QCOMPARE(Calls->load(), 2); QCOMPARE(Engine.Read("one", "file.productName").Raw().toString(), QString("product"));
        Engine.Cancel(); QCOMPARE(Engine.Read("other", "file.fileVersion").State(), ValueState::OnDemand); QCOMPARE(Engine.Pending(), 0);
        Engine.Resume(); Engine.Read("other", "file.fileVersion"); QTRY_COMPARE(Engine.Pending(), 0);
        auto Started = std::make_shared<std::atomic<bool>>(false);
        auto Disposable = std::make_unique<QueryEngine>([Started](const QString&, const QString&, std::stop_token) { *Started = true; std::this_thread::sleep_for(std::chrono::milliseconds(180)); return FieldValues{}; });
        Disposable->Read("slow", "file.fileVersion"); QTRY_VERIFY(Started->load());
        QElapsedTimer Timer; Timer.start(); Disposable.reset(); QVERIFY(Timer.elapsed() < 100); QTest::qWait(200);
    }
    void querySnapshotRetainsEarlyResults()
    {
        using namespace hc::columns;
        QueryEngine Engine([](const QString&, const QString&, std::stop_token) {
            return FieldValues{{"file.fileVersion", CellValue::Ready(QString("ready"))}};
        }, nullptr, 20);
        Engine.BeginSnapshot(); Engine.Read("one", "file.fileVersion");
        QTRY_COMPARE(Engine.Pending(), 0);
        QTest::qWait(40);
        QCOMPARE(Engine.Read("one", "file.fileVersion", false).State(), ValueState::Ready);
        Engine.EndSnapshot();
        QCOMPARE(Engine.Read("one", "file.fileVersion", false).State(), ValueState::Pending);
        Engine.Read("one", "file.fileVersion"); QTRY_COMPARE(Engine.Pending(), 0);
        QTest::qWait(40); Engine.BeginSnapshot();
        QCOMPARE(Engine.Read("one", "file.fileVersion", false).State(), ValueState::Pending);
    }
    void slowQueriesLeaveMetadataCapacity()
    {
        using namespace hc::columns;
        struct State { std::atomic<int> slow{0}, active{0}, peak{0}, zone{0}; std::atomic<bool> release{false}; };
        auto Shared = std::make_shared<State>();
        const int Limit = qMax(2, QThread::idealThreadCount());
        QueryEngine Engine([Shared](const QString&, const QString& Family, std::stop_token Stop) {
            const int Active = ++Shared->active;
            int Peak = Shared->peak.load(); while (Peak < Active && !Shared->peak.compare_exchange_weak(Peak, Active)) {}
            if (Family == "version") {
                ++Shared->slow; QElapsedTimer Timer; Timer.start();
                while (!Shared->release && !Stop.stop_requested() && Timer.elapsed() < 5000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else if (Family == "zone") ++Shared->zone;
            --Shared->active;
            return FieldValues{{Family == "zone" ? "file.alternateDataStream.zone" : "file.fileVersion", CellValue::Ready(quint64(3))}};
        });
        for (int I = 0; I < Limit + 2; ++I) Engine.Read(QString::number(I), "file.fileVersion", true, false);
        QTRY_COMPARE(Shared->slow.load(), Limit - 1);
        Engine.Read("visible", "file.alternateDataStream.zone"); Engine.Read("visible", "file.alternateDataStream.zone");
        QTRY_COMPARE(Engine.Read("visible", "file.alternateDataStream.zone", false).State(), ValueState::Ready);
        QCOMPARE(Shared->zone.load(), 1); QVERIFY(Shared->slow.load() <= Limit - 1);
        QVERIFY(Shared->peak.load() <= Limit);
        Shared->release = true; QTRY_COMPARE(Engine.Pending(), 0);
        // Local metadata can use the full model budget; the provider queue reserves no idle workers.
        QVERIFY(Shared->slow.load() == Limit + 2);
    }
    void flatProfileRoundTripAndRules()
    {
        using namespace hc::columns;
        using P = FlatPresentation;
        // 모르는 키는 다시 저장해도 남는다(다음 버전의 설정을 지우지 않는다).
        auto Profile = FlatDefaultProfile(); Profile.title_ = "시험";
        Profile.presentation_.structure_ = P::Structure::Column; Profile.presentation_.location_ = P::Location::SecondLine;
        Profile.presentation_.groupField_ = "file.extension"; Profile.presentation_.groupMode_ = "relativeDate";
        auto Encoded = EncodeFlatProfile(Profile);
        QCOMPARE(Encoded["presentation"].toObject()["lines"].toInt(), 2);
        QCOMPARE(Encoded["presentation"].toObject()["group"].toObject()["mode"].toString(), QString("value"));
        Encoded["future"] = "keep"; auto Presentation = Encoded["presentation"].toObject(); Presentation["futureKey"] = 3; Encoded["presentation"] = Presentation;
        auto Decoded = DecodeFlatProfile(Encoded); QVERIFY(Decoded);
        QCOMPARE(Decoded->presentation_.lines_, 2);
        QCOMPARE(Decoded->presentation_.groupMode_, QString("value"));
        const auto Again = EncodeFlatProfile(*Decoded);
        QCOMPARE(Again["future"].toString(), QString("keep"));
        QCOMPARE(Again["presentation"].toObject()["futureKey"].toInt(), 3);
        QVERIFY(!DecodeFlatProfile(QJsonObject{{"version", 2}, {"id", IdText(QUuid::createUuid())}, {"title", "x"}}));
        // 단계 규칙: 썸네일은 묶음 없음 · 폴더 구획만, 트리는 한 줄, 폴더 그룹은 경로 위치 고정.
        P Value; Value.view_ = P::View::Thumbnails; Value.structure_ = P::Structure::Tree; Value.lines_ = 2;
        Value = NormalizeFlatPresentation(Value);
        QCOMPARE(Value.structure_, P::Structure::None); QCOMPARE(Value.lines_, 1);
        Value = {}; Value.structure_ = P::Structure::Tree; Value.lines_ = 2; Value.location_ = P::Location::Column;
        Value = NormalizeFlatPresentation(Value);
        QCOMPARE(Value.lines_, 1); QCOMPARE(Value.location_, P::Location::Prefix);
        QVERIFY(FlatRows(Value).tree_); QVERIFY(FlatRows(Value).linesFixed_); QVERIFY(!FlatRows(Value).location_);
        Value = {}; Value.structure_ = P::Structure::Folder; Value.location_ = P::Location::Column;
        QCOMPARE(NormalizeFlatPresentation(Value).location_, P::Location::Prefix);
        Value = {}; Value.tileSize_ = 100; Value.treeExpandLevel_ = 7;
        QCOMPARE(NormalizeFlatPresentation(Value).tileSize_, 96); QCOMPARE(NormalizeFlatPresentation(Value).treeExpandLevel_, -1);
        // 합성: 시안 D · A · C · B · 열 값 그룹.
        const auto Origin = DefaultProfile();
        auto Flat = FlatDefaultProfile();
        auto Effective = ComposeFlatProfile(Flat, Origin);
        QVERIFY(Effective.prefix_); QVERIFY(Effective.depthSort_); QVERIFY(!Effective.columns_.grouping_);
        QCOMPARE(Effective.columns_.id_, FlatBuiltInProfileId); QCOMPARE(Effective.columns_.mode_, LayoutMode::Details);
        QVERIFY(IsSupportedProfile(Effective.columns_));
        Flat.presentation_.location_ = P::Location::Column;
        Effective = ComposeFlatProfile(Flat, Origin);
        QVERIFY(!Effective.prefix_);
        QCOMPARE(Effective.columns_.columns_[FlatNameColumn(Effective.columns_) + 1].fieldId_, QString("file.relativePath"));
        Flat.presentation_.columnLast_ = true;
        Effective = ComposeFlatProfile(Flat, Origin);
        QCOMPARE(Effective.columns_.columns_.back().fieldId_, QString("file.relativePath"));
        Flat.presentation_.location_ = P::Location::SecondLine; Flat.presentation_.secondRight_ = P::SecondRight::Size;
        Effective = ComposeFlatProfile(Flat, Origin);
        QCOMPARE(Effective.columns_.mode_, LayoutMode::NameFirst);
        const auto Size = FlatFieldColumn(Effective.columns_, "file.size");
        QVERIFY(Size >= 0); QVERIFY(!Effective.columns_.columns_[Size].widths_[1].hidden_);
        Flat.presentation_ = {}; Flat.presentation_.structure_ = P::Structure::Folder; Flat.presentation_.groupTitleLeaf_ = true;
        Effective = ComposeFlatProfile(Flat, Origin);
        QVERIFY(Effective.columns_.grouping_); QVERIFY(Effective.groupTitleLeaf_);
        const auto Relative = FlatFieldColumn(Effective.columns_, "file.relativePath");
        QCOMPARE(Effective.columns_.grouping_->levels_.front().columnId_, Effective.columns_.columns_[Relative].id_);
        QVERIFY(ValidateGrouping(Effective.columns_).isEmpty());
        Flat.presentation_ = {}; Flat.presentation_.structure_ = P::Structure::Column; Flat.presentation_.groupField_ = "file.extension";
        Effective = ComposeFlatProfile(Flat, Origin);
        QVERIFY(Effective.columns_.grouping_); QCOMPARE(Effective.columns_.grouping_->levels_.front().mode_, QString("value"));
        QVERIFY(ValidateGrouping(Effective.columns_).isEmpty());
        // 적용 조건: 조건 순서대로, 잘못된 세트는 건너뛰고, 맞지 않으면 기본 세트 → 기본 제공 세트.
        auto Other = FlatDefaultProfile(); Other.id_ = QUuid::createUuid(); Other.title_ = "다른";
        QJsonObject Library{{IdText(Other.id_), EncodeFlatProfile(Other)}};
        QJsonObject Selection = DefaultFlatSelection();
        Selection["rules"] = QJsonArray{QJsonObject{{"enabled", true}, {"path", "D:/Photos/*"}, {"profile", IdText(QUuid::createUuid())}},
            QJsonObject{{"enabled", true}, {"path", "D:/Photos/*"}, {"profile", IdText(Other.id_)}}};
        const auto Match = [](const QString& Pattern, const QString& Path) { return MatchFolderCondition(Pattern, Path); };
        int Rule = -1;
        QCOMPARE(SelectFlatProfile(Selection, Library, "D:/Photos/2026", Match, &Rule).id_, Other.id_); QCOMPARE(Rule, 1);
        QCOMPARE(SelectFlatProfile(Selection, Library, "C:/Work", Match, &Rule).id_, FlatBuiltInProfileId); QCOMPARE(Rule, -1);
        Selection["defaultProfile"] = IdText(Other.id_);
        QCOMPARE(SelectFlatProfile(Selection, Library, "C:/Work", Match).id_, Other.id_);
        QVERIFY(!ValidateFlatSelection(Selection, Library).isEmpty());
        Selection["rules"] = QJsonArray{QJsonObject{{"enabled", true}, {"path", "D:/Photos/*"}, {"profile", IdText(Other.id_)}}};
        QVERIFY(ValidateFlatSelection(Selection, Library).isEmpty());
        // 이관은 한 번만 하고, 반복해도 같다.
        QJsonObject Columns;
        QVERIFY(MigrateFlatColumnSets(Columns));
        const auto First = Columns;
        QVERIFY(!MigrateFlatColumnSets(Columns));
        QCOMPARE(Columns, First);
        QVERIFY(DecodeFlatProfile(Columns[FlatProfilesKey].toObject()[IdText(FlatBuiltInProfileId)].toObject()));
        QVERIFY(Columns[FlatSelectionKey].isObject());
    }
    void pathFlagsAndFormats()
    {
        using namespace hc::columns;
        FileSnapshot File; File.name_ = "report.tar.gz"; File.parentPath_ = "C:\\Projects\\docs";
        File.attributes_ = 0x21; File.size_ = 1048576; File.typeDescription_ = "Archive document";
        File.accessed_ = QDateTime(QDate(2026, 10, 2), QTime(12, 34, 56));
        QCOMPARE(Resolve("file.path", File).Raw().toString(), QString("C:\\Projects\\docs\\report.tar.gz"));
        QCOMPARE(Resolve("file.parentName", File).Raw().toString(), QString("docs"));
        QCOMPARE(Resolve("file.pathLength", File).Raw().toULongLong(), quint64(Resolve("file.path", File).Raw().toString().size()));
        QVERIFY(Resolve("file.readOnly", File).Raw().toBool()); QVERIFY(Resolve("file.archive", File).Raw().toBool());
        QVERIFY(!Resolve("file.hidden", File).Raw().toBool());
        QCOMPARE(Resolve("file.type", File).Raw().toString(), QString("Archive document"));
        FormatOptions Date; Date.datePattern_ = "HH:mm:ss";
        QCOMPARE(Format(Resolve("file.accessed", File), Date), QString("12:34:56"));
        FormatOptions Size; Size.size_ = SizeFormat::MiB; Size.precision_ = 1;
        QCOMPARE(Format(Resolve("file.size", File), Size), QLocale().toString(1.0, 'f', 1) + " MiB");
        QCOMPARE(Resolve("file.size", File).Raw().toULongLong(), quint64(1048576));
        // 평면 보기의 상대 경로: 기준 폴더가 없으면 값이 없고, 기준 폴더의 항목은 빈 문자열이다.
        QCOMPARE(Resolve("file.relativePath", File).State(), ValueState::Empty);
        File.basePath_ = "C:\\Projects";
        QCOMPARE(Resolve("file.relativePath", File).Raw().toString(), QString("docs"));
        File.basePath_ = "c:/projects/"; File.parentPath_ = "C:/Projects/docs/2026";
        QCOMPARE(Resolve("file.relativePath", File).Raw().toString(), QString("docs\\2026"));
        File.basePath_ = "C:\\Projects\\docs\\2026";
        QCOMPARE(Resolve("file.relativePath", File).State(), ValueState::Ready);
        QVERIFY(Resolve("file.relativePath", File).Raw().toString().isEmpty());
        File.basePath_ = "C:\\"; File.parentPath_ = "C:\\Projects\\docs";
        QCOMPARE(Resolve("file.relativePath", File).Raw().toString(), QString("Projects\\docs"));
        // 접두가 같아도 다른 폴더(C:\Proj와 C:\Projects)는 상대 경로가 아니다.
        File.basePath_ = "C:\\Proj";
        QCOMPARE(Resolve("file.relativePath", File).State(), ValueState::Empty);
        File.basePath_ = "\\\\server\\share"; File.parentPath_ = "\\\\server\\share\\a\\b";
        QCOMPARE(Resolve("file.relativePath", File).Raw().toString(), QString("a\\b"));
        File.basePath_.clear();
        QCOMPARE(MakeGroupValue("file.relativePath", "value", CellValue::Ready(QString())).text_, GroupText("기준 폴더"));
        QCOMPARE(MakeGroupValue("file.relativePath", "value", CellValue::Ready(QString("A\\b"))).key_, MakeGroupValue("file.relativePath", "value", CellValue::Ready(QString("a\\B"))).key_);
        ColumnDefinition Relative; Relative.fieldId_ = "file.relativePath";
        QVERIFY(SupportsGrouping(Relative));
        File.parentPath_ = "\\\\server\\share\\";
        QCOMPARE(Resolve("file.path", File).Raw().toString(), QString("\\\\server\\share\\report.tar.gz"));
        QCOMPARE(Resolve("file.parentName", File).Raw().toString(), QString("share"));
        auto Profile = DefaultProfile(); Profile.columns_[1].fieldId_ = "file.path";
        QVERIFY(IsSupportedProfile(Profile)); Profile.columns_[1].fieldId_ = "wdx.unknown.field";
        QVERIFY(!IsSupportedProfile(Profile)); // Unsupported providers remain preserved but not executed.
    }
    void typedValuesPreservePrecisionAndState()
    {
        using namespace hc::columns;
        for (auto State : {ValueState::Empty, ValueState::Pending, ValueState::OnDemand, ValueState::Unsupported, ValueState::Error}) {
            const auto Value = CellValue::Missing(State);
            QCOMPARE(Value.State(), State);
            QVERIFY(!Value.Raw().isValid()); QVERIFY(!Value.Value()); QVERIFY(!Value.Type());
        }
        const auto Zero = CellValue::Ready(quint64(0));
        QCOMPARE(Zero.State(), ValueState::Ready); QCOMPARE(Format(Zero), QString("0"));
        const auto Large = CellValue::Ready(std::numeric_limits<quint64>::max());
        QCOMPARE(Large.Type(), std::optional(FieldType::UnsignedInteger));
        QCOMPARE(Large.Raw().toULongLong(), std::numeric_limits<quint64>::max());
        QCOMPARE(Format(Large), QString("18446744073709551615"));
        QCOMPARE(CellValue::Ready(std::numeric_limits<qint64>::min()).Raw().toLongLong(), std::numeric_limits<qint64>::min());
        QCOMPARE(CellValue::Ready(false).Type(), std::optional(FieldType::Boolean));
        QCOMPARE(CellValue::Ready(QString{}).State(), ValueState::Ready);
        QCOMPARE(CellValue::Ready(std::numeric_limits<double>::quiet_NaN()).State(), ValueState::Error);
        const auto Size = CellValue::Ready(quint64(1536));
        FormatOptions Options; Options.size_ = SizeFormat::KiB;
        QCOMPARE(Format(Size, Options), QLocale().toString(1.5, 'f', 2) + " KiB");
        QCOMPARE(Size.Raw().toULongLong(), quint64(1536)); // Formatting cannot alter sort data.
    }
    void fileNamesAndMissingMetadata()
    {
        using namespace hc::columns;
        FileSnapshot File; File.name_ = "archive.tar.gz"; File.size_ = 0;
        QCOMPARE(Resolve("file.name", File).Raw().toString(), QString("archive.tar.gz"));
        QCOMPARE(Resolve("file.stem", File).Raw().toString(), QString("archive.tar"));
        QCOMPARE(Resolve("file.extension", File).Raw().toString(), QString("gz"));
        File.name_ = ".config";
        QCOMPARE(Resolve("file.stem", File).Raw().toString(), File.name_);
        QCOMPARE(Resolve("file.extension", File).State(), ValueState::Empty);
        File.name_ = ".claude.json";
        QCOMPARE(Resolve("file.stem", File).Raw().toString(), QString(".claude"));
        File.name_ = "trailing.";
        QCOMPARE(Resolve("file.stem", File).Raw().toString(), QString("trailing"));
        QCOMPARE(Resolve("file.extension", File).State(), ValueState::Empty);
        File.name_ = "folder.with.dots"; File.directory_ = true; File.size_.reset();
        QCOMPARE(Resolve("file.stem", File).Raw().toString(), File.name_);
        QCOMPARE(Resolve("file.size", File).State(), ValueState::OnDemand);
        File.size_ = 0;
        QCOMPARE(Resolve("file.size", File).State(), ValueState::Ready);
        QCOMPARE(Resolve("file.created", File).State(), ValueState::Empty);
        for (const auto& Id : {"hc.size", "hc.fs.size", "[=HC.Fs.Size]", "wdx.missing.field"})
            QCOMPARE(Resolve(Id, File).State(), ValueState::Unsupported);
    }
    void profileRoundTripAndStableIdentity()
    {
        using namespace hc::columns;
        auto Profile = DefaultProfile();
        QVERIFY(Validate(Profile).isEmpty());
        QCOMPARE(Profile.columns_[0].id_, DefaultProfile().columns_[0].id_);
        for (auto Mode : {LayoutMode::Details, LayoutMode::NameFirst, LayoutMode::NameLast}) {
            Profile.mode_ = Mode;
            const auto Stored = QJsonDocument(EncodeProfile(Profile)).toJson();
            const auto Loaded = DecodeProfile(QJsonDocument::fromJson(Stored).object());
            QVERIFY(Loaded); QVERIFY(*Loaded == Profile);
        }
        const auto PrimaryId = Profile.primaryNameId_;
        Profile.columns_[0].title_ = "Renamed caption";
        Profile.columns_[0].fieldId_ = "file.stem";
        std::swap(Profile.columns_[0], Profile.columns_[2]);
        const auto Loaded = DecodeProfile(EncodeProfile(Profile));
        QVERIFY(Loaded); QCOMPARE(Loaded->primaryNameId_, PrimaryId);
        QCOMPARE(Loaded->columns_[2].id_, PrimaryId);
        auto DuplicateField = Profile.columns_[2]; DuplicateField.id_ = QUuid::createUuid();
        Profile.columns_.append(DuplicateField);
        QVERIFY(Validate(Profile).isEmpty()); // Repeated fields are different column instances.
        QCOMPARE(Profile.columns_[2].widths_[1].value_, 100.0);
        QCOMPARE(Profile.columns_[2].widths_[2].value_, 100.0);
    }
    void rejectInvalidProfiles()
    {
        using namespace hc::columns;
        auto Profile = DefaultProfile();
        Profile.columns_[1].id_ = Profile.columns_[0].id_;
        QVERIFY(!Validate(Profile).isEmpty()); QVERIFY(EncodeProfile(Profile).isEmpty());
        Profile = DefaultProfile(); Profile.primaryNameId_ = QUuid::createUuid();
        QVERIFY(!Validate(Profile).isEmpty());
        Profile = DefaultProfile(); Profile.columns_[0].fieldId_ = "hc.fs.name";
        QVERIFY(!Validate(Profile).isEmpty());
        Profile = DefaultProfile(); Profile.sorts_.append(Profile.sorts_.front());
        QVERIFY(!Validate(Profile).isEmpty());
        Profile = DefaultProfile(); Profile.columns_[0].widths_[1].hidden_ = true;
        QVERIFY(!Validate(Profile).isEmpty());
        auto Json = EncodeProfile(DefaultProfile()); Json["version"] = 99;
        QVERIFY(!DecodeProfile(Json));
        Json = EncodeProfile(DefaultProfile());
        auto Columns = Json["columns"].toArray(); auto Column = Columns[1].toObject();
        auto Format = Column["format"].toObject(); Format["precision"] = 1.5;
        Column["format"] = Format; Columns[1] = Column; Json["columns"] = Columns;
        QVERIFY(!DecodeProfile(Json));
        QVERIFY(!DecodeProfile(QJsonObject{{"columns", QJsonArray{"hc.size"}}}));
    }
    void oldWidthsAreNotImported()
    {
        QTemporaryDir Directory; hc::settings::Repository Settings(Directory.path());
        const QMap<QString, ColumnWidthPolicy> Defaults{{"name", {WidthUnit::Percent, WidthMode::Flexible, 46}}};
        WidthSettings Old; Old.columns_["name"].value_ = 1;
        QVERIFY(SaveWidths(Settings, "Views/ColumnWidths/cmpLeftPanel/builtin-details", Old));
        const auto Key = QString(hc::columns::SettingsPrefix) + "cmpLeftPanel/new-profile";
        const auto Current = LoadWidthPolicies(Settings, Key, Defaults);
        QCOMPARE(Current.columns_["name"].value_, 46.0);
        QVERIFY(SaveWidths(Settings, Key, Current));
        QCOMPARE(LoadWidthPolicies(Settings, Key, Defaults).columns_["name"].value_, 46.0);
    }
    void percentBoundsAndRounding()
    {
        ColumnWidthPolicy Fixed{WidthUnit::Percent, WidthMode::Fixed, 25};
        QCOMPARE(Resolve(Fixed, 1000)->width_, 250);
        QCOMPARE(Resolve(Fixed, 800)->width_, 200);
        QVERIFY(!Resolve(Fixed, 0));
        ColumnWidthPolicy Flexible{WidthUnit::Percent, WidthMode::Flexible, 30, 15, 45};
        QCOMPARE(Resolve(Flexible, 1000)->width_, 300);
        QCOMPARE(Resolve(FromDrag(Flexible, 10, 1000), 1000)->width_, 150);
        QCOMPARE(Resolve(FromDrag(Flexible, 900, 1000), 1000)->width_, 450);
        QCOMPARE(FromDrag(Fixed, 900, 1000).value_, 25.0);
        Fixed.value_ = 33.333333;
        QVERIFY(std::abs(Resolve(Fixed, 801)->width_ - 801 * Fixed.value_ / 100) <= 0.5);
        Fixed.value_ = 0;
        QCOMPARE(Resolve(Fixed, 800)->width_, 1);
        ColumnWidthPolicy Pixels{WidthUnit::Pixels, WidthMode::Flexible, 220};
        QCOMPARE(Resolve(Pixels, 100)->width_, 220); // Overflow is not normalized.
        for (int Index = 0; Index < 1000; ++Index) { Resolve(Flexible, 800); Resolve(Flexible, 1000); }
        QCOMPARE(Flexible.value_, 30.0);
    }
    void invalidPolicies()
    {
        ColumnWidthPolicy Policy{WidthUnit::Percent, WidthMode::Fixed, 25, 30, 50};
        QVERIFY(!Validate(Policy).empty());
        Policy.mode_ = WidthMode::Flexible;
        QVERIFY(Validate(Policy).empty());
        QCOMPARE(Resolve(Policy, 1000)->width_, 300);
        Policy.minimum_ = 60;
        QVERIFY(!Validate(Policy).empty());
        for (double Value : {-1.0, 100.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        { Policy = {}; Policy.unit_ = WidthUnit::Percent; Policy.value_ = Value; QVERIFY(!Validate(Policy).empty()); }
    }
    void storageDefaultsAndUnknownVersion()
    {
        QTemporaryDir Directory;
        const auto Path = Directory.path();
        hc::settings::Repository Settings(Path);
        const QMap<QString, int> Defaults{{"name", 220}, {"size", 80}};
        auto Values = LoadWidths(Settings, "view", Defaults);
        QCOMPARE(Values.columns_["name"].unit_, WidthUnit::Pixels);
        QCOMPARE(Values.columns_["name"].value_, 220.0);
        Values.columns_["name"] = {WidthUnit::Percent, WidthMode::Fixed, 25, 10, 40, true};
        QVERIFY(SaveWidths(Settings, "view", Values));
        hc::settings::Repository Reopened(Path);
        const auto Restored = LoadWidths(Reopened, "view", Defaults);
        QVERIFY(Restored.columns_["name"] == Values.columns_["name"]);
        QCOMPARE(Resolve(Restored.columns_["name"], 800)->width_, 200);
        Settings.Put(hc::settings::Section::Columns, QJsonObject{{"view", QJsonObject{{"version",2},{"columns",QJsonObject{}}}}});
        const auto Unknown = LoadWidths(Settings, "view", Defaults);
        QVERIFY(!Unknown.writable_);
        QVERIFY(!SaveWidths(Settings, "view", Unknown));
        QVERIFY(Settings.Read(hc::settings::Section::Columns).toObject()["view"].toObject()["version"].toInt() == 2);
        QVERIFY(!DecodeWidth(QJsonObject{{"unit", "percent"}, {"mode", "fixed"}, {"value", "25"}}));
    }
};
QTEST_GUILESS_MAIN(ColumnTests)
#include "tstColumnWidths.moc"
