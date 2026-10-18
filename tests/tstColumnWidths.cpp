#include "UniqueLibs/columnWidthSettings.hpp"
#include <QtTest>
#include "commonLibs/columns/luaColumns.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/columns/columnSort.hpp"
#include <atomic>
#include <thread>
#include <limits>
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/columns/columnProfileStorage.hpp"
#include "commonLibs/view/listIconSize.hpp"
using namespace hc::view;

class ColumnTests : public QObject
{
    Q_OBJECT
private slots:
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
