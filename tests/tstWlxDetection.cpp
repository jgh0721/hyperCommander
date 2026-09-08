#include <QtTest>
#include <random>
#include "pluginHost/wlxDetection.hpp"

using namespace hc::plugins;
class WlxDetectionTests : public QObject
{
    Q_OBJECT
private slots:
    void expressions_data()
    {
        QTest::addColumn<QByteArray>("Expression"); QTest::addColumn<QByteArray>("Extension");
        QTest::addColumn<QByteArray>("Bytes"); QTest::addColumn<quint64>("Size");
        QTest::addColumn<bool>("Force"); QTest::addColumn<int>("Expected"); QTest::addColumn<bool>("Multimedia");
        const auto Row = [](const char* Name, QByteArray Expression, QByteArray Extension, QByteArray Bytes, quint64 Size, bool Force, int Expected, bool Multimedia = false) {
            QTest::newRow(Name) << Expression << Extension << Bytes << Size << Force << Expected << Multimedia;
        };
        Row("extensions", "EXT=\"WAV\" | EXT=\"AVI\"", "avi", {}, 0, false, 1);
        Row("extension-case-literal", "EXT=\"txt\"", "txt", {}, 0, false, 0);
        Row("wave-header", "EXT=\"WAV\" & [0]=\"R\" & [1]=\"I\" & [2]=\"F\" & [3]=\"F\" & FIND(\"WAVEfmt\")", "wav", "RIFF....WAVEfmt", 15, false, 1);
        Row("size-force-small", "EXT=\"WAV\" & (SIZE<1000000 | FORCE)", "wav", {}, 999999, false, 1);
        Row("size-force-large", "EXT=\"WAV\" & (SIZE<1000000 | FORCE)", "wav", {}, 1000000, false, 0);
        Row("size-forced", "EXT=\"WAV\" & (SIZE<1000000 | FORCE)", "wav", {}, 1000000, true, 1);
        Row("zip", "([0]=\"P\" & [1]=\"K\" & [2]=3 & [3]=4) | ([0]=\"P\" & [1]=\"K\" & [2]=7 & [3]=8)", "zip", QByteArray::fromHex("504b0304"), 4, false, 1);
        Row("split-zip", "([0]=\"P\" & [1]=\"K\" & [2]=3 & [3]=4) | ([0]=\"P\" & [1]=\"K\" & [2]=7 & [3]=8)", "zip", QByteArray::fromHex("504b0708"), 4, false, 1);
        Row("html-excluded", "EXT=\"TXT\" & !(FINDI(\"<HEAD>\") | FINDI(\"<BODY>\"))", "txt", "<bOdY>abc", 9, false, 0);
        Row("text-accepted", "EXT=\"TXT\" & !(FINDI(\"<HEAD>\") | FINDI(\"<BODY>\"))", "txt", "plain", 5, false, 1);
        Row("multimedia", "MULTIMEDIA & (EXT=\"WAV\" | EXT=\"MP3\")", "mp3", {}, 0, false, 1, true);
        Row("multimedia-false", "0 & MULTIMEDIA", {}, {}, 0, false, 0, true);
        Row("multimedia-literal", "EXT=\"MULTIMEDIA\"", "multimedia", {}, 0, false, 1);
        Row("precedence-or", "1 | 0 & 0", {}, {}, 0, false, 1);
        Row("precedence-compare", "0 = 0 & 2 > 1 | 1 != 1", {}, {}, 0, false, 1);
        Row("parentheses", "(1 | 0) & 0", {}, {}, 0, false, 0);
        Row("nested-not", "!(!(SIZE>0))", {}, {}, 3, false, 1);
        Row("single-char-number", "\"A\"=65 & 65=\"A\"", {}, {}, 0, false, 1);
        Row("mixed-long-string", "65!=\"AA\" | 65<\"AA\" | \"AA\">65", {}, {}, 0, false, 0);
        Row("strings", "\"AB\"<\"AC\" & \"AA\"!=\"AB\" & \"ZZ\">\"AZ\"", {}, {}, 0, false, 1);
        Row("unsigned-byte", "[0]=255", {}, QByteArray::fromHex("ff"), 1, false, 1);
        Row("raw-ansi", QByteArray("[0]=\"") + char(0xe9) + "\"", {}, QByteArray::fromHex("e9"), 1, false, 1);
        Row("binary-nul", "[0]=0 & FINDI(\"abc\")", {}, QByteArray("\0aBc", 4), 4, false, 1);
        Row("find-case-sensitive", "FIND(\"abc\")", {}, "aBc", 3, false, 0);
        Row("find-limit", "FIND(\"XYZ\")", {}, QByteArray(8192, 'a') + "XYZ", 8195, false, 0);
        Row("find-last-byte", "FIND(\"XYZ\") & [8191]=\"Z\"", {}, QByteArray(8189, 'a') + "XYZ", 8192, false, 1);
        Row("find-cross-limit", "FIND(\"XYZ\")", {}, QByteArray(8191, 'a') + "XYZ", 8194, false, 0);
        Row("eof-unknown", "[0]=0", {}, {}, 0, false, -1);
        Row("eof-not-unknown", "!([0]=0)", {}, {}, 0, false, -1);
        Row("eof-or-true", "[1]=0 | EXT=\"TXT\"", "txt", {}, 0, false, 1);
        Row("eof-and-false", "[1]=0 & EXT=\"BIN\"", "txt", {}, 0, false, 0);
        Row("large-size-exact", "SIZE=18446744073709551615 & SIZE>9007199254740992", {}, {}, std::numeric_limits<quint64>::max(), false, 1);
        Row("size-no-rounding", "SIZE=9007199254740992", {}, {}, 9007199254740993ULL, false, 0);
        Row("whitespace", " \t ( EXT = \"TXT\" ) \r\n", "txt", {}, 0, false, 1);
        Row("empty", {}, {}, {}, 0, false, 0);
        Row("empty-search", "FIND(\"\")", {}, {}, 0, false, 1);
        Row("literal-backslash", "FIND(\"a\\b\")", {}, "a\\b", 3, false, 1);
    }
    void expressions()
    {
        QFETCH(QByteArray, Expression); QFETCH(QByteArray, Extension); QFETCH(QByteArray, Bytes); QFETCH(quint64, Size);
        QFETCH(bool, Force); QFETCH(int, Expected); QFETCH(bool, Multimedia);
        auto Parsed = WlxDetectionExpression::Parse(Expression); QVERIFY(Parsed.HasValue());
        auto Result = Parsed.Value().Evaluate({Extension, Bytes, Size, Force});
        QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        QCOMPARE(Result.Value().matches_ ? int(*Result.Value().matches_) : -1, Expected);
        QCOMPARE(Result.Value().multimedia_, Multimedia);
    }
    void invalidSyntax()
    {
        for (const auto* Source : {"1 |", "& 1", "()", "!1", "!(1", "[8192]=1", "[-1]=0", "[a]=0", "EXT=TXT", "ext=\"TXT\"", "UNKNOWN", "FIND(1)", "FIND(\"a\"", "\"open", "1==1", "1<=1", "1>=1", "1=1=1", "1 + 2", "18446744073709551616", "SIZE=1junk", "1 || 0", "0 && 1", "1 | BAD"})
            QVERIFY2(!WlxDetectionExpression::Parse(Source).HasValue(), Source);
        QVERIFY(!WlxDetectionExpression::Parse(QByteArray("1\0|0", 4)).HasValue());
        QVERIFY(!WlxDetectionExpression::Parse(QByteArray(2048, ' ')).HasValue());
        QVERIFY(!WlxDetectionExpression::Parse(QByteArray(64, '(') + "1" + QByteArray(64, ')')).HasValue());
        QVERIFY(WlxDetectionExpression::Parse(QByteArray(63, '(') + "1" + QByteArray(63, ')')).HasValue());
        QVERIFY(WlxDetectionExpression::Parse(QByteArray(2046, ' ') + "1").HasValue());
        QByteArray Many = "1"; for (int Index = 0; Index < 600; ++Index) Many += "|1";
        QVERIFY(!WlxDetectionExpression::Parse(Many).HasValue());
    }
    void inputContract()
    {
        auto Header = WlxDetectionExpression::Parse("[0]=1"); QVERIFY(Header.HasValue()); QVERIFY(Header.Value().NeedsPrefix()); QVERIFY(!Header.Value().NeedsExtension());
        auto Result = Header.Value().Evaluate({{}, QByteArray(1, char(1)), 2});
        QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().code_, std::string("wlx-detect-prefix-incomplete"));
        auto Extension = WlxDetectionExpression::Parse("EXT=\"TXT\""); QVERIFY(Extension.HasValue()); QVERIFY(!Extension.Value().NeedsPrefix()); QVERIFY(Extension.Value().NeedsExtension());
        QVERIFY(Extension.Value().Evaluate({"txt", {}, 1000000}).HasValue());
        QVERIFY(!Extension.Value().Evaluate({QByteArray("x\0y", 3), {}, 0}).HasValue());
        for (const auto* Source : {"\"ABC\"", "!(\"A\")", "\"A\" & 1", "0 | \"X\""})
        { auto Parsed = WlxDetectionExpression::Parse(Source); QVERIFY(Parsed.HasValue()); QVERIFY(!Parsed.Value().Evaluate({}).HasValue()); }
    }
    void malformedCorpus()
    {
        std::mt19937 Random(0x574c58); const QByteArray Alphabet = "EXTSIZEFORCEMULTIMEDIAFIND0123456789[]()!<>=&|\" \t";
        // Fixed seed: arbitrary syntax must either fail explicitly or produce
        // one bounded result, including malformed operands after decisive OR.
        for (int Case = 0; Case < 5000; ++Case)
        {
            QByteArray Source; const auto Length = Random() % 256;
            for (unsigned Index = 0; Index < Length; ++Index) Source += Alphabet[Random() % Alphabet.size()];
            auto Parsed = WlxDetectionExpression::Parse(Source);
            if (Parsed.HasValue()) { const auto Result = Parsed.Value().Evaluate({"txt", "abc", 3}); Q_UNUSED(Result); }
        }
    }
};
QTEST_GUILESS_MAIN(WlxDetectionTests)
#include "tstWlxDetection.moc"
