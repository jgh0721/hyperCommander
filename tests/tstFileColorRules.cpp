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
    void globRunsMatchReference()
    {
        // Star-only patterns skip the DP; '?' patterns still take it. Both must
        // agree with an independent DP over case-folded code points.
        const QString Kelvin = QString(QChar(0x212a)), Smile = QString::fromUcs4(U"😀"), High = QString(QChar(0xd800)), Low = QString(QChar(0xdc00));
        QStringList Patterns{"*", "**", "*.*", "*.txt", "*.TXT", "~$*", "*draft*", "a*b*c", "ab*ba", "*aa*aa*", "*a*a*a*", "a*", "*a", Kelvin + "*",
            "*ς", "*Σ*", "ẞ*", "*" + Smile, Smile + "*" + Smile, "*/*", "a/*", "*\\", "*.tar.gz", "*.한글", "?*", "*?", "a?b*", "*?.txt", "*" + High, Low + "*"};
        QStringList Names{"", "a", "A", "ab", "aba", "abba", "aaa", "aaaa", "abc", "aXbYc", "acb", "x.txt", "X.TXT", "x.txt.bak", "~$doc.docx", "~doc",
            "my_draft_v2", "DRAFT", "k", "K", Kelvin, "σ", "ς", "Σ", "ß", "ss", "ẞ", Smile, Smile + "x" + Smile, Smile + Smile, "a" + High, Low + "a",
            High + Low, "x" + High, "a/b", "a\\b", "/", "x.tar.gz", "가.한글", "a.b.c"};
        QRandomGenerator Random(20261028);
        const QStringList PatternParts{"a", "A", "b", Kelvin, "*", "*", "?", "/", Smile}, NameParts{"a", "A", "b", "B", "k", Kelvin, "/", ".", Smile, High, Low};
        const auto Compose = [&](const QStringList& Parts, int Longest) {
            QString Text; for (int I = int(Random.bounded(Longest + 1)); I > 0; --I) Text += Parts[Random.bounded(int(Parts.size()))];
            return Text;
        };
        for (int I = 0; I < 400; ++I) Patterns.append(Compose(PatternParts, 6));
        for (int I = 0; I < 200; ++I) Names.append(Compose(NameParts, 8));
        for (const auto& Pattern : Patterns) {
            hc::text::FileNameGlob Glob; QVERIFY(Glob.Compile(Pattern).isEmpty());
            for (const auto& Name : Names)
                QVERIFY2(Glob.Matches(Name) == referenceGlob(Pattern, Name), qPrintable(QString("pattern=%1 name=%2").arg(Pattern, Name)));
        }
    }
    void suffixMatchesFileInfo()
    {
        const QStringList Names{"", ".", "..", "...", "a", "a.", ".a", "..a", "a..", "a..b", "a.b.c", ".bashrc", ".tar.gz", "a/b.c", "a\\b.c",
            "a.b/c", "a.b\\c", "a.b/", "/a.b", "C:a.txt", "C:", "a:b.c", "a.b:c", "a.txt ", "a. b", " .txt", "file.tar.gz", "\\\\server\\share.x",
            "가.나다", QString::fromUcs4(U"😀.😀"), QString("a.") + QChar(0xd800), QString(QChar(0xdc00)) + ".b", "a.b::$DATA"};
        for (const auto& Name : Names) QCOMPARE(FileColorRules::Suffix(Name).toString(), QFileInfo(Name).suffix());
        FileColorRules Rules; QVERIFY(Rules.Load(document({{"type", "link"}, {"op", "is"}, {"value", "shortcut"}})).isEmpty());
        QVERIFY(Rules.Matches(FileRuleInput{"a.LNK"}).matched_); QVERIFY(Rules.Matches(FileRuleInput{"a.Url"}).matched_);
        QVERIFY(!Rules.Matches(FileRuleInput{"a.lnk.txt"}).matched_); QVERIFY(!Rules.Matches(FileRuleInput{"a.lnk", 0x10}).matched_);
    }
    void styleComposition()
    {
        // Effects outside the six keys never reach a result.
        const auto Group = [](const QString& Id, const QJsonObject& Font) {
            QJsonObject G{{"id", Id}, {"name", Id}, {"join", "all"}, {"light", QJsonObject{{"foreground", "#2266AA"}}},
                {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}}}}};
            if (!Font.isEmpty()) G["font"] = Font;
            return G;
        };
        const QJsonArray Groups{Group("plain", {}), Group("first", {{"italic", true}, {"size", 9}}),
            Group("second", {{"bold", true}, {"italic", false}, {"border", "dot"}}), Group("third", {{"underline", true}, {"bold", false}})};
        FileColorRules Rules; QVERIFY(Rules.Load({{"version", 3}, {"mode", "first"}, {"groups", QJsonArray{Groups[1]}}}).isEmpty());
        FileColorRules::EvaluationContext Context;
        QCOMPARE(Rules.EvaluateStyle(FileRuleInput{"a.txt"}, &Context).font_, (QJsonObject{{"italic", true}}));
        QVERIFY(Rules.Load({{"version", 3}, {"mode", "merge"}, {"groups", Groups}}).isEmpty());
        const QJsonObject Expected{{"italic", true}, {"bold", true}, {"border", "dot"}, {"underline", true}};
        for (int Pass = 0; Pass < 2; ++Pass) QCOMPARE(Rules.EvaluateStyle(FileRuleInput{"a.txt"}, &Context).font_, Expected);
        const auto Diagnosed = Rules.Evaluate(FileRuleInput{"a.txt"});
        QCOMPARE(Diagnosed.font_, Expected); QCOMPARE(Diagnosed.groups_, (QStringList{"plain", "first", "second", "third"}));
        QCOMPARE(Diagnosed.owners_, (QMap<QString, QString>{{"foreground", "plain"}, {"italic", "first"}, {"bold", "second"}, {"border", "second"}, {"underline", "third"}}));
        QVERIFY(Rules.Load({{"version", 3}, {"mode", "merge"}, {"groups", QJsonArray{Groups[0], Groups[3]}}}).isEmpty());
        QCOMPARE(Rules.EvaluateStyle(FileRuleInput{"a.txt"}, &Context).font_, (QJsonObject{{"underline", true}, {"bold", false}}));
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
    void wildcardBenchmark()
    {
        QJsonArray Groups;
        for (int I = 0; I < 16; ++I) Groups.append(QJsonObject{{"id", QString::number(I)}, {"name", QString::number(I)}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "pattern"}, {"syntax", "wildcard"}, {"op", "matches"}, {"value", QJsonArray{QString("*.ext%1").arg(I)}}}}},
            {"light", QJsonObject{{"foreground", "#2266AA"}}}, {"font", QJsonObject{{"bold", true}}}});
        FileColorRules Rules; QVERIFY(Rules.Load({{"version", 3}, {"mode", "first"}, {"groups", Groups}}).isEmpty());
        QList<FileRuleInput> Inputs;
        for (int I = 0; I < 10000; ++I) Inputs.append(FileRuleInput{QString("file_%1.ext%2").arg(I).arg(I % 80), 0, 123, true});
        QList<qint64> Samples;
        for (int Run = 0; Run < 5; ++Run) {
            FileColorRules::EvaluationContext Context; int Hits = 0; QElapsedTimer Clock; Clock.start();
            for (const auto& Input : Inputs) Hits += Rules.EvaluateStyle(Input, &Context).matched_;
            Samples.append(Clock.nsecsElapsed()); QCOMPARE(Hits, 2000);
        }
        std::sort(Samples.begin(), Samples.end());
        qInfo() << QStringLiteral("10000 rows / 16 wildcard groups with font, median ms:") << Samples[2] / 1e6 << QStringLiteral("samples ns:") << Samples;
    }
private:
    // Literals, '?' and '*' only. Suffix DP: Can[I][J] says Name[I..] matches
    // Tokens[J..]. Unpaired surrogates in a name stay code points, as in the
    // matcher; the pattern side goes through toUcs4() like Compile().
    static bool referenceGlob(const QString& Pattern, const QString& Name) {
        QList<char32_t> Tokens, Text;
        for (const auto Point : Pattern.toUcs4())
            if (Point != '*' || Tokens.isEmpty() || Tokens.last() != '*') Tokens.append(Point == '*' || Point == '?' ? Point : QChar::toCaseFolded(Point));
        for (qsizetype At = 0; At < Name.size(); ++At) {
            char32_t Point = Name[At].unicode();
            if (Name[At].isHighSurrogate() && At + 1 < Name.size() && Name[At + 1].isLowSurrogate()) { Point = QChar::surrogateToUcs4(Name[At], Name[At + 1]); ++At; }
            Text.append(Point);
        }
        const auto Free = [](char32_t Point) { return Point != '/' && Point != '\\'; };
        std::vector<std::vector<char>> Can(Text.size() + 1, std::vector<char>(Tokens.size() + 1, 0));
        Can[Text.size()][Tokens.size()] = 1;
        for (qsizetype I = Text.size(); I >= 0; --I) for (qsizetype J = Tokens.size() - 1; J >= 0; --J) {
            if (Tokens[J] == '*') Can[I][J] = Can[I][J + 1] || (I < Text.size() && Free(Text[I]) && Can[I + 1][J]);
            else if (I < Text.size()) Can[I][J] = (Tokens[J] == '?' ? Free(Text[I]) : QChar::toCaseFolded(Text[I]) == Tokens[J]) && Can[I + 1][J + 1];
        }
        return Can[0][0];
    }
private slots:
    void fieldConditionsAndPending()
    {
        using hc::columns::CellValue; using hc::columns::ValueState;
        const auto Field = [](const QString& Id, const QString& Type, const QString& Op, const QJsonValue& Value) {
            return QJsonObject{{"type", "field"}, {"field", Id}, {"valueType", Type}, {"op", Op}, {"value", Value}};
        };
        const auto Group = [](const QString& Id, bool All, const QJsonArray& Conditions, const QString& Color) {
            return QJsonObject{{"id", Id}, {"name", Id}, {"join", All ? "all" : "any"}, {"conditions", Conditions}, {"light", QJsonObject{{"foreground", Color}}}};
        };
        const QJsonObject Images{{"type", "extension"}, {"op", "in"}, {"value", "jpg, png"}};
        const auto Wide = Group("wide", true, {Field("file.imageWidth", "unsigned", "ge", "3840"), Images}, "#AA0000");
        const auto Jpeg = Group("jpeg", true, {QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "jpg"}}}, "#0000AA");
        QJsonObject Doc{{"version", 4}, {"mode", "first"}, {"groups", QJsonArray{Wide, Jpeg}}};
        auto V3 = Doc; V3["version"] = 3; QVERIFY(!FileColorRules::Validate(V3).isEmpty());
        FileColorRules Rules; const auto Error = Rules.Load(Doc); QVERIFY2(Error.isEmpty(), qPrintable(Error));
        QCOMPARE(Rules.RequiredFields(), QStringList{"file.imageWidth"});

        // The saved order puts the width first; the extension still decides a text file without it.
        auto Text = Rules.Evaluate(FileRuleInput{"notes.txt"});
        QVERIFY(!Text.pending_); QVERIFY(!Text.matched_); QCOMPARE(Text.neededSlots_, quint64(0));
        // A pending group above hides the lower JPEG group in first mode, with the pending style.
        FileRuleInput Photo{"photo.jpg"};
        auto Waiting = Rules.Evaluate(Photo);
        QVERIFY(Waiting.pending_); QVERIFY(!Waiting.matched_); QCOMPARE(Waiting.neededSlots_, quint64(1));
        QCOMPARE(Waiting.background_, QColor("#F3F1EC")); QCOMPARE(Waiting.marker_, QColor("#B7791F")); QVERIFY(!Waiting.foreground_.isValid());
        Photo.dark_ = true; QCOMPARE(Rules.Evaluate(Photo).background_, QColor("#2A2924")); QCOMPARE(Rules.Evaluate(Photo).marker_, QColor("#D9A44A")); Photo.dark_ = false;
        // Arrival changes the cache key and decides the row.
        const auto PendingKey = FileRuleKey::From(Photo);
        Photo.fields_ = {CellValue::Ready(quint64(4032))};
        QVERIFY(!(FileRuleKey::From(Photo) == PendingKey));
        auto Decided = Rules.Evaluate(Photo); QVERIFY(!Decided.pending_); QCOMPARE(Decided.foreground_, QColor("#AA0000"));
        Photo.fields_ = {CellValue::Ready(quint64(1920))};
        QCOMPARE(Rules.Evaluate(Photo).foreground_, QColor("#0000AA"));
        Photo.fields_ = {CellValue::Missing(ValueState::Unsupported)};
        Decided = Rules.Evaluate(Photo); QVERIFY(!Decided.pending_); QCOMPARE(Decided.foreground_, QColor("#0000AA"));
        // One evaluation context serves a whole listing: a pending outcome must not be filed under
        // the extension as decided for the next file of the same type.
        {
            FileColorRules::EvaluationContext Context; FileRuleInput First{"a.jpg"}, Second{"b.jpg"};
            Second.fields_ = {CellValue::Ready(quint64(4032))};
            QVERIFY(Rules.Evaluate(First, &Context).pending_);
            const auto Shared = Rules.Evaluate(Second, &Context); QVERIFY(!Shared.pending_); QCOMPARE(Shared.foreground_, QColor("#AA0000"));
            QVERIFY(Rules.Evaluate(First, &Context).pending_);
        }
        // Merge mode keeps the whole row pending while any group is undecided.
        Doc["mode"] = "merge"; QVERIFY(Rules.Load(Doc).isEmpty());
        QVERIFY(Rules.Evaluate(FileRuleInput{"photo.jpg"}).pending_);

        // Cost tiers: a plugin value is requested only after the slow built-in one could not decide.
        const auto Plugin = QStringLiteral("wdx.0123456789abcdef0123456789abcdef.1.0");
        const auto Tiered = Group("tiered", true, {Field(Plugin, "text", "contains", "Canon"), Field("file.alternateDataStream.count", "unsigned", "ge", "1")}, "#00AA00");
        Doc = {{"version", 4}, {"mode", "first"}, {"groups", QJsonArray{Tiered}}}; QVERIFY(Rules.Load(Doc).isEmpty());
        const int Streams = int(Rules.RequiredFields().indexOf("file.alternateDataStream.count")), Model = int(Rules.RequiredFields().indexOf(Plugin));
        QVERIFY(Streams >= 0 && Model >= 0);
        FileRuleInput Raw{"raw.cr3"};
        QCOMPARE(Rules.Evaluate(Raw).neededSlots_, quint64(1) << Streams);
        Raw.fields_.resize(2); Raw.fields_[Streams] = CellValue::Ready(quint64(0)); Raw.fields_[Model] = CellValue::Missing(ValueState::Pending);
        auto NoStreams = Rules.Evaluate(Raw); QVERIFY(!NoStreams.pending_); QVERIFY(!NoStreams.matched_);
        Raw.fields_[Streams] = CellValue::Ready(quint64(2));
        QCOMPARE(Rules.Evaluate(Raw).neededSlots_, quint64(1) << Model);
        Raw.fields_[Model] = CellValue::Ready(QString("Canon EOS R5"));
        QVERIFY(Rules.Evaluate(Raw).matched_);

        // Document colors override the defaults; documents without field conditions keep v3 keys.
        Doc["pending"] = QJsonObject{{"light", QJsonObject{{"background", "#EEEEEE"}}}}; QVERIFY(Rules.Load(Doc).isEmpty());
        QCOMPARE(Rules.Evaluate(FileRuleInput{"raw.cr3"}).background_, QColor("#EEEEEE"));
        QCOMPARE(Rules.Evaluate(FileRuleInput{"raw.cr3"}).marker_, QColor("#B7791F"));
        Doc["pending"] = QJsonObject{{"light", QJsonObject{{"background", "blue-ish"}}}}; QVERIFY(!FileColorRules::Validate(Doc).isEmpty());
        QVERIFY(Rules.Load(document({{"type", "extension"}, {"op", "in"}, {"value", "txt"}})).isEmpty());
        QVERIFY(Rules.RequiredFields().isEmpty()); QCOMPARE(FileRuleKey::From(FileRuleInput{"a.txt"}).fields_, quint64(0));
        // Lua outputs are profile-scoped and rejected (design 13.1).
        Doc = {{"version", 4}, {"mode", "first"}, {"groups", QJsonArray{Group("lua", true, {Field("lua.0123456789abcdef0123456789abcdef.area", "integer", "ge", "1")}, "#000000")}}};
        QVERIFY(FileColorRules::Validate(Doc).contains("Lua"));
    }
    void fieldConditionEditor()
    {
        QTableWidget Table(0, 4);
        hc::ui::FileColorConditionRow Row(&Table, QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}}, 4);
        auto* Field = qobject_cast<QComboBox*>(Table.cellWidget(0, 0)); auto* Operator = qobject_cast<QComboBox*>(Table.cellWidget(0, 1));
        // Fields and built-in types share one picker; duplicates of built-in types are stored as those types.
        QVERIFY(Field->findData("file.imageWidth") >= 0); QVERIFY(Field->findData("extension") >= 0);
        QCOMPARE(Field->findData("file.extension"), -1); QCOMPARE(Field->findData("file.size"), -1);
        Field->setCurrentIndex(Field->findData("file.imageWidth"));
        auto Record = Row.Record();
        QCOMPARE(Record["type"].toString(), QString("field")); QCOMPARE(Record["valueType"].toString(), QString("unsigned"));
        QCOMPARE(Record["op"].toString(), QString("ge")); QVERIFY(Table.cellWidget(0, 2)->findChild<QLabel*>("hc.condition.unitLabel"));
        hc::columns::FieldCondition Compiled; QVERIFY(hc::columns::CompileFieldCondition(Record, Compiled).isEmpty());
        Operator->setCurrentIndex(Operator->findData("between"));
        QCOMPARE(Row.Record()["value"].toArray().size(), 2);
        Table.cellWidget(0, 2)->findChild<QLineEdit*>("hc.condition.valueHigh")->setText("8000");
        QCOMPARE(Row.Record()["value"].toArray()[1].toString(), QString("8000"));
        // Bytes take a unit; choices take a checkable menu; ADS names only offer substring tests.
        Field->setCurrentIndex(Field->findData("file.alternateDataStream.totalSize"));
        QCOMPARE(Row.Record()["unit"].toString(), QString("MiB")); QVERIFY(Table.cellWidget(0, 2)->findChild<QComboBox*>("hc.condition.unit"));
        Field->setCurrentIndex(Field->findData("file.alternateDataStream.zone"));
        QCOMPARE(Row.Record()["op"].toString(), QString("in")); QCOMPARE(Row.Record()["value"].toArray(), QJsonArray{"0"});
        auto* Choices = Table.cellWidget(0, 2)->findChild<QToolButton*>("hc.condition.choices"); QVERIFY(Choices);
        Choices->menu()->actions()[3]->setChecked(true); Choices->menu()->actions()[0]->setChecked(false);
        QCOMPARE(Row.Record()["value"].toArray(), QJsonArray{"3"});
        Field->setCurrentIndex(Field->findData("file.alternateDataStream.names"));
        QCOMPARE(Operator->count(), 2); QCOMPARE(Row.Record()["value"].toString(), QString("Zone.Identifier"));
        // Text wildcards over paths; switching back to a built-in type drops the field keys.
        Field->setCurrentIndex(Field->findData("file.junction.target"));
        Operator->setCurrentIndex(Operator->findData("globMatches"));
        Table.cellWidget(0, 2)->findChild<QLineEdit*>("hc.condition.value")->setText("\\\\nas\\*; D:\\a\\;b");
        QCOMPARE(Row.Record()["syntax"].toString(), QString("wildcard")); QCOMPARE(Row.Record()["value"].toArray(), QJsonArray({"\\\\nas\\*", "D:\\a;b"}));
        Field->setCurrentIndex(Field->findData("size"));
        QCOMPARE(Row.Record()["type"].toString(), QString("size")); QVERIFY(!Row.Record().contains("field")); QVERIFY(!Row.Record().contains("valueType"));
        // Lua is listed but cannot be chosen; a field the catalog lacks is preserved and labelled.
        const auto Lua = Field->findData("lua"); QVERIFY(Lua >= 0);
        QVERIFY(!qobject_cast<QStandardItemModel*>(Field->model())->item(Lua)->isEnabled());
        const QJsonObject Missing{{"type", "field"}, {"field", "wdx.0123456789abcdef0123456789abcdef.2.0"}, {"valueType", "text"}, {"op", "contains"}, {"value", "x"}, {"fieldTitle", "Exif / Model"}};
        QTableWidget Other(0, 4); hc::ui::FileColorConditionRow Kept(&Other, Missing, 4);
        auto* KeptField = qobject_cast<QComboBox*>(Other.cellWidget(0, 0));
        QVERIFY(KeptField->currentText().contains("Exif / Model")); QCOMPARE(Kept.Record(), Missing);
    }
    void selectionFieldVerdicts()
    {
        using namespace hc::selection; using hc::columns::CellValue; using hc::columns::Truth;
        Criteria C; C.advanced_ = true; C.all_ = true; C.pattern_ = "*.jpg"; C.syntax_ = Syntax::Wildcard; C.scope_ = Scope::Files;
        C.conditions_ = QJsonArray{QJsonObject{{"type", "field"}, {"field", "file.imageWidth"}, {"valueType", "unsigned"}, {"op", "ge"}, {"value", "3840"}}};
        Matcher Match; QVERIFY(Match.Compile(C).isEmpty());
        QCOMPARE(Match.RequiredFields(), QStringList{"file.imageWidth"});
        // The name decides a text file without reading a width; a JPEG waits for it.
        QCOMPARE(Match.Evaluate(FileRuleInput{"notes.txt"}, {}).truth_, Truth::False);
        const auto Waiting = Match.Evaluate(FileRuleInput{"a.jpg"}, {});
        QCOMPARE(Waiting.truth_, Truth::Unknown); QCOMPARE(Waiting.needed_, QStringList{"file.imageWidth"});
        QCOMPARE(Match.Evaluate(FileRuleInput{"a.jpg"}, {{"file.imageWidth", CellValue::Ready(quint64(4032))}}).truth_, Truth::True);
        QCOMPARE(Match.Evaluate(FileRuleInput{"a.jpg"}, {{"file.imageWidth", CellValue::Ready(quint64(1920))}}).truth_, Truth::False);
        QVERIFY(!Match.Matches(FileRuleInput{"a.jpg"})); // A pending verdict never selects.
        // OR: a matching name decides without the field.
        C.all_ = false; QVERIFY(Match.Compile(C).isEmpty());
        QCOMPARE(Match.Evaluate(FileRuleInput{"a.jpg"}, {}).truth_, Truth::True);
        QCOMPARE(Match.Evaluate(FileRuleInput{"a.png"}, {}).truth_, Truth::Unknown);
        // Saved criteria round-trip their conditions; older saves have none.
        QCOMPARE(Criteria::Load(C.Save()).conditions_, C.conditions_);
        QVERIFY(Criteria::Load(QJsonObject{{"pattern", "*"}}).conditions_.isEmpty());
        QVERIFY(!Criteria::Load(QJsonObject{{"pattern", "*"}}).Save().contains("conditions"));
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
