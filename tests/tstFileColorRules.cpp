#include "stdafx.h"
#include "UniqueLibs/settings/fileColorRules.hpp"
#include "UniqueLibs/fileRulePaint.hpp"
#include "HyperCommander/UIs/fileColorConditionRow.hpp"
#include "UniqueLibs/itemSelection.hpp"
#include <QtTest>
#include <QElapsedTimer>

using namespace hc::settings;

class FileColorRuleTests final : public QObject
{
    Q_OBJECT
private slots:
    void dimAndRowBorders()
    {
        auto Doc = document({{"type", "pattern"}, {"syntax", "literal"}, {"op", "contains"}, {"value", "sample"}});
        auto Groups = Doc["groups"].toArray(); auto Group = Groups[0].toObject();
        Group["font"] = QJsonObject{{"dim", true}, {"border", "dot"}}; Groups[0] = Group; Doc["groups"] = Groups;
        FileColorRules Rules; QVERIFY(Rules.Load(Doc).isEmpty()); FileColorRules::EvaluationContext Context;
        const auto Style = Rules.Evaluate(FileRuleInput{"sample.txt"}, &Context);
        QCOMPARE(Style.font_["dim"], QJsonValue(true)); QCOMPARE(Style.font_["border"].toString(), QString("dot"));
        auto Higher = Group; Higher["id"] = "higher"; Higher["name"] = "override";
        Higher["font"] = QJsonObject{{"dim", false}, {"border", "none"}};
        Doc["mode"] = "merge"; Doc["groups"] = QJsonArray{Higher, Group}; QVERIFY(Rules.Load(Doc).isEmpty());
        const auto Merged = Rules.Evaluate(FileRuleInput{"sample.txt"}, &Context);
        QCOMPARE(Merged.font_["dim"], QJsonValue(false)); QCOMPARE(Merged.font_["border"].toString(), QString("none"));
        Higher["font"] = QJsonObject{{"dim", "yes"}}; Doc["groups"] = QJsonArray{Higher}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
        Higher["font"] = QJsonObject{{"border", "unknown"}}; Doc["groups"] = QJsonArray{Higher}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
        const auto FramePixels = [](const QString& Border) {
            QImage Image(160, 36, QImage::Format_ARGB32_Premultiplied); Image.fill(Qt::white);
            { QPainter Painter(&Image); hc::ui::PaintFileRuleBorder(&Painter, Image.rect(), Qt::red, {{"border", Border}}); }
            int Count = 0; for (int Y = 0; Y < Image.height(); ++Y) for (int X = 0; X < Image.width(); ++X)
                if (Image.pixelColor(X, Y) == QColor(Qt::red)) ++Count;
            return Count;
        };
        const auto Solid = FramePixels("solid"), Dotted = FramePixels("dot");
        QVERIFY(Solid > 300); QVERIFY(Dotted > 0 && Dotted < Solid); QCOMPARE(FramePixels("none"), 0);
        for (const auto& Key : {"dash", "dashDot", "dashDotDot"}) QVERIFY(FramePixels(Key) > 0);
    }
    void rtlNamesAndOperators()
    {
        const QStringList Rtl{QStringLiteral("left-\u05d0.txt"), QStringLiteral("left-\u0627.txt"),
            QStringLiteral("safe-\u202etxt.exe"), QStringLiteral("\u200fmark.txt"), QStringLiteral("\u061cmark.txt"),
            QStringLiteral("\u202bembed.txt"), QStringLiteral("\u2067isolate.txt"), QString::fromUcs4(U"\U0001e900.txt")};
        const QStringList Plain{QStringLiteral("ASCII.txt"), QStringLiteral("한글.txt"), QStringLiteral("\u202dltr.txt"),
            QStringLiteral("\u202altr.txt"), QStringLiteral("\u202cend.txt"), QStringLiteral("\U0001f600.txt")};
        for (const auto& Op : {"contains", "notContains"}) {
            const auto Doc = document({{"type", "rtl"}, {"op", Op}});
            QVERIFY(FileColorRules::Validate(Doc).isEmpty());
            FileColorRules Rules; QVERIFY(Rules.Load(Doc).isEmpty()); FileColorRules::EvaluationContext Context;
            for (const auto& Name : Rtl) { QVERIFY(FileColorRules::HasRtlName(Name)); QCOMPARE(Rules.Matches(FileRuleInput{Name}, &Context).matched_, QByteArray(Op) == "contains"); }
            for (const auto& Name : Plain) { QVERIFY(!FileColorRules::HasRtlName(Name)); QCOMPARE(Rules.Matches(FileRuleInput{Name}, &Context).matched_, QByteArray(Op) == "notContains"); }
            QCOMPARE(Rules.Matches(FileRuleInput{Rtl.front(), 0x10}, &Context).matched_, QByteArray(Op) == "contains");
        }
        QVERIFY(!FileColorRules::Validate(document({{"type", "rtl"}, {"op", "is"}})).isEmpty());
        QTableWidget Table(0, 4); hc::ui::FileColorConditionRow Row(&Table, {{"type", "rtl"}, {"op", "contains"}}, 2);
        auto* Field = qobject_cast<QComboBox*>(Table.cellWidget(0, 0)); QCOMPARE(Field->currentText(), QString("RTL 문자"));
        auto* Operator = qobject_cast<QComboBox*>(Table.cellWidget(0, 1)); QCOMPARE(Operator->count(), 2);
        QCOMPARE(Operator->itemText(0), QString("포함")); QCOMPARE(Operator->itemText(1), QString("비포함"));
        QVERIFY(!Table.cellWidget(0, 2)->findChild<QLineEdit*>());
        Operator->setCurrentIndex(1); QCOMPARE(Row.Record()["op"].toString(), QString("notContains"));
        QCOMPARE(Row.Record()["type"].toString(), QString("rtl")); QVERIFY(!Row.Record().contains("value"));
    }
    void conditionEditor()
    {
        QTableWidget Table(0, 4);
        const QJsonObject Original{{"type", "extension"}, {"op", "in"}, {"value", "txt"}, {"future", "keep"}};
        hc::ui::FileColorConditionRow Row(&Table, Original, 2);
        QCOMPARE(Row.Record(), Original);
        auto* Operator = qobject_cast<QComboBox*>(Table.cellWidget(0, 1)); QCOMPARE(Operator->count(), 6);
        const auto Value = [&] { return Table.cellWidget(0, 2)->findChild<QLineEdit*>("hc.condition.value"); };
        Operator->setCurrentIndex(Operator->findData("regexNotMatches"));
        const auto Pattern = QStringLiteral("\\A(?:a;b|\\d+)\\z"); Value()->setText(Pattern);
        QCOMPARE(Row.Record()["syntax"].toString(), QString("regex")); QCOMPARE(Row.Record()["op"].toString(), QString("notMatches"));
        QCOMPARE(Row.Record()["value"].toString(), Pattern);
        Operator->setCurrentIndex(Operator->findData("in")); QCOMPARE(Value()->text(), QString("txt"));
        Operator->setCurrentIndex(Operator->findData("regexMatches")); QCOMPARE(Value()->text(), Pattern);
        Operator->setCurrentIndex(Operator->findData("globMatches")); Value()->setText("jp*; literal\\;x");
        QCOMPARE(Row.Record()["value"].toArray(), QJsonArray({"jp*", "literal;x"})); QCOMPARE(Row.Record()["future"].toString(), QString("keep"));
        auto* Field = qobject_cast<QComboBox*>(Table.cellWidget(0, 0)); Field->setCurrentIndex(Field->findData("pattern"));
        Operator->setCurrentIndex(Operator->findData("notMatches")); QCOMPARE(Row.Record()["op"].toString(), QString("notMatches"));
        QVERIFY(Table.columnWidth(1) >= Operator->fontMetrics().horizontalAdvance(Operator->itemText(5)) + 36);
    }
    void selectionUsesVersionAndErrors()
    {
        using namespace hc::selection;
        auto Doc = document({{"type", "pattern"}, {"syntax", "regex"}, {"op", "matches"}, {"value", "\\Areport_\\d+\\.txt\\z"}});
        const auto List = Groups(Doc); QCOMPARE(List.size(), 1); QCOMPARE(List[0].version_, 3);
        Criteria C; C.groupId_ = List[0].id_; C.group_ = List[0].rule_; C.groupVersion_ = List[0].version_;
        Matcher Match; QVERIFY(Match.Compile(C).isEmpty()); QVERIFY(Match.Matches(FileRuleInput{"report_12.txt"})); QVERIFY(!Match.Matches(FileRuleInput{"notes.txt"}));
        C.group_["conditions"] = QJsonArray{QJsonObject{{"type", "pattern"}, {"syntax", "regex"}, {"op", "notMatches"}, {"value", "(a+)+$"}}};
        QVERIFY(Match.Compile(C).isEmpty()); QString Error;
        QVERIFY(!Match.Matches(FileRuleInput{QString(500, 'a') + '!'}, &Error)); QVERIFY(!Error.isEmpty());
    }
    void globParity()
    {
        const QStringList Patterns{"*", "?", "*.*", "a*?b", "a**b", "a.b", "[abc]", "[a-z]", "[^a-c]", "[!a-c]", "[?]", "[*]", "[]a]", "[[]", "[-a]", "[a-]", "[A-Z]", "[K]", "[s]", "[Σ]", "[K]", "[ſ]", "[ς]", "[K-Å]", "[ẞ]", "한?", "*보고서*", "literal;*.txt", "[[:alpha:]]", "[a&&b]"};
        QStringList Names{"", "a", "b", "c", "abc", "ab", "aab", "a.txt", "a.b", "README", "*", "?", "[", "]", "-", "A", "Z", "K", "k", "K", "s", "ſ", "Σ", "σ", "ς", "한글", "한글", "보고서.txt", "literal;a.txt", "aaab", "\n", QString::fromUcs4(U"😀")};
        for (int I = 0; I < 300; ++I) Names.append(QString("%1%2%3").arg(QChar('a' + I % 5)).arg(QChar('a' + (I / 5) % 5)).arg(QChar('a' + (I / 25) % 5)));
        for (const auto& Pattern : Patterns) {
            hc::text::FileNameGlob Glob; const auto Error = Glob.Compile(Pattern);
            const auto Reference = QRegularExpression::fromWildcard(Pattern);
            QCOMPARE(Glob.IsValid(), Reference.isValid());
            for (const auto& Name : Names) {
                const bool Expected = Reference.isValid() && Reference.match(Name).hasMatch();
                QVERIFY2(Glob.Matches(Name) == Expected, qPrintable(QString("pattern=%1 name=%2 expected=%3 error=%4").arg(Pattern, Name).arg(Expected).arg(Error)));
            }
        }
    }
    void matchModes_data()
    {
        QTest::addColumn<QString>("Type"); QTest::addColumn<QString>("Syntax"); QTest::addColumn<QString>("Pattern");
        QTest::addColumn<QString>("Name"); QTest::addColumn<bool>("Expected");
        QTest::newRow("extension-list") << QStringLiteral("extension") << QStringLiteral("literal") << QStringLiteral(".txt, cpp") << QStringLiteral("a.TXT") << true;
        QTest::newRow("extension-exact") << QStringLiteral("extension") << QStringLiteral("literal") << QStringLiteral("txt") << QStringLiteral("a.txtx") << false;
        QTest::newRow("none-list") << QStringLiteral("extension") << QStringLiteral("literal") << QStringLiteral("<none>") << QStringLiteral("README") << true;
        QTest::newRow("extension-glob") << QStringLiteral("extension") << QStringLiteral("wildcard") << QStringLiteral("jp*") << QStringLiteral("a.JPEG") << true;
        QTest::newRow("extension-question") << QStringLiteral("extension") << QStringLiteral("wildcard") << QStringLiteral("h??") << QStringLiteral("a.hpp") << true;
        QTest::newRow("suffix-only") << QStringLiteral("extension") << QStringLiteral("wildcard") << QStringLiteral("tar.*") << QStringLiteral("a.tar.gz") << false;
        QTest::newRow("none-glob") << QStringLiteral("extension") << QStringLiteral("wildcard") << QStringLiteral("*") << QStringLiteral("README") << true;
        QTest::newRow("none-nonempty") << QStringLiteral("extension") << QStringLiteral("wildcard") << QStringLiteral("?*") << QStringLiteral("README") << false;
        QTest::newRow("none-regex") << QStringLiteral("extension") << QStringLiteral("regex") << QStringLiteral("\\A\\z") << QStringLiteral("README") << true;
        QTest::newRow("regex-extension") << QStringLiteral("extension") << QStringLiteral("regex") << QStringLiteral("\\A(?:jpe?g|png)\\z") << QStringLiteral("a.PNG") << true;
        QTest::newRow("literal-metacharacters") << QStringLiteral("pattern") << QStringLiteral("literal") << QStringLiteral("[a].*") << QStringLiteral("before[a].*after") << true;
        QTest::newRow("literal-caseless") << QStringLiteral("pattern") << QStringLiteral("literal") << QStringLiteral("REPORT") << QStringLiteral("my_report.txt") << true;
        QTest::newRow("glob-whole") << QStringLiteral("pattern") << QStringLiteral("wildcard") << QStringLiteral("report") << QStringLiteral("my_report.txt") << false;
        QTest::newRow("glob-name") << QStringLiteral("pattern") << QStringLiteral("wildcard") << QStringLiteral("report_??.txt") << QStringLiteral("REPORT_12.TXT") << true;
        QTest::newRow("regex-substring") << QStringLiteral("pattern") << QStringLiteral("regex") << QStringLiteral("report_\\d{2}") << QStringLiteral("my_report_12.txt") << true;
        QTest::newRow("regex-anchors") << QStringLiteral("pattern") << QStringLiteral("regex") << QStringLiteral("\\Areport_\\d{2}\\.txt\\z") << QStringLiteral("my_report_12.txt") << false;
        QTest::newRow("regex-delimiters") << QStringLiteral("pattern") << QStringLiteral("regex") << QStringLiteral("\\Aa;b,c d\\z|other") << QStringLiteral("a;b,c d") << true;
        QTest::newRow("regex-unicode") << QStringLiteral("pattern") << QStringLiteral("regex") << QStringLiteral("\\A\\p{L}+\\.txt\\z") << QStringLiteral("한글.txt") << true;
        QTest::newRow("regex-backref") << QStringLiteral("pattern") << QStringLiteral("regex") << QStringLiteral("\\A(a+)\\1\\z") << QStringLiteral("aaaa") << true;
        QTest::newRow("regex-spaces") << QStringLiteral("pattern") << QStringLiteral("regex") << QStringLiteral(" a ") << QStringLiteral(" a ") << true;
    }
    void matchModes()
    {
        QFETCH(QString, Type); QFETCH(QString, Syntax); QFETCH(QString, Pattern); QFETCH(QString, Name); QFETCH(bool, Expected);
        for (bool Negate : {false, true}) {
            const QString Op = Syntax == "literal" ? Type == "extension" ? Negate ? "notIn" : "in" : Negate ? "notContains" : "contains" : Negate ? "notMatches" : "matches";
            const auto Doc = document({{"type", Type}, {"syntax", Syntax}, {"op", Op}, {"value", Syntax == "wildcard" ? QJsonValue(QJsonArray{Pattern}) : QJsonValue(Pattern)}});
            FileColorRules Rules; QVERIFY2(Rules.Load(Doc).isEmpty(), qPrintable(FileColorRules::Validate(Doc)));
            QCOMPARE(Rules.Evaluate(Name, 0, 0, false).matched_, Expected != Negate);
            QCOMPARE(Rules.Matches(FileRuleInput{Name}).matched_, Expected != Negate);
            if (Type == "extension") QVERIFY(!Rules.Evaluate(Name, 0x10, 0, false).matched_);
            QVERIFY(!Rules.Evaluate("..", 0x10, 0, false).matched_);
        }
    }
    void errorsAndDates()
    {
        FileColorRules Rules;
        QVERIFY(!Rules.Load(document({{"type", "pattern"}, {"syntax", "regex"}, {"op", "matches"}, {"value", "("}})).isEmpty());
        QVERIFY(!Rules.HasRules());
        auto Disabled = document({{"type", "pattern"}, {"syntax", "regex"}, {"op", "matches"}, {"value", "("}});
        auto DisabledGroup = Disabled["groups"].toArray()[0].toObject(); DisabledGroup["enabled"] = false; Disabled["groups"] = QJsonArray{DisabledGroup};
        QVERIFY(!Rules.Load(Disabled).isEmpty());
        for (const auto& Op : {"matches", "notMatches"}) {
            QVERIFY(Rules.Load(document({{"type", "pattern"}, {"syntax", "regex"}, {"op", Op}, {"value", "(a+)+$"}})).isEmpty());
            const auto Result = Rules.Evaluate(QString(500, 'a') + '!', 0, 0, false);
            QVERIFY(!Result.matched_); QVERIFY(!Result.error_.isEmpty());
        }
        auto Doc = document({{"type", "extension"}, {"op", "in"}, {"value", "txt"}});
        auto Group = Doc["groups"].toArray()[0].toObject(); Group["join"] = "any";
        Group["conditions"] = QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}},
            QJsonObject{{"type", "modified"}, {"op", "within"}, {"unit", "minutes"}, {"value", "1"}}};
        Doc["groups"] = QJsonArray{Group}; QVERIFY(Rules.Load(Doc).isEmpty());
        FileRuleInput Input{"a.txt"}; Input.referenceUtcMs_ = 100000; Input.modifiedUtcMs_ = 100000;
        QCOMPARE(Rules.Evaluate(Input).validUntilUtcMs_, std::optional<qint64>(160001));
        Input.modifiedUtcMs_ = 200000; QCOMPARE(Rules.Evaluate(Input).validUntilUtcMs_, std::optional<qint64>(200000));
    }
    void extensionCache()
    {
        FileColorRules Rules; FileColorRules::EvaluationContext Context;
        QVERIFY(Rules.Load(document({{"type", "extension"}, {"syntax", "regex"}, {"op", "matches"}, {"value", "(?-i)TXT"}})).isEmpty());
        QVERIFY(Rules.Evaluate(FileRuleInput{"a.TXT"}, &Context).matched_);
        QVERIFY(!Rules.Evaluate(FileRuleInput{"a.txt"}, &Context).matched_);
        QVERIFY(Rules.Evaluate(FileRuleInput{"b.TXT"}, &Context).matched_); QCOMPARE(Context.hits_, quint64(1));
        QVERIFY(Rules.Load(document({{"type", "extension"}, {"op", "in"}, {"value", "cpp"}})).isEmpty());
        QVERIFY(!Rules.Evaluate(FileRuleInput{"a.TXT"}, &Context).matched_);
        for (int I = 0; I < 5000; ++I) Rules.Evaluate(FileRuleInput{QString("file.ext%1").arg(I)}, &Context);
        QVERIFY(Context.extensions_.size() <= 4096);
        auto Doc = document({{"type", "extension"}, {"op", "in"}, {"value", "txt"}});
        Rules.Load(Doc); Rules.Evaluate(FileRuleInput{"a.txt"}, &Context); const auto Revision = Context.revision_;
        auto G = Doc["groups"].toArray()[0].toObject(); G["light"] = QJsonObject{{"foreground", "#AA4422"}}; Doc["groups"] = QJsonArray{G};
        Rules.Load(Doc); QCOMPARE(Rules.Evaluate(FileRuleInput{"a.txt"}, &Context).foreground_, QColor("#AA4422")); QCOMPARE(Context.revision_, Revision);
    }
    void regexExecutionLimit()
    {
        const QRegularExpression Pattern("(*LIMIT_MATCH=1000)(a+)+$", QRegularExpression::CaseInsensitiveOption);
        QVERIFY2(Pattern.isValid(), qPrintable(Pattern.errorString()));
        Pattern.optimize();
        const auto Match = Pattern.match(QString(500, 'a') + '!');
        QVERIFY(!Match.isValid());
        QVERIFY(!Match.hasMatch());
        const QRegularExpression Override("(*LIMIT_MATCH=1000)(*LIMIT_MATCH=100000000)(a+)+$");
        QVERIFY(Override.isValid());
        QElapsedTimer Clock; Clock.start();
        QVERIFY(!Override.match(QString(500, 'a') + '!').isValid());
        QVERIFY(Clock.elapsed() < 1000);
    }
    void baselineGlobSemantics()
    {
        const QStringList Patterns{"[", "[]", "[!]", "[a-c]", "[^a-c]", "[!a-c]", "[?]", "a**b", "*.*", "?"};
        for (const auto& Text : Patterns) {
            const auto Regex = QRegularExpression::fromWildcard(Text);
            qInfo().noquote() << Text << Regex.pattern() << Regex.isValid();
        }
        QVERIFY(QRegularExpression::fromWildcard(QStringLiteral("?")).match(QString::fromUcs4(U"😀")).hasMatch());
        QVERIFY(!QRegularExpression::fromWildcard(QStringLiteral("*.*")).match("README").hasMatch());
    }
    void extensionBenchmark()
    {
        QJsonArray Groups;
        for (int I = 0; I < 64; ++I) Groups.append(QJsonObject{{"id", QString::number(I)}, {"name", QString::number(I)},
            {"join", "all"}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", QString("ext%1").arg(I)}}}},
            {"light", QJsonObject{{"foreground", "#2266AA"}}}, {"autoDark", true}});
        FileColorRules Rules; Rules.Load({{"version", 2}, {"mode", "first"}, {"groups", Groups}});
        QList<FileRuleInput> Inputs;
        for (int I = 0; I < 10000; ++I) Inputs.append(FileRuleInput{QString("file_%1.ext%2").arg(I).arg(I % 80), 0, 123, true});
        QList<qint64> Samples;
        for (int Run = 0; Run < 5; ++Run) {
            int Hits = 0; QElapsedTimer Clock; Clock.start();
            for (const auto& Input : Inputs) Hits += Rules.Evaluate(Input).matched_;
            Samples.append(Clock.nsecsElapsed()); QCOMPARE(Hits, 8000);
        }
        std::sort(Samples.begin(), Samples.end());
        qInfo() << QStringLiteral("10000 rows / 64 groups, median ms:") << Samples[2] / 1e6 << QStringLiteral("samples ns:") << Samples;
        Samples.clear();
        for (int Run = 0; Run < 5; ++Run) {
            FileColorRules::EvaluationContext Context; int Hits = 0; QElapsedTimer Clock; Clock.start();
            for (const auto& Input : Inputs) Hits += Rules.EvaluateStyle(Input, &Context).matched_;
            Samples.append(Clock.nsecsElapsed()); QCOMPARE(Hits, 8000); QCOMPARE(Context.misses_, quint64(80)); QCOMPARE(Context.hits_, quint64(9920));
        }
        std::sort(Samples.begin(), Samples.end());
        qInfo() << QStringLiteral("10000 rows / 64 groups with extension cache, median ms:") << Samples[2] / 1e6 << QStringLiteral("samples ns:") << Samples;
    }
private:
    static QJsonObject document(const QJsonObject& Condition) {
        const QJsonObject Group{{"id", "test"}, {"name", "시험"}, {"join", "all"},
            {"conditions", QJsonArray{Condition}}, {"light", QJsonObject{{"foreground", "#2266AA"}}}};
        return {{"version", 3}, {"mode", "first"}, {"groups", QJsonArray{Group}}};
    }
};
QTEST_MAIN(FileColorRuleTests)
#include "tstFileColorRules.moc"
