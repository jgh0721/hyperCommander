#include "UniqueLibs/columnWidthSettings.hpp"
#include <QtTest>
#include <limits>
using namespace hc::view;

class ColumnTests : public QObject
{
    Q_OBJECT
private slots:
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
    void storageMigrationAndUnknownVersion()
    {
        QTemporaryDir Directory;
        const auto Path = Directory.filePath("widths.ini");
        QSettings Settings(Path, QSettings::IniFormat);
        const QMap<QString, int> Legacy{{"name", 220}, {"size", 80}};
        auto Values = LoadWidths(Settings, "view", Legacy);
        QCOMPARE(Values.columns_["name"].unit_, WidthUnit::Pixels);
        QCOMPARE(Values.columns_["name"].value_, 220.0);
        Values.columns_["name"] = {WidthUnit::Percent, WidthMode::Fixed, 25, 10, 40, true};
        QVERIFY(SaveWidths(Settings, "view", Values));
        QSettings Reopened(Path, QSettings::IniFormat);
        const auto Restored = LoadWidths(Reopened, "view", Legacy);
        QVERIFY(Restored.columns_["name"] == Values.columns_["name"]);
        QCOMPARE(Resolve(Restored.columns_["name"], 800)->width_, 200);
        Settings.setValue("view", QByteArray("{\"version\":2,\"columns\":{}}"));
        const auto Unknown = LoadWidths(Settings, "view", Legacy);
        QVERIFY(!Unknown.writable_);
        QVERIFY(!SaveWidths(Settings, "view", Unknown));
        QVERIFY(Settings.value("view").toByteArray().contains("2"));
        QVERIFY(!DecodeWidth(QJsonObject{{"unit", "percent"}, {"mode", "fixed"}, {"value", "25"}}));
    }
};
QTEST_GUILESS_MAIN(ColumnTests)
#include "tstColumnWidths.moc"
