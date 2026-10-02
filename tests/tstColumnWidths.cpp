#include "UniqueLibs/columnWidthSettings.hpp"
#include <QtTest>
#include <limits>
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/columns/columnProfileStorage.hpp"
using namespace hc::view;

class ColumnTests : public QObject
{
    Q_OBJECT
private slots:
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
